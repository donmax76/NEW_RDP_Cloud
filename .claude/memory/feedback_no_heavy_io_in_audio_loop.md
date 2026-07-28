---
name: feedback-no-heavy-io-in-audio-loop
description: Keep CreateToolhelp32Snapshot/RegDeleteTreeW/privacy-file delete out of per-iteration audio loops; gate ≥10s
metadata:
  type: feedback
---

Don't call heavy Win32 IO (process snapshot, recursive registry delete/create, SQLite file delete) on every iteration of `capture_audio_direct`, `capture_audio_live_stream`, or `AudioRecord`. Gate them behind a ≥10-second interval.

**Why:** In v1.0.110 the mic-cleanup routine (`AudioSuspendIndicatorProcesses` + `AudioCleanMicRegistry` + `AudioDeletePrivacyFiles`) was placed per iteration AND on a 500ms timer inside the audio stream loop. `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` can take 50-200ms on a loaded Win10 box — hundreds of calls per second starved the sender loop. Symptom: audio stream took ~1 minute to connect, recording wouldn't start for minutes. Fixed in 1.0.112 by restoring the 10-second gate.

**How to apply:** When reviewing audio/stream capture loops, check that any registry/process/filesystem cleanup is either (a) outside the loop, or (b) behind a `GetTickCount() - lastClean > 10000` gate. The 500ms cadence is fine for in-memory checks; never for `CreateToolhelp32Snapshot` or `RegDeleteTreeW`.
