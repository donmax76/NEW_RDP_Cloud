#pragma once
// WebRTC H264 media track streaming via libdatachannel.
// Host creates a PeerConnection with an H264 video track.
// Client (browser) connects via RTCPeerConnection and receives
// the stream natively in a <video> element — no JS decoding needed.
//
// Signaling (offer/answer/ICE) goes through the existing main WebSocket.
// Video goes over UDP (SRTP) — no TCP head-of-line blocking, no burst delivery.

#include <string>
#include <cstdint>
#include <functional>

namespace webrtc_stream {

// Callback to send signaling messages (JSON) to the client via main WebSocket
using SendTextFn = std::function<void(const std::string&)>;

// Callback when browser requests a keyframe (PLI/FIR via RTCP)
using KeyframeRequestFn = std::function<void()>;

// Initialize: client sends webrtc_offer, host creates answer with H264 video track.
// send_text sends JSON back: {"webrtc_answer":{...}} and {"webrtc_ice":{...}}
// on_keyframe_request: called when browser sends PLI — host should produce IDR
// turn_server: optional, e.g. "turn:user:pass@1.2.3.4:3478"
bool init_from_offer(const std::string& type, const std::string& sdp,
                     SendTextFn send_text,
                     KeyframeRequestFn on_keyframe_request = nullptr,
                     const std::string& stun_server = "stun:stun.l.google.com:19302",
                     const std::string& turn_server = "");

// Add remote ICE candidate from client
void add_ice_candidate(const std::string& candidate, const std::string& mid);

// Send one H264 encoded frame (Annex B NAL units with start codes).
// timestamp_us: presentation timestamp in microseconds (monotonically increasing).
// is_keyframe: hint for the packetizer.
// Returns true if sent via WebRTC.
bool send_h264_frame(const uint8_t* data, size_t size, uint64_t timestamp_us, bool is_keyframe);

// Whether the WebRTC track is open and ready to send
bool is_ready();

// Close and cleanup
void close();

} // namespace webrtc_stream
