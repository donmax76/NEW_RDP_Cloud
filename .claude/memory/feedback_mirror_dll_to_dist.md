---
name: feedback-mirror-dll-to-dist
description: Post-build copy build/bin/pnpext.dll → dist/usb/pnpext.dll so installer bundle stays in sync
metadata:
  type: feedback
---

After every build of the host DLL, the freshly-built `pnpext.dll` must land in BOTH:
1. `build/bin/pnpext.dll` (primary CMake output)
2. `dist/usb/pnpext.dll` (installer bundle — used by `install.bat`/`install.ps1`/`WPnpSvc.exe`)

**Why:** `dist/usb/` is the ready-to-ship installer bundle. Stale DLL there means deploying an old build.

**How to apply:**
- `_build.bat` handles this automatically as a post-build step.
- If you ever build without `_build.bat` (direct `cmake --build build`), manually copy afterwards.
- Don't touch other files in `dist/usb/` — only `pnpext.dll` is regenerated from source.
