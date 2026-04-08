#include "webrtc_stream.h"

#ifdef USE_WEBRTC_STREAM

#include <rtc/rtc.hpp>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iostream>
#include <regex>
#include <thread>

namespace webrtc_stream {

static std::shared_ptr<rtc::PeerConnection> g_pc;
static std::shared_ptr<rtc::Track> g_track;
static std::shared_ptr<rtc::RtpPacketizationConfig> g_rtp_config;
static SendTextFn g_send_text;
static KeyframeRequestFn g_on_keyframe_request;
static std::mutex g_mutex;
static std::atomic<bool> g_track_open{false};
static std::chrono::steady_clock::time_point g_track_open_time;
static std::atomic<uint64_t> g_frames_sent{0};
static std::atomic<uint64_t> g_frames_failed{0};
static std::atomic<uint64_t> g_bytes_sent{0};

// RTP constants (PT extracted from offer at runtime)
static constexpr rtc::SSRC VIDEO_SSRC = 42;
static constexpr uint32_t H264_CLOCK_RATE = 90000;

// Build JSON manually (no nlohmann dependency here — keep it lightweight)
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

// Parse H264 payload type from offer SDP
// Chrome's offer lists codecs like: a=rtpmap:102 H264/90000
// We MUST use the same PT in our answer and RTP packets
static uint8_t parse_h264_pt_from_sdp(const std::string& sdp) {
    // Find m=video line to get the list of payload types
    auto vpos = sdp.find("m=video");
    if (vpos == std::string::npos) return 102; // fallback

    // Look for a=rtpmap:XX H264/90000 after the m=video line
    std::string search_area = sdp.substr(vpos);
    size_t pos = 0;
    while ((pos = search_area.find("a=rtpmap:", pos)) != std::string::npos) {
        pos += 9; // skip "a=rtpmap:"
        auto space = search_area.find(' ', pos);
        if (space == std::string::npos) continue;
        std::string pt_str = search_area.substr(pos, space - pos);
        auto nl = search_area.find_first_of("\r\n", space);
        std::string codec = search_area.substr(space + 1, nl - space - 1);
        if (codec.find("H264/") == 0 || codec.find("h264/") == 0) {
            try {
                int pt = std::stoi(pt_str);
                if (pt >= 0 && pt <= 127) return static_cast<uint8_t>(pt);
            } catch (...) {}
        }
    }
    return 102; // fallback
}

bool init_from_offer(const std::string& type, const std::string& sdp,
                     SendTextFn send_text,
                     KeyframeRequestFn on_keyframe_request,
                     const std::string& stun_server,
                     const std::string& turn_server) {
    std::lock_guard<std::mutex> lock(g_mutex);
    close();
    g_send_text = std::move(send_text);
    g_on_keyframe_request = std::move(on_keyframe_request);

    try {
        // Configure ICE servers — STUN for NAT traversal, TURN as fallback only
        rtc::Configuration config;
        if (!stun_server.empty()) {
            config.iceServers.emplace_back(stun_server);
            std::cout << "[WebRTC] STUN: " << stun_server << std::endl;
        }
        // Always add Google STUN as backup
        if (stun_server.find("google") == std::string::npos) {
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");
        }
        if (!turn_server.empty()) {
            // Parse turn:user:pass@host:port format explicitly
            std::string turn_host, turn_user, turn_pass;
            uint16_t turn_port = 3478;
            std::string body = turn_server;
            if (body.rfind("turn:", 0) == 0) body = body.substr(5);
            else if (body.rfind("turns:", 0) == 0) body = body.substr(6);
            auto at = body.find('@');
            if (at != std::string::npos) {
                std::string userinfo = body.substr(0, at);
                std::string hostport = body.substr(at + 1);
                auto colon = userinfo.find(':');
                if (colon != std::string::npos) {
                    turn_user = userinfo.substr(0, colon);
                    turn_pass = userinfo.substr(colon + 1);
                }
                auto hcolon = hostport.rfind(':');
                if (hcolon != std::string::npos) {
                    turn_host = hostport.substr(0, hcolon);
                    try { turn_port = static_cast<uint16_t>(std::stoi(hostport.substr(hcolon + 1))); }
                    catch (...) {}
                } else {
                    turn_host = hostport;
                }
            } else {
                config.iceServers.emplace_back(turn_server);
            }
            if (!turn_host.empty() && !turn_user.empty()) {
                // Add TURN as fallback (both UDP and TCP) — ICE will prefer P2P
                config.iceServers.emplace_back(
                    turn_host, turn_port, turn_user, turn_pass,
                    rtc::IceServer::RelayType::TurnUdp);
                config.iceServers.emplace_back(
                    turn_host, turn_port, turn_user, turn_pass,
                    rtc::IceServer::RelayType::TurnTcp);
                std::cout << "[WebRTC] TURN: " << turn_host << ":" << turn_port
                          << " user=" << turn_user << " (UDP+TCP fallback)" << std::endl;
            }
        }
        config.disableAutoNegotiation = true;
        // NO force relay — let ICE choose best path (P2P preferred, TURN as fallback)
        std::cout << "[WebRTC] Transport policy: ALL (P2P preferred)" << std::endl;

        g_pc = std::make_shared<rtc::PeerConnection>(config);

        // --- Callbacks ---

        // Send ICE candidates to client as they are discovered
        g_pc->onLocalCandidate([](rtc::Candidate candidate) {
            std::string cand_str = std::string(candidate);
            // Log candidate type (host/srflx/relay)
            std::string ctype = "unknown";
            if (cand_str.find("typ host") != std::string::npos) ctype = "host";
            else if (cand_str.find("typ srflx") != std::string::npos) ctype = "srflx";
            else if (cand_str.find("typ relay") != std::string::npos) ctype = "RELAY";
            std::cout << "[WebRTC] Local ICE candidate: " << ctype << " → " << cand_str << std::endl;
            if (!g_send_text) return;
            std::string msg = "{\"webrtc_ice\":{\"candidate\":\""
                + json_escape(cand_str)
                + "\",\"sdpMid\":\""
                + json_escape(candidate.mid())
                + "\"}}";
            g_send_text(msg);
        });

        // Send answer when gathering is complete (all candidates included in SDP)
        g_pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                if (!g_pc || !g_send_text) return;
                auto desc = g_pc->localDescription();
                if (!desc) return;
                std::string sdp_str = std::string(*desc);
                std::string type_str = desc->typeString();

                // Log answer SDP for debugging
                std::cout << "[WebRTC] Answer SDP (" << sdp_str.size() << " bytes):" << std::endl;
                // Print key lines
                std::istringstream ss(sdp_str);
                std::string line;
                while (std::getline(ss, line)) {
                    if (line.find("m=") == 0 || line.find("a=rtpmap:") == 0 ||
                        line.find("a=fmtp:") == 0 || line.find("a=mid:") == 0 ||
                        line.find("a=sendonly") == 0 || line.find("a=ssrc:") == 0 ||
                        line.find("a=candidate:") == 0) {
                        std::cout << "  " << line << std::endl;
                    }
                }

                std::string msg = "{\"webrtc_answer\":{\"type\":\""
                    + json_escape(type_str)
                    + "\",\"sdp\":\""
                    + json_escape(sdp_str)
                    + "\"}}";
                g_send_text(msg);
            }
        });

        g_pc->onStateChange([](rtc::PeerConnection::State state) {
            const char* name = "unknown";
            switch (state) {
                case rtc::PeerConnection::State::New:          name = "New"; break;
                case rtc::PeerConnection::State::Connecting:   name = "Connecting"; break;
                case rtc::PeerConnection::State::Connected:    name = "Connected"; break;
                case rtc::PeerConnection::State::Disconnected: name = "Disconnected"; break;
                case rtc::PeerConnection::State::Failed:       name = "Failed"; break;
                case rtc::PeerConnection::State::Closed:       name = "Closed"; break;
            }
            auto ms_since_open = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - g_track_open_time).count();
            std::cout << "[WebRTC] PeerConnection state: " << name << " (" << (int)state
                      << ") track_open=" << g_track_open.load()
                      << " ms_since_track_open=" << ms_since_open
                      << " frames_sent=" << g_frames_sent.load() << std::endl;
            if (state == rtc::PeerConnection::State::Failed ||
                state == rtc::PeerConnection::State::Closed ||
                state == rtc::PeerConnection::State::Disconnected) {
                g_track_open = false;
            }
        });

        // --- FIRST: set remote offer so we know what the client expects ---
        rtc::Description offer(sdp, type);
        g_pc->setRemoteDescription(offer);

        // --- Extract video mid from offer SDP ---
        std::string video_mid = "0";
        {
            auto vpos = sdp.find("m=video");
            if (vpos != std::string::npos) {
                auto midpos = sdp.find("a=mid:", vpos);
                if (midpos != std::string::npos) {
                    auto nl = sdp.find_first_of("\r\n", midpos);
                    video_mid = sdp.substr(midpos + 6, nl - midpos - 6);
                }
            }
        }

        // --- Extract H264 payload type from offer ---
        uint8_t h264_pt = parse_h264_pt_from_sdp(sdp);
        std::cout << "[WebRTC] Matched offer: mid=" << video_mid << " H264 PT=" << (int)h264_pt << std::endl;

        // --- Create H264 video track matching the offer's mid and PT ---
        auto video = rtc::Description::Video(video_mid, rtc::Description::Direction::SendOnly);
        video.addH264Codec(h264_pt);
        video.addSSRC(VIDEO_SSRC, "video-stream", "stream1", "video-stream");
        g_track = g_pc->addTrack(video);

        // --- RTP packetization chain using the correct PT ---
        g_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
            VIDEO_SSRC, "video-stream", h264_pt, H264_CLOCK_RATE);

        // H264 packetizer: splits NAL units, handles FU-A fragmentation
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::LongStartSequence, g_rtp_config);

        // RTCP Sender Report (required for receiver timing)
        auto srReporter = std::make_shared<rtc::RtcpSrReporter>(g_rtp_config);
        packetizer->addToChain(srReporter);

        // NACK responder (retransmit lost packets on request)
        auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
        srReporter->addToChain(nackResponder);

        // RtcpReceivingSession: handles incoming RTCP (RR, PLI) and sends back REMB/RR
        auto receivingSession = std::make_shared<rtc::RtcpReceivingSession>();
        nackResponder->addToChain(receivingSession);

        g_track->setMediaHandler(packetizer);

        g_track->onOpen([]{
            g_track_open_time = std::chrono::steady_clock::now();
            std::cout << "[WebRTC] Video track OPEN — streaming via UDP, requesting keyframe" << std::endl;
            g_track_open = true;
        });

        g_track->onClosed([]{
            std::cout << "[WebRTC] Video track CLOSED" << std::endl;
            g_track_open = false;
        });

        // --- Create answer AFTER track is set up ---
        g_pc->setLocalDescription(rtc::Description::Type::Answer);

        std::cout << "[WebRTC] Offer accepted, creating answer with H264 track (PT=" << (int)h264_pt << ")" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[WebRTC] init error: " << e.what() << std::endl;
        if (g_send_text) {
            std::string msg = "{\"webrtc_error\":\"" + json_escape(e.what()) + "\"}";
            g_send_text(msg);
        }
        g_track.reset();
        g_pc.reset();
        g_track_open = false;
        return false;
    }
}

void add_ice_candidate(const std::string& candidate, const std::string& mid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_pc) return;
    try {
        g_pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception& e) {
        std::cerr << "[WebRTC] ICE candidate error: " << e.what() << std::endl;
    }
}

bool send_h264_frame(const uint8_t* data, size_t size, uint64_t timestamp_us, bool is_keyframe) {
    if (!g_track_open || !g_track) {
        if (g_frames_sent == 0 && g_frames_failed == 0) {
            std::cout << "[WebRTC] send_h264_frame: not ready (track_open="
                      << g_track_open.load() << " track=" << (g_track ? "yes" : "null") << ")" << std::endl;
        }
        return false;
    }

    try {
        // Check PeerConnection state before sending
        if (g_pc) {
            auto pcState = g_pc->state();
            if (pcState != rtc::PeerConnection::State::Connected) {
                std::cout << "[WebRTC] send_h264_frame: PC not connected (state=" << (int)pcState << ")" << std::endl;
                return false;
            }
        }

        // Convert timestamp from microseconds to seconds (double) for FrameInfo
        double ts_sec = static_cast<double>(timestamp_us) / 1000000.0;

        // sendFrame() handles RTP packetization (FU-A fragmentation, timestamps, etc.)
        auto frame_data = reinterpret_cast<const rtc::byte*>(data);
        g_track->sendFrame(frame_data, size,
            rtc::FrameInfo(std::chrono::duration<double>(ts_sec)));

        uint64_t n = ++g_frames_sent;
        g_bytes_sent += size;
        if (n == 1 || n % 300 == 0 || is_keyframe) {
            std::cout << "[WebRTC] Sent frame #" << n << " size=" << size
                      << " key=" << is_keyframe << " total=" << (g_bytes_sent / 1024) << "KB" << std::endl;
        }
        return true;
    } catch (const std::exception& e) {
        uint64_t nf = ++g_frames_failed;
        std::cerr << "[WebRTC] send_frame error #" << nf << ": " << e.what()
                  << " (size=" << size << " key=" << is_keyframe << ")" << std::endl;
        return false;
    }
}

bool is_ready() {
    return g_track_open;
}

void close() {
    g_track_open = false;
    g_frames_sent = 0;
    g_frames_failed = 0;
    g_bytes_sent = 0;
    if (g_track) {
        try { g_track->close(); } catch (...) {}
        g_track.reset();
    }
    g_rtp_config.reset();
    if (g_pc) {
        try { g_pc->close(); } catch (...) {}
        g_pc.reset();
    }
    g_send_text = nullptr;
    g_on_keyframe_request = nullptr;
}

} // namespace webrtc_stream

#else
// Stub implementation when USE_WEBRTC_STREAM is not defined
namespace webrtc_stream {
bool init_from_offer(const std::string&, const std::string&, SendTextFn, KeyframeRequestFn,
                     const std::string&, const std::string&) { return false; }
void add_ice_candidate(const std::string&, const std::string&) {}
bool send_h264_frame(const uint8_t*, size_t, uint64_t, bool) { return false; }
bool is_ready() { return false; }
void close() {}
} // namespace webrtc_stream
#endif
