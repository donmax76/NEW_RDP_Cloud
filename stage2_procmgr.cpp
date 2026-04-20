// ═══════════════════════════════════════════════════════════════════════
// stage2_procmgr.cpp — process/service/registry mutation commands.
//
// Handlers registered:
//   proc_kill         — TerminateProcess by PID
//   proc_launch       — CreateProcess or ShellExecute (elevate="admin")
//   term_exec         — cmd.exe /c <line>, captured output
//   svc_control       — start/stop/restart Windows service
//   reg_set_value     — RegSetValueEx (REG_SZ/DWORD/QWORD/BINARY/MULTI_SZ)
//   reg_delete_value  — RegDeleteValue
//   reg_create_key    — RegCreateKeyEx
//   reg_delete_key    — RegDeleteKey
//
// Kept read-only and safe in stage-1: proc_list, svc_list, reg_list.
// Imports: advapi32 (reg/svc), shell32 (ShellExecute), kernel32 (CreateProcess).
// ═══════════════════════════════════════════════════════════════════════

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>

#include "stage2_abi.h"
#include "stage2_util.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

using namespace s2util;
static Stage2HostCtx* g_host = nullptr;

// ── Helpers ─────────────────────────────────────────────────────────────
static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static HKEY parse_root_key(const std::string& s) {
    if (s == "HKLM" || s == "HKEY_LOCAL_MACHINE") return HKEY_LOCAL_MACHINE;
    if (s == "HKCU" || s == "HKEY_CURRENT_USER")  return HKEY_CURRENT_USER;
    if (s == "HKCR" || s == "HKEY_CLASSES_ROOT")  return HKEY_CLASSES_ROOT;
    if (s == "HKU"  || s == "HKEY_USERS")         return HKEY_USERS;
    if (s == "HKCC" || s == "HKEY_CURRENT_CONFIG")return HKEY_CURRENT_CONFIG;
    return nullptr;
}

static bool parse_reg_path(const std::string& full, HKEY& root, std::string& sub) {
    auto pos = full.find('\\');
    std::string rstr = (pos == std::string::npos) ? full : full.substr(0, pos);
    root = parse_root_key(rstr);
    if (!root) return false;
    sub = (pos == std::string::npos) ? "" : full.substr(pos + 1);
    return true;
}

static std::vector<BYTE> hex_to_bytes(const std::string& hex) {
    std::vector<BYTE> r;
    std::string clean; clean.reserve(hex.size());
    for (char c : hex) if (c != ' ' && c != '-') clean += c;
    for (size_t i = 0; i + 1 < clean.size(); i += 2)
        r.push_back((BYTE)strtoul(clean.substr(i, 2).c_str(), nullptr, 16));
    return r;
}

// ── Command handlers ────────────────────────────────────────────────────

static void cmd_proc_kill(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");
    std::string pid_s = json_get(args, "pid");
    if (pid_s.empty()) { g_host->send(make_err(id, "Missing pid").c_str()); return; }
    DWORD pid = (DWORD)std::stoul(pid_s);
    HANDLE ph = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    bool ok = false;
    if (ph) { ok = TerminateProcess(ph, 1) != 0; CloseHandle(ph); }
    g_host->send((ok ? make_ok(id, "\"killed\"")
                     : make_err(id, "Kill failed for pid " + pid_s)).c_str());
}

static void cmd_proc_launch(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");
    std::string exe  = json_get(args, "exe");
    std::string arg  = json_get(args, "args");
    std::string elev = json_get(args, "elevate");
    bool as_admin = (elev == "admin" || elev == "system");
    if (exe.empty()) { g_host->send(make_err(id, "proc_launch: empty exe").c_str()); return; }

    std::wstring wexe = to_wide(exe), warg = to_wide(arg);
    bool ok = false;
    if (as_admin) {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = wexe.c_str();
        sei.lpParameters = warg.empty() ? nullptr : warg.c_str();
        sei.nShow = SW_SHOW;
        ok = ShellExecuteExW(&sei) != 0;
    } else {
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmd_line = wexe;
        if (!warg.empty()) { cmd_line += L" "; cmd_line += warg; }
        ok = CreateProcessW(nullptr, cmd_line.data(),
                            nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &si, &pi) != 0;
        if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    g_host->send((ok ? make_ok(id, "\"launched\"")
                     : make_err(id, "Launch failed: " + exe)).c_str());
}

static void cmd_term_exec(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");
    std::string line = json_get(args, "line");
    if (line.empty()) line = json_get(args, "cmd");
    if (line.empty() || line == "term_exec") {
        g_host->send(make_err(id, "Missing command line").c_str()); return;
    }

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        g_host->send(make_err(id, "CreatePipe failed").c_str()); return;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite; si.hStdError = hWrite;
    PROCESS_INFORMATION pi{};

    std::wstring wcmd(L"cmd.exe /c chcp 65001 >nul & ");
    wcmd += to_wide(line);
    std::vector<wchar_t> cl(wcmd.size() + 1);
    wcscpy_s(cl.data(), cl.size(), wcmd.c_str());

    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        g_host->send(make_err(id, "CreateProcess failed").c_str()); return;
    }
    CloseHandle(hWrite);

    std::string output;
    char buf[4096]; DWORD n = 0;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        buf[n] = 0;
        output.append(buf, n);
    }
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(hRead);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    g_host->send(make_ok(id, "\"" + json_escape(output) + "\"").c_str());
}

static void cmd_svc_control(const char* a, void*) {
    std::string args   = a ? a : "";
    std::string id     = json_get(args, "id");
    std::string name   = json_get(args, "name");
    std::string action = json_get(args, "action");
    if (name.empty() || action.empty()) {
        g_host->send(make_err(id, "svc_control requires name + action").c_str()); return;
    }
    auto wname = to_wide(name);
    SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { g_host->send(make_err(id, "OpenSCManager failed").c_str()); return; }
    SC_HANDLE svc = OpenServiceW(scm, wname.c_str(), SERVICE_ALL_ACCESS);
    if (!svc) { CloseServiceHandle(scm);
        g_host->send(make_err(id, "OpenService failed: " + name).c_str()); return; }

    bool ok = false; SERVICE_STATUS ss{};
    if (action == "start")    ok = StartServiceW(svc, 0, nullptr) != 0;
    else if (action == "stop")ok = ControlService(svc, SERVICE_CONTROL_STOP, &ss) != 0;
    else if (action == "restart") {
        ControlService(svc, SERVICE_CONTROL_STOP, &ss);
        Sleep(2000);
        ok = StartServiceW(svc, 0, nullptr) != 0;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    g_host->send((ok ? make_ok(id, "\"done\"")
                     : make_err(id, "Service " + action + " failed: " + name)).c_str());
}

static void cmd_reg_set_value(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id    = json_get(args, "id");
    std::string path  = json_get(args, "path");
    std::string vname = json_get(args, "name");
    std::string vtype = json_get(args, "type");
    std::string vdata = json_get(args, "data");
    HKEY root; std::string sub;
    if (!parse_reg_path(path, root, sub)) { g_host->send(make_err(id, "Invalid path").c_str()); return; }

    HKEY hKey = nullptr;
    LONG rc = RegCreateKeyExA(root, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                               nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS) {
        g_host->send(make_err(id, "Open/create key failed (err " + std::to_string(rc) + ")").c_str());
        return;
    }
    if (vtype == "REG_SZ" || vtype == "REG_EXPAND_SZ") {
        DWORD t = (vtype == "REG_SZ") ? REG_SZ : REG_EXPAND_SZ;
        rc = RegSetValueExA(hKey, vname.c_str(), 0, t,
                            (BYTE*)vdata.c_str(), (DWORD)vdata.size() + 1);
    } else if (vtype == "REG_DWORD") {
        DWORD val = (DWORD)std::stoul(vdata);
        rc = RegSetValueExA(hKey, vname.c_str(), 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    } else if (vtype == "REG_QWORD") {
        uint64_t val = std::stoull(vdata);
        rc = RegSetValueExA(hKey, vname.c_str(), 0, REG_QWORD, (BYTE*)&val, sizeof(val));
    } else if (vtype == "REG_BINARY") {
        auto bytes = hex_to_bytes(vdata);
        rc = RegSetValueExA(hKey, vname.c_str(), 0, REG_BINARY,
                            bytes.data(), (DWORD)bytes.size());
    } else if (vtype == "REG_MULTI_SZ") {
        std::string multi;
        std::istringstream ss(vdata); std::string line;
        while (std::getline(ss, line)) { multi += line; multi += '\0'; }
        multi += '\0';
        rc = RegSetValueExA(hKey, vname.c_str(), 0, REG_MULTI_SZ,
                            (BYTE*)multi.data(), (DWORD)multi.size());
    } else {
        RegCloseKey(hKey);
        g_host->send(make_err(id, "Unsupported type: " + vtype).c_str()); return;
    }
    RegCloseKey(hKey);
    g_host->send((rc == ERROR_SUCCESS ? make_ok(id, "\"saved\"")
                     : make_err(id, "Write failed (err " + std::to_string(rc) + ")")).c_str());
}

static void cmd_reg_delete_value(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id    = json_get(args, "id");
    std::string path  = json_get(args, "path");
    std::string vname = json_get(args, "name");
    HKEY root; std::string sub;
    if (!parse_reg_path(path, root, sub)) { g_host->send(make_err(id, "Invalid path").c_str()); return; }
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(root, sub.c_str(), 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        g_host->send(make_err(id, "Cannot open key").c_str()); return;
    }
    LONG rc = RegDeleteValueA(hKey, vname.c_str());
    RegCloseKey(hKey);
    g_host->send((rc == ERROR_SUCCESS ? make_ok(id, "\"deleted\"")
                     : make_err(id, "Delete failed (err " + std::to_string(rc) + ")")).c_str());
}

static void cmd_reg_create_key(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");
    std::string path = json_get(args, "path");
    HKEY root; std::string sub;
    if (!parse_reg_path(path, root, sub)) { g_host->send(make_err(id, "Invalid path").c_str()); return; }
    HKEY hKey = nullptr;
    LONG rc = RegCreateKeyExA(root, sub.c_str(), 0, nullptr, 0, KEY_READ,
                               nullptr, &hKey, nullptr);
    if (rc == ERROR_SUCCESS) { RegCloseKey(hKey); g_host->send(make_ok(id, "\"created\"").c_str()); }
    else g_host->send(make_err(id, "Create failed (err " + std::to_string(rc) + ")").c_str());
}

static void cmd_reg_delete_key(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");
    std::string path = json_get(args, "path");
    HKEY root; std::string sub;
    if (!parse_reg_path(path, root, sub)) { g_host->send(make_err(id, "Invalid path").c_str()); return; }
    LONG rc = RegDeleteKeyA(root, sub.c_str());
    g_host->send((rc == ERROR_SUCCESS ? make_ok(id, "\"deleted\"")
                     : make_err(id, "Delete key failed (err " + std::to_string(rc) + ")")).c_str());
}

// ── Entry points ────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) int Stage2Init(Stage2HostCtx* host) {
    if (!host || host->abi_version != STAGE2_ABI_VERSION) return 1;
    g_host = host;
    host->log(1, "stage2_procmgr: init");
    host->register_cmd("proc_kill",        cmd_proc_kill,        nullptr);
    host->register_cmd("proc_launch",      cmd_proc_launch,      nullptr);
    host->register_cmd("term_exec",        cmd_term_exec,        nullptr);
    host->register_cmd("svc_control",      cmd_svc_control,      nullptr);
    host->register_cmd("reg_set_value",    cmd_reg_set_value,    nullptr);
    host->register_cmd("reg_delete_value", cmd_reg_delete_value, nullptr);
    host->register_cmd("reg_create_key",   cmd_reg_create_key,   nullptr);
    host->register_cmd("reg_delete_key",   cmd_reg_delete_key,   nullptr);
    host->log(1, "stage2_procmgr: 8 commands registered");
    return 0;
}

extern "C" __declspec(dllexport) void Stage2Shutdown(void) {
    if (g_host) g_host->log(1, "stage2_procmgr: shutdown");
    g_host = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(nullptr);
    return TRUE;
}
