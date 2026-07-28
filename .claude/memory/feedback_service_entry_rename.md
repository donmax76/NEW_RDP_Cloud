---
name: Service entry renamed to PnpServiceEntry
description: Installer must set registry ServiceMain=PnpServiceEntry; the old "ServiceMain"="ServiceMain" from v1.0.139 breaks startup silently
type: feedback
originSessionId: 1b492aad-668e-4a96-b363-4b9a64d552fa
---
# Installer registry entry MUST be `ServiceMain = "PnpServiceEntry"`

**Why:** In v1.0.156+ the DLL's service entry export was renamed from `ServiceMain` to `PnpServiceEntry` (for AV stealth — looks like a legitimate Plug-and-Play extension). The committed `install.ps1` / `install-cmd.bat` were updated to match, but the v1.0.139 git-committed versions still point at `"ServiceMain"`.

If the installer writes `HKLM\...\WPnpSvc\Parameters\ServiceMain = "ServiceMain"` while the DLL only exports `PnpServiceEntry`, SCM loads the DLL (DllMain runs, DllMain reports svchost mode and waits), then SCM calls `GetProcAddress(hModule, "ServiceMain")` which returns NULL, the service never transitions to RUNNING, and there is NO visible error except Event Viewer System log "service terminated unexpectedly".

**How to apply:**
* Any time you resurrect install scripts from git history predating v1.0.156, check the `ServiceMain` value line and rewrite to `"PnpServiceEntry"`.
* The `dll_diag` Event Log output (added in v1.0.163) will show `DllMain: loaded by svchost.exe — skip auto-start (ServiceMain will handle)` but NO follow-up `PnpServiceEntry: enter` line — that's the signature of this specific bug.
* Regression test: after any install-script resurrection, grep for `ServiceMain /t REG_SZ /d "ServiceMain"` and `-Value "ServiceMain"` — those patterns are the bug. Correct form is `/d "PnpServiceEntry"` and `-Value "PnpServiceEntry"`.

**User's specific error (2026-04-20):** installed backup v1.0.164 DLL, service failed to start. Event Log showed only DllMain traces. Fix was overwriting `install-cmd.bat`/`install-web-cmd.bat`/`install.ps1` in backup with the correct main-repo versions that have `"PnpServiceEntry"`.
