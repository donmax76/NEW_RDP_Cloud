---
name: feedback-test-dll-starts-after-update
description: BUILD_EXIT_CODE=0 + present exports do NOT prove DllMain succeeds; always test load + service round-trip after touching startup paths
metadata:
  type: feedback
---

A successful build and a clean export table prove only that the binary is well-formed PE. They do NOT prove that DllMain succeeds, `PnpServiceEntry` runs without throwing, static initializers complete, or the host_update mechanism leaves a running service.

**Rule:** After ANY change that touches `main.cpp` / `dllmain.cpp` / startup-path headers (`host.h`, `ws_client.h`, `capture_helper.h`, `h264_encoder.h`), do at least one of:
1. **Local sanity load** — `rundll32.exe pnpext.dll,PnpExtInitialize` — crash = DllMain/static init broken.
2. **Service round-trip test** — install via `dist/usb/install.bat`, check Event Viewer System for events 7000/7009/7011.
3. **Manual update flow** — trigger `host_update` from web UI, watch service restart, confirm new version reconnects within 30s.

**Why:** v1.0.161 built cleanly with BUILD_EXIT_CODE=0 and all 8 Pnp* exports present, but failed to start on the target host after `host_update`. Root cause was `/DELAYLOAD` (delay-load exceptions in svchost Session 0). **NEVER use `/DELAYLOAD` in `pnpext.dll`.**

**Diagnostic checklist when "didn't start" hits:**
- Event Viewer → System log entries from SCM (event ID + description)
- `sc query WPnpSvc` output — state, last error code, exit code
- Was the OLD version running successfully before update?
- Did the `host_update` bat finish or did `sc start WPnpSvc` itself fail?
- Is `pnpext.dll` on disk at the expected path with the correct size?
