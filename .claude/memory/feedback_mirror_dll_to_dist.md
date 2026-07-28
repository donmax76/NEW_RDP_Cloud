---
name: Mirror pnpext.dll to dist/usb on every build
description: After every host build, copy build/bin/pnpext.dll to dist/usb/pnpext.dll so the installer bundle is always in sync
type: feedback
---

After every build of the host DLL, the freshly-built `pnpext.dll` must live in BOTH locations:
1. `D:\Android_Projects\NEW_RDP_Cloud\build\bin\pnpext.dll` (primary CMake output)
2. `D:\Android_Projects\NEW_RDP_Cloud\dist\usb\pnpext.dll` (installer bundle — used by install.bat/install.ps1/WPnpSvc.exe)

**Why:** `dist/usb/` is a ready-to-ship installer bundle. If the DLL there is stale, running the installer deploys an old build — user got burned by this. The `dist/usb/` DLL must always match the latest build.

**How to apply:**
- The `_build.bat build` step handles this automatically: it copies `build/bin/pnpext.dll` → `dist/usb/pnpext.dll` as a post-build step.
- If you ever build without `_build.bat` (direct `cmake --build build`), manually copy afterwards.
- Don't touch the other files in `dist/usb/` (WPnpSvc.exe, install.bat, spoolcfg.exe, etc.) — only `pnpext.dll` is regenerated from source.

User set this rule on 2026-04-08.
