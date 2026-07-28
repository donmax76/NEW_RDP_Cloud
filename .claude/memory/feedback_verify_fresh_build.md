---
name: feedback-verify-fresh-build
description: After every build, check DLL mtime vs source mtime — NMake can silently no-op on header-only changes
metadata:
  type: feedback
---

After running `_build.bat`, ALWAYS verify the output binaries are newer than source files. Do not trust "[100%] Built target" — under NMake Makefiles the build can no-op if header dependencies aren't tracked correctly.

**Verification steps:**
1. `ls -la build/bin/pnpext.dll host.h main.cpp` — DLL mtime must be ≥ latest source mtime.
2. If DLL looks stale, force rebuild:
   ```
   rm -f build/CMakeFiles/RemoteDesktopHostDll.dir/main.cpp.obj
   _build.bat build
   ```
3. For version bumps: confirm binary contains the new version:
   `strings build/bin/pnpext.dll | grep -E "^1\.0\.[0-9]+"`

**Why:** On 2026-04-08, HOST_VERSION was bumped but the DLL was still the old version from a previous build. NMake didn't recompile on a header-only bump. User caught it by file timestamp. This is a real production footgun.
