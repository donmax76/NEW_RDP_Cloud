#pragma once
#include "host.h"
#include "logger.h"
#include "capture_ipc.h"
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")

// ═══════════════════════════════════════════════════════════════
// Windows Service framework for Session 0 operation
// Spawns capture helper in interactive session
// ═══════════════════════════════════════════════════════════════

#define SERVICE_NAME "Prometey"

// Forward: the main host logic (connect loop, command handler, etc.)
extern void host_main_loop();
extern std::atomic<bool> g_running;

// ── Service globals ──
static SERVICE_STATUS        g_svc_status{};
static SERVICE_STATUS_HANDLE g_svc_status_handle = nullptr;

// ── Capture helper management ──
static HANDLE              g_helper_process = nullptr;
static CaptureIpcReader    g_ipc_reader;
static std::string         g_ipc_name;
static std::thread         g_helper_monitor_thread;
static std::atomic<bool>   g_helper_running{false};
static std::atomic<DWORD>  g_active_session{0xFFFFFFFF};

// ── Get exe path ──
static std::string get_exe_path() {
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return buf;
}

// ── Spawn capture helper in interactive session ──
static bool spawn_capture_helper(DWORD session_id) {
    Logger& log = Logger::get();

    // Kill existing helper first
    if (g_helper_process) {
        g_ipc_reader.set_shutdown();
        WaitForSingleObject(g_helper_process, 3000);
        TerminateProcess(g_helper_process, 0);
        CloseHandle(g_helper_process);
        g_helper_process = nullptr;
    }

    // Get user token for the session
    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(session_id, &hToken)) {
        log.error("WTSQueryUserToken failed for session " + std::to_string(session_id) +
                  ", err=" + std::to_string(GetLastError()));
        return false;
    }

    // Duplicate token as primary
    HANDLE hPrimaryToken = nullptr;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr,
                          SecurityImpersonation, TokenPrimary, &hPrimaryToken)) {
        log.error("DuplicateTokenEx failed, err=" + std::to_string(GetLastError()));
        CloseHandle(hToken);
        return false;
    }
    CloseHandle(hToken);

    // Create/reset IPC
    g_ipc_reader.close();
    g_ipc_name = "rdh_" + std::to_string(GetCurrentProcessId());
    if (!g_ipc_reader.create(g_ipc_name)) {
        log.error("Failed to create IPC shared memory");
        CloseHandle(hPrimaryToken);
        return false;
    }
    g_ipc_reader.set_parent_pid(GetCurrentProcessId());
    g_ipc_reader.set_fps(30);
    g_ipc_reader.set_scale(80);

    // Build command line
    std::string exe = get_exe_path();
    std::string cmdLine = "\"" + exe + "\" --capture-helper " +
                          std::to_string(GetCurrentProcessId()) + " " + g_ipc_name;

    // Launch in user's session on interactive desktop
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.lpDesktop = (LPSTR)"winsta0\\default";
    PROCESS_INFORMATION pi = {};

    // Set environment for the user session
    LPVOID pEnv = nullptr;
    CreateEnvironmentBlock(&pEnv, hPrimaryToken, FALSE);

    DWORD creationFlags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    BOOL ok = CreateProcessAsUserA(
        hPrimaryToken,
        exe.c_str(),
        (LPSTR)cmdLine.c_str(),
        nullptr, nullptr, FALSE,
        creationFlags,
        pEnv,
        nullptr,
        &si, &pi
    );

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hPrimaryToken);

    if (!ok) {
        log.error("CreateProcessAsUser failed, err=" + std::to_string(GetLastError()));
        return false;
    }

    g_helper_process = pi.hProcess;
    CloseHandle(pi.hThread);
    g_active_session = session_id;

    log.info("Capture helper spawned in session " + std::to_string(session_id) +
             ", PID=" + std::to_string(pi.dwProcessId));
    return true;
}

// ── Monitor helper process: restart on crash ──
static void helper_monitor_func() {
    Logger& log = Logger::get();
    int crash_count = 0;
    auto last_crash = std::chrono::steady_clock::now();

    while (g_running && g_helper_running) {
        // Check active session
        DWORD activeSession = WTSGetActiveConsoleSessionId();

        if (activeSession == 0xFFFFFFFF) {
            // No interactive session
            if (g_helper_process) {
                g_ipc_reader.set_shutdown();
                WaitForSingleObject(g_helper_process, 3000);
                TerminateProcess(g_helper_process, 0);
                CloseHandle(g_helper_process);
                g_helper_process = nullptr;
                log.info("No active session, helper terminated");
            }
            Sleep(5000);
            continue;
        }

        // Session changed (fast user switching)
        if (activeSession != g_active_session && g_helper_process) {
            log.info("Session changed " + std::to_string((DWORD)g_active_session) +
                     " -> " + std::to_string(activeSession));
            g_ipc_reader.set_shutdown();
            WaitForSingleObject(g_helper_process, 3000);
            TerminateProcess(g_helper_process, 0);
            CloseHandle(g_helper_process);
            g_helper_process = nullptr;
        }

        // Spawn helper if not running
        if (!g_helper_process) {
            // Rate limit restarts
            auto now = std::chrono::steady_clock::now();
            auto since_crash = std::chrono::duration_cast<std::chrono::seconds>(now - last_crash).count();
            if (since_crash < 30) {
                crash_count++;
                if (crash_count > 5) {
                    log.error("Helper crashed too many times, waiting 30s");
                    Sleep(30000);
                    crash_count = 0;
                    continue;
                }
            } else {
                crash_count = 0;
            }

            if (!spawn_capture_helper(activeSession)) {
                Sleep(5000);
                continue;
            }
            last_crash = now;
        }

        // Wait for helper to exit or timeout (poll every 2s)
        DWORD wait = WaitForSingleObject(g_helper_process, 2000);
        if (wait == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(g_helper_process, &exitCode);
            CloseHandle(g_helper_process);
            g_helper_process = nullptr;
            log.warn("Capture helper exited with code " + std::to_string(exitCode));
            last_crash = std::chrono::steady_clock::now();
            Sleep(1000); // Brief delay before restart
        }
    }

    // Cleanup
    if (g_helper_process) {
        g_ipc_reader.set_shutdown();
        WaitForSingleObject(g_helper_process, 3000);
        TerminateProcess(g_helper_process, 0);
        CloseHandle(g_helper_process);
        g_helper_process = nullptr;
    }
    g_ipc_reader.close();
}

// ── Service control handler ──
static DWORD WINAPI ServiceCtrlHandlerEx(DWORD dwControl, DWORD dwEventType,
                                          LPVOID lpEventData, LPVOID lpContext) {
    switch (dwControl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_svc_status.dwCurrentState = SERVICE_STOP_PENDING;
            SetServiceStatus(g_svc_status_handle, &g_svc_status);
            g_running = false;
            g_helper_running = false;
            return NO_ERROR;

        case SERVICE_CONTROL_SESSIONCHANGE: {
            WTSSESSION_NOTIFICATION* sn = (WTSSESSION_NOTIFICATION*)lpEventData;
            Logger& log = Logger::get();
            switch (dwEventType) {
                case WTS_CONSOLE_CONNECT:
                    log.info("Session event: WTS_CONSOLE_CONNECT session=" +
                             std::to_string(sn ? sn->dwSessionId : 0));
                    break;
                case WTS_CONSOLE_DISCONNECT:
                    log.info("Session event: WTS_CONSOLE_DISCONNECT");
                    break;
                case WTS_SESSION_LOGON:
                    log.info("Session event: WTS_SESSION_LOGON session=" +
                             std::to_string(sn ? sn->dwSessionId : 0));
                    break;
                case WTS_SESSION_LOGOFF:
                    log.info("Session event: WTS_SESSION_LOGOFF");
                    break;
            }
            return NO_ERROR;
        }

        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// ── ServiceMain ──
static VOID WINAPI ServiceMain(DWORD dwArgc, LPSTR* lpszArgv) {
    Logger& log = Logger::get();

    g_svc_status_handle = RegisterServiceCtrlHandlerExA(SERVICE_NAME, ServiceCtrlHandlerEx, nullptr);
    if (!g_svc_status_handle) {
        log.error("RegisterServiceCtrlHandlerEx failed");
        return;
    }

    g_svc_status.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwCurrentState     = SERVICE_START_PENDING;
    g_svc_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
                                       SERVICE_ACCEPT_SESSIONCHANGE;
    g_svc_status.dwWin32ExitCode    = 0;
    g_svc_status.dwCheckPoint       = 1;
    g_svc_status.dwWaitHint         = 10000;
    SetServiceStatus(g_svc_status_handle, &g_svc_status);

    log.info("Service starting...");

    // Start helper monitor thread
    g_helper_running = true;
    g_helper_monitor_thread = std::thread(helper_monitor_func);

    // Report running
    g_svc_status.dwCurrentState = SERVICE_RUNNING;
    g_svc_status.dwCheckPoint   = 0;
    g_svc_status.dwWaitHint     = 0;
    SetServiceStatus(g_svc_status_handle, &g_svc_status);

    log.info("Service running");

    // Run the main host logic (connect loop, command handler, etc.)
    host_main_loop();

    // Cleanup
    g_helper_running = false;
    if (g_helper_monitor_thread.joinable()) g_helper_monitor_thread.join();

    g_svc_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_svc_status_handle, &g_svc_status);
    log.info("Service stopped");
}

// ── Entry point for service mode ──
static int run_as_service() {
    Logger& log = Logger::get();
    log.info("Starting as Windows service...");

    SERVICE_TABLE_ENTRYA dispatchTable[] = {
        { (LPSTR)SERVICE_NAME, ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherA(dispatchTable)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            log.error("Not running as a service. Use --install to register, or run without --service for standalone mode.");
        } else {
            log.error("StartServiceCtrlDispatcher failed, err=" + std::to_string(err));
        }
        return 1;
    }
    return 0;
}

// ── Install service ──
static int install_service() {
    std::string exe = get_exe_path();
    std::string cmdLine = "\"" + exe + "\" --service";

    SC_HANDLE hSCM = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!hSCM) {
        fprintf(stderr, "OpenSCManager failed (run as admin), err=%lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE hSvc = CreateServiceA(
        hSCM,
        SERVICE_NAME,
        "Prometey",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        cmdLine.c_str(),
        nullptr, nullptr, nullptr,
        "LocalSystem",  // Run as SYSTEM
        nullptr
    );

    if (!hSvc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            fprintf(stderr, "Service already exists. Use --uninstall first to reinstall.\n");
        } else {
            fprintf(stderr, "CreateService failed, err=%lu\n", err);
        }
        CloseServiceHandle(hSCM);
        return 1;
    }

    // Set service to restart on failure
    SERVICE_FAILURE_ACTIONSA fa = {};
    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART, 5000 },  // Restart after 5s
        { SC_ACTION_RESTART, 10000 }, // Restart after 10s
        { SC_ACTION_RESTART, 30000 }, // Restart after 30s
    };
    fa.dwResetPeriod = 3600; // Reset failure count after 1 hour
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2A(hSvc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    printf("Service '%s' installed successfully.\n", SERVICE_NAME);
    printf("Start with: net start %s\n", SERVICE_NAME);

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return 0;
}

// ── Uninstall service ──
static int uninstall_service() {
    SC_HANDLE hSCM = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        fprintf(stderr, "OpenSCManager failed (run as admin), err=%lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE hSvc = OpenServiceA(hSCM, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!hSvc) {
        fprintf(stderr, "Service '%s' not found, err=%lu\n", SERVICE_NAME, GetLastError());
        CloseServiceHandle(hSCM);
        return 1;
    }

    // Stop if running
    SERVICE_STATUS status{};
    ControlService(hSvc, SERVICE_CONTROL_STOP, &status);
    Sleep(2000);

    if (!DeleteService(hSvc)) {
        fprintf(stderr, "DeleteService failed, err=%lu\n", GetLastError());
        CloseServiceHandle(hSvc);
        CloseServiceHandle(hSCM);
        return 1;
    }

    printf("Service '%s' uninstalled successfully.\n", SERVICE_NAME);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return 0;
}
