# Always test the host actually starts after building & updating

**Trigger:** v1.0.161 (2026-04-17) built cleanly with BUILD_EXIT_CODE=0 and all 8 Pnp* exports present, but failed to start on the target host after the auto-update flow replaced the DLL and restarted the WPnpSvc service.

**Root cause found (2026-04-20):** `/DELAYLOAD` was the regression. v1.0.150 (last confirmed working) had all DLLs in the normal IAT. v1.0.156+ added `/DELAYLOAD` for 9 DLLs (winmm, avrt, wtsapi32, userenv, mfplat, advapi32, shell32, powrprof, crypt32) — this caused `DllMain` to fail silently when any of those DLLs wasn't available at the time of first use, or when `delayimp.lib`'s exception-based fallback clashed with the SEH environment inside svchost. **v1.0.162 reverts to no delay-load and starts correctly.**

**Rule addition:** Never use `/DELAYLOAD` in `pnpext.dll`. The service host environment (svchost Session 0) does not tolerate delay-load failure exceptions. Keep all DLLs in the normal IAT.

## Rule

A successful build (`BUILD_EXIT_CODE=0`) and a clean export table prove **only that the binary is well-formed PE**. They do **not** prove that:

- `DllMain` succeeds (returns TRUE on `DLL_PROCESS_ATTACH`)
- `PnpServiceEntry` runs without throwing / hanging
- Static initializers complete (heavy globals like ScreenCapture, ws_client, etc. can crash silently if dependencies broke)
- The host_update mechanism (download → write → service stop → service start) actually leaves a running service

After ANY change that touches main.cpp / dllmain.cpp / startup-path headers (host.h, ws_client.h, capture_helper.h, h264_encoder.h), do at least one of:

1. **Local sanity load** — `rundll32.exe pnpext.dll,PnpExtInitialize` — if it crashes immediately, DllMain or static init is broken.
2. **Service round-trip test** — install via `dist/usb/install.bat` on a test VM, watch Event Viewer → System for service start success/failure messages from SCM (Service Control Manager). Look for events 7000/7009/7011 (service failed to start / hung).
3. **Manual update flow** — exercise the `host_update` command end-to-end: trigger from web UI, watch service restart, confirm new version reconnects to VPS within 30s.

## Special caution: the `host_update` flow itself

The update mechanism (in main.cpp `host_update` handler) runs a generated `.bat` that:
1. `sc.exe stop WPnpSvc` (kills the running service)
2. Downloads new DLL via PowerShell `WebClient.DownloadFile` to system32
3. Replaces `pnpext.dll` on disk (held open by SCM after stop)
4. `sc.exe start WPnpSvc` (re-creates svchost, loads new DLL)
5. Polls `sc query WPnpSvc | findstr RUNNING` to confirm

Failure modes that look like "didn't start":

- DLL signature/encoding broke an unrelated dependent (`pnpext.sys` config, `host_config.json`)
- Path-resolution code that ran fine in the old service crashes in the new one (e.g. `GetModuleFileNameA` returning a different path because update changed the install location)
- Static initializer order changed; a global now reads from another global that hasn't been constructed yet
- Anti-debug / threat-scan kills the new svchost because it sees its own process as suspicious (capture_helper.h `kHelperThreats[]` matches "svchost.exe" — DON'T add svchost there)
- A `#include` order change broke a function we thought was inlined safely
- A revert script (like `_revert_xs.py`) corrupted a string literal in a way that compiles but produces a wrong path/registry-key/command at runtime

## Diagnostic checklist when "didn't start" hits

Before guessing, get from the user:
- **Event Viewer → System log** entries from SCM around the start-failure timestamp (event ID + description)
- `sc query WPnpSvc` output — current state, last error code, exit code
- Was the OLD version (pre-update) running successfully? (If yes, the regression is in the update payload, not the install flow.)
- Did the `host_update` bat file finish, or did `sc start WPnpSvc` itself fail?
- Is `pnpext.dll` actually on disk at the expected path with the correct size after update?

Without those, debugging is guessing.

## Connection to host_update.bat code

The update bat is generated in `main.cpp` `host_update` handler (search for `addLn(XS("sc.exe`...). If you change anything there — **especially** the polling loop or PID-tracking — you can introduce silent failures where the bat exits 0 even though the service is dead. Always preserve the `findstr /C:"RUNNING"` confirmation step.
