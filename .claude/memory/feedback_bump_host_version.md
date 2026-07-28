---
name: Always bump HOST_VERSION
description: Bump HOST_VERSION in host.h on every code change to the host (main.cpp, dllmain.cpp, headers, etc.) before rebuilding
type: feedback
---

Every time code in the host is changed (main.cpp, dllmain.cpp, any *.h, CMakeLists, etc.), increment the patch number in `host.h` → `#define HOST_VERSION "1.0.N"` before rebuilding the DLL/EXE.

**Why:** User tracks host versions on deployed machines via the `host_version` field sent in status responses. If the version doesn't change, they can't tell old vs new binaries apart after an update push. User made this rule explicit on 2026-04-08 after I forgot to bump for the thread-fix + audio DSP + playlist changes (bumped from 1.0.58 → 1.0.59 retroactively).

**How to apply:**
- Before any `cmake --build` on this project, check if host code changed — if yes, open `host.h` and bump the patch number (1.0.N → 1.0.N+1).
- Doesn't apply to changes that don't affect the compiled host: index.html alone, server.py, README, scripts, memory files.
- Don't batch multiple bumps into one — each rebuild session gets exactly one version bump.
