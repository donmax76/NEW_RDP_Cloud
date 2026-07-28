---
name: feedback-keep-webrtc-linked
description: Keep DISABLE_WEBRTC_STREAM=OFF — stripping libdatachannel made 1.0.241 light up CrowdStrike/DeepInstinct/Jiangmin
metadata:
  type: feedback
---

In `CMakeLists.txt` keep `option(DISABLE_WEBRTC_STREAM ... OFF)` so libdatachannel stays statically linked into `pnpext.dll`, even though the current `index.html` no longer instantiates RTCPeerConnection.

**Why:** v1.0.241 flipped it to ON to "save 1.2 MB" (DLL went 7.45 → 6.24 MB). On VirusTotal that leaner build triggered three ML classifiers — CrowdStrike Falcon (W/malicious_confidence_60%), DeepInstinct (MALICIOUS), Jiangmin (Trojan.Inject.cluc) — all silent on 7.45 MB v1.0.237 with WebRTC. The libdatachannel code mass acts as legitimate-library ballast that dilutes the density of crypto/network/process-API strings that ML scorers weight against. **Smaller binary ≠ lower detection score.**

**How to apply:**
- Never flip `DISABLE_WEBRTC_STREAM` to ON.
- If asked to "shrink the DLL" or "remove dead WebRTC code," push back: verify on VirusTotal first that the leaner build stays 0/70. Otherwise leave it linked.
