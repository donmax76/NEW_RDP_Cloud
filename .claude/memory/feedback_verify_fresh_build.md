---
name: Verify build produced fresh binaries
description: After every build, verify build/bin/pnpext.dll timestamp is newer than the latest source change — if not, force-rebuild
type: feedback
---

After running `_build.bat build`, ALWAYS verify the output binaries are actually newer than source files. Do not trust "[100%] Built target" — under NMake Makefiles the build can no-op if header dependencies aren't tracked correctly, leaving stale binaries with the old HOST_VERSION.

**Verification steps:**
1. After build: `ls -la build/bin/pnpext.dll host.h main.cpp` — DLL mtime must be ≥ latest source mtime.
2. If DLL looks stale (older than sources you edited), force rebuild:
   ```
   rm -f build/CMakeFiles/RemoteDesktopHostDll.dir/main.cpp.obj build/CMakeFiles/RemoteDesktopHostExe.dir/main.cpp.obj
   _build.bat build
   ```
3. For version bumps specifically, confirm the binary contains the new version:
   `strings build/bin/pnpext.dll | grep -E "^1\.0\.[0-9]+"`

**Why:** On 2026-04-08 I bumped HOST_VERSION 1.0.58 → 1.0.59, reported "rebuilt" to the user, but the DLL was still 1.0.58 from a previous build. User caught it via file timestamp. This was a real footgun — user is shipping DLLs to production machines.

**How to apply:** Every time you rebuild, do a `ls -la` sanity check before telling the user it's ready. If you bumped a version or made a change that must appear in the binary, verify with `strings` or similar. Never assume NMake correctly tracked header-only changes.
