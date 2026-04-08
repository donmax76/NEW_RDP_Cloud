/*
 * Standalone Capture Helper EXE
 * Spawned by host DLL (Session 0) in user session via CreateProcessAsUser
 * Captures screen + encodes H.264 + writes to IPC shared memory
 *
 * Usage: CaptureHelper.exe <parent_pid> <ipc_name>
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <string>
#include <thread>
#include <chrono>
#include <gdiplus.h>
#include <mmsystem.h>
#include <avrt.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")

#include "capture_ipc.h"
#include "screen_capture.h"
#include "h264_encoder.h"
#include "logger.h"

static void diag(const char* msg) {
    // File logging permanently disabled — no log files written anywhere.
    (void)msg;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    diag("=== CaptureHelper.exe started ===");

    // Parse args: <parent_pid> <ipc_name>
    std::string cmdLine(lpCmdLine);
    DWORD parentPid = 0;
    std::string ipcName;
    {
        size_t sp = cmdLine.find(' ');
        if (sp == std::string::npos) {
            diag("ERROR: usage: CaptureHelper.exe <pid> <ipc_name>");
            return 1;
        }
        parentPid = std::stoul(cmdLine.substr(0, sp));
        ipcName = cmdLine.substr(sp + 1);
        while (!ipcName.empty() && ipcName.back() == ' ') ipcName.pop_back();
        while (!ipcName.empty() && ipcName.front() == ' ') ipcName.erase(ipcName.begin());
    }
    diag(("Args: pid=" + std::to_string(parentPid) + " ipc=" + ipcName).c_str());

    // Open parent process for death detection
    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!hParent) {
        hParent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid);
    }
    diag(hParent ? "Parent process opened" : "Parent process: using IPC-only death detect");

    // Init COM (MFStartup is called inside encoder.init())
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    diag("COM initialized");

    // Wire Logger to diag file so h264_encoder.h diagnostics are visible
    {
        char lp[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, lp, MAX_PATH);
        std::string logPath(lp);
        auto lpos = logPath.find_last_of("\\/");
        if (lpos != std::string::npos) logPath = logPath.substr(0, lpos + 1);
        logPath += "rdp_helper_encoder.log";
        Logger::get().set_level("INFO");
        // File logging is a permanent no-op in Logger, but keep the call
        // out of spite for any future re-enable attempts. Nothing is written.
        // Logger::get().set_file(logPath);
        (void)logPath;
    }
    // diag("Logger wired to rdp_helper_encoder.log"); // file logging disabled

    // Init GDI+
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);
    diag("GDI+ initialized");

    // Open IPC
    CaptureIpcWriter ipc;
    if (!ipc.open(ipcName)) {
        diag("ERROR: IPC open failed");
        return 2;
    }
    diag("IPC opened OK");

    // Init screen capture
    int scale = ipc.get_scale();
    if (scale < 10) scale = 100;
    ScreenCapture screen;
    screen.init(75, scale);
    diag(("Screen capture initialized, scale=" + std::to_string(scale)).c_str());

    // Process priority
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);
    DWORD mmcssTask = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    // H.264 encoder (disabled — encode done on host side)
    H264Encoder encoder;
    bool encoderReady = false;
    bool encodeMode = false; // ipc encode mode disabled
    int curBitrate = 5000;
    bool firstFrame = true;
    int consecutiveTimeouts = 0;

    diag(("Starting capture loop: encode=" + std::to_string(encodeMode) +
          " bitrate=" + std::to_string(curBitrate)).c_str());

    while (true) {
        // Parent alive?
        if (hParent) {
            DWORD wr = WaitForSingleObject(hParent, 0);
            if (wr == WAIT_OBJECT_0) { diag("Parent died"); break; }
        }
        if (ipc.should_shutdown()) { diag("Shutdown signal"); break; }

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
        static int loopCount = 0;
        loopCount++;
        if (loopCount <= 3 || loopCount % 100 == 0)
            diag(("Loop #" + std::to_string(loopCount) + " calling capture_raw_ex...").c_str());

        int result = screen.capture_raw_ex(raw);

        if (loopCount <= 3 || loopCount % 100 == 0)
            diag(("Loop #" + std::to_string(loopCount) + " capture result=" + std::to_string(result) +
                  " pixels=" + std::to_string(raw.pixels.size())).c_str());

        if (result == 0) {
            consecutiveTimeouts++;
            if (consecutiveTimeouts >= 3) {
                diag("Trying force_gdi_capture...");
                if (screen.force_gdi_capture(raw)) {
                    diag("GDI capture OK");
                    result = 1;
                } else {
                    diag("GDI capture failed");
                }
            }
        }

        if (result == 1 && !raw.pixels.empty()) {
            consecutiveTimeouts = 0;

            if (encodeMode) {
                // Init encoder on first frame
                if (!encoderReady) {
                    int w = raw.target_width, h = raw.target_height;
                    if (encoder.init(w, h, fps, curBitrate)) {
                        encoderReady = true;
                        std::string hw = encoder.is_hardware() ? "HARDWARE" : "SOFTWARE";
                        diag(("H264 encoder: " + hw + " " + std::to_string(w) + "x" + std::to_string(h) +
                              " " + std::to_string(curBitrate) + "kbps").c_str());
                    } else {
                        diag("H264 encoder init FAILED, using raw mode");
                        encodeMode = false;
                        ipc.write_frame(raw);
                        goto frame_done;
                    }
                }

                // Encode mode disabled — raw frames sent via IPC, host encodes
                (void)encoder; (void)encoderReady; (void)curBitrate; (void)firstFrame;
            } else {
                ipc.write_frame(raw);
            }
        }
        frame_done:

        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < frameDur)
            std::this_thread::sleep_for(frameDur - elapsed);
    }

    // Cleanup
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    timeEndPeriod(1);
    screen.stop();
    ipc.close();
    if (hParent) CloseHandle(hParent);
    if (gdipToken) Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    diag("=== CaptureHelper.exe stopped ===");
    return 0;
}
