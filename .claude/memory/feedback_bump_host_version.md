---
name: feedback-bump-host-version
description: Increment HOST_VERSION in host.h on every host-code change before rebuilding
metadata:
  type: feedback
---

Every time code in the host is changed (`main.cpp`, `dllmain.cpp`, any `.h`, `CMakeLists`, etc.), increment the patch number in `host.h` → `#define HOST_VERSION "1.0.N"` before rebuilding.

**Why:** User tracks host versions on deployed machines via the `host_version` field in status responses. If the version doesn't change, old vs new binaries can't be distinguished after an update push.

**How to apply:**
- `_build.bat` runs `_bump_version.py` automatically — handles the bump if you use the standard build pipeline.
- If you build directly with `run_build.ps1` (which skips `_bump_version.py`), manually bump first.
- Doesn't apply to changes that don't affect the compiled host: `index.html` alone, `server.py`, README, scripts, memory files.
- Don't batch multiple bumps — each rebuild session gets exactly one version bump.
