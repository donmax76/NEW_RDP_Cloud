---
name: project-audio-dsp-architecture
description: Two independent DSP chains — host-side (baked into file/stream before encoding) and client-side (browser-realtime)
metadata:
  type: project
---

The audio panel has TWO separate DSP chains. Don't confuse them.

## HOST side (main.cpp, applied before Opus encoding)
Controls left of the "CLIENT:" divider in the audio panel:
- **Denoise** → `g_audio_denoise` → high-pass 80Hz (1-pole IIR) + two-pass noise gate (15th-percentile noise floor, 2.2x/1.4x thresholds, 40% attenuation)
- **Normalize** → `g_audio_normalize` → peak normalization to 90% int16 max (batch path only; removed from live path — caused pumping)
- **Hum: Off/50/60** → `g_audio_hum_filter` → cascade of 3 biquad notch filters (RBJ cookbook, Q=10) at base + 2 harmonics

DSP chain order: `hum → denoise → normalize`. Baked into `.ogg` files (AUDR) and live OGG/Opus chunks (ALIV). **Cannot be changed for already-recorded files.**

Live path has persistent DSP state across chunks (HighPassState, HumFilterBank) to avoid stitching artifacts. Noise gate and normalize NOT applied in live path.

## CLIENT side (index.html, Web Audio API, realtime)
Controls right of the "CLIENT:" divider:
- **HP** → BiquadFilter `highpass` @ 80Hz, Q=0.707
- **Hum: Off/50/60** → 3 BiquadFilter `notch` filters at base × 1,2,3, Q=15

Two parallel graphs (playback + live). Settings updated via `auApplyCleanup()` without graph rebuild; persisted in `localStorage['au_cleanup']`.

**When to use which:**
- New recordings: host-side is sufficient.
- Cleaning OLD recorded files with hum: use CLIENT side (host can't re-encode history).
- A/B testing during live: CLIENT lets you toggle without restarting the stream.
