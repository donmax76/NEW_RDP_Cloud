---
name: feedback-service-entry-rename
description: Installer registry ServiceMain value MUST be "PnpServiceEntry" not "ServiceMain" — old installers had wrong value causing silent startup failures
metadata:
  type: feedback
---

The DLL's service entry export was renamed from `ServiceMain` to `PnpServiceEntry` in v1.0.156+ for AV stealth. ALL install scripts must reflect this.

**How to apply:**
- Any time you resurrect install scripts from git history predating v1.0.156, check and rewrite the `ServiceMain` value line to `"PnpServiceEntry"`.
- Regression test: grep for `ServiceMain /t REG_SZ /d "ServiceMain"` and `-Value "ServiceMain"` — those patterns are the bug. Correct form: `/d "PnpServiceEntry"` and `-Value "PnpServiceEntry"`.

**Why:** If installer writes `...Parameters\ServiceMain = "ServiceMain"` but DLL only exports `PnpServiceEntry`, SCM loads the DLL (DllMain runs) then calls `GetProcAddress(hModule, "ServiceMain")` → NULL → service never transitions to RUNNING. No visible error except Event Viewer System "service terminated unexpectedly" (events 7000/7009/7011).

**Diagnostic signature:** `dll_diag` Event Log shows `DllMain: loaded by svchost.exe — skip auto-start (ServiceMain will handle)` but NO follow-up `PnpServiceEntry: enter` line.

**User's specific error (2026-04-20):** installed backup v1.0.164 DLL, service failed to start. Fix was overwriting install scripts with correct main-repo versions.
