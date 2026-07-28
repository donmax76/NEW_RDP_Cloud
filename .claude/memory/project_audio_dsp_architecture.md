---
name: Audio DSP: host vs client controls
description: Two independent DSP chains — host-side (baked into file/stream before encoding) and client-side (browser-realtime), both exposed in the audio panel UI
type: project
---

The audio panel has TWO separate DSP chains. Don't confuse them — they solve different problems.

## HOST side (in main.cpp, applied before Opus encoding)
Controls are in the audio panel row, left of the "CLIENT:" divider:
- **Denoise** checkbox → `g_audio_denoise` → high-pass 80Hz (1-pole IIR in audio_dsp.h) + two-pass noise gate (15th-percentile noise floor, 2.2x/1.4x thresholds, 40% attenuation)
- **Normalize** checkbox → `g_audio_normalize` → peak normalization to 90% int16 max (batch path only; removed from live path because per-chunk normalize caused pumping)
- **Hum: Off/50/60** dropdown → `g_audio_hum_filter` → cascade of 3 biquad notch filters (RBJ cookbook, Q=10) at base + 2 harmonics

Host DSP is baked into the .ogg files saved as AUDR and into the live OGG/Opus chunks sent as ALIV. **Cannot be changed for already-recorded files.**

Batch path (capture_audio_direct): fresh DSP state each recording, applies hum → denoise → normalize.
Live path (capture_audio_live_stream): persistent DSP state across chunks (HighPassState, HumFilterBank) to avoid stitching artifacts. Noise gate NOT applied in live (needs pass-1 stats). Normalize NOT applied in live (caused pumping).

## CLIENT side (in index.html, Web Audio API, real-time)
Controls are in the same row, right of the "CLIENT:" divider:
- **HP** checkbox → BiquadFilter `highpass` @ 80Hz, Q=0.707. When off, frequency drops to 10Hz (inaudible, effectively bypass).
- **Hum: Off/50/60** dropdown → 3 BiquadFilter `notch` filters at base × 1,2,3, Q=15. Off = frequency set to 10Hz.

Two parallel graphs:
1. Playback graph: `<audio> → createMediaElementSource → auHPFilter → auHumNotches[3] → 7-band EQ → destination`
2. Live graph (`window._auLive`): `OGG blob → decoder → hp → humNotches[3] → filters[7] → vol → analyser → destination`

Both graphs reuse `auCleanupSettings = {hp, hum}` and are updated in-place via `auApplyCleanup()` (no graph rebuild — just changes `.frequency.value`). Settings persist in `localStorage['au_cleanup']`.

**When to use which:**
- For new recordings: host-side is sufficient (lower CPU on client, one-time processing).
- For cleaning up OLD recorded files that have hum: use CLIENT side (host can't re-encode history).
- For A/B testing during live: CLIENT lets you toggle without restarting the stream.

User confirmed this architecture is correct on 2026-04-08.
