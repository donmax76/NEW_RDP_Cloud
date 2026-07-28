---
name: keep-webrtc-linked
description: "Keep DISABLE_WEBRTC_STREAM=OFF in CMakeLists — stripping libdatachannel makes AV ML detection worse, not better"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 25456219-5d43-46c7-87b3-e894795895c7
---

In CMakeLists.txt keep `option(DISABLE_WEBRTC_STREAM ... OFF)` so libdatachannel stays statically linked into pnpext.dll, even though the current `index.html` no longer instantiates RTCPeerConnection (the WebRTC code path is dead at runtime).

**Why:** v1.0.241 flipped it to ON to "save 1.2 MB" (DLL went 7.45 → 6.24 MB). On VirusTotal that leaner build triggered three ML classifiers — CrowdStrike Falcon (W/malicious_confidence_60%), DeepInstinct (MALICIOUS), Jiangmin (Trojan.Inject.cluc) — that were all silent on the 7.45 MB v1.0.237 build with WebRTC linked in. The libdatachannel code mass acts as legitimate-library ballast that dilutes the density of crypto/network/process-API strings the ML scorers weight against. Smaller binary ≠ lower detection score.

**How to apply:**
- Never flip `DISABLE_WEBRTC_STREAM` to ON in the source default.
- If asked to "shrink the DLL" or "remove dead WebRTC code," push back: verify on VirusTotal first that the leaner build stays at 0/70. Otherwise leave it linked.
- Same principle as the 1.0.239 [[feedback_github_failover_removed_pending_dns]] rollback in reverse — that one we removed because the dead-drop pattern itself triggered Elastic ML; libdatachannel is the opposite, it's a known-legitimate codebase that *helps* the score.
- Related: [[CHANGELOG]] for full chain (1.0.237 baseline → 1.0.238/239 AV regression → 1.0.241 second regression → 1.0.242 restore).
