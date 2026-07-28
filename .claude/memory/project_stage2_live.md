---
name: project-stage2-live
description: v1.0.170 confirmed end-to-end in production — VPS serves on-the-fly encrypted blobs, host reflective-loads filemgr/procmgr/defender, commands dispatch through stage-2 in RAM
metadata:
  type: project
---

## Production confirmation (2026-04-20)

Real VPS (64.226.66.66) + real target machine + real room_token. Stage-2 fetch → decrypt → reflective-load → dispatch chain confirmed working for all 3 modules.

**VPS `journalctl -u rdp-relay | grep stage2`:**
```
stage2: served procmgr.bin  (247,324 bytes) to host token=my-room-...
stage2: served defender.bin (248,348 bytes) to host token=my-room-...
stage2: served filemgr.bin  (269,340 bytes) to host token=my-room-...
```

**Host Event Log (WPnpSvc):**
```
stage2: fetch_blob_sync sending filemgr id=s2_4_885384609
stage2: cached filemgr (269340 B)
stage2_filemgr: init
stage2_filemgr: 6 commands registered
stage2: loaded filemgr
```

## Bug chain v1.0.166 → v1.0.170

Took 5 iterations to land the working end-to-end:
1. **v1.0.166**: prefetch caching blobs but never calling `ensure_loaded`
2. **v1.0.167**: `shutdown_all` only wiped *loaded* modules' caches; orphan blobs poisoned next startup
3. **v1.0.168**: WSS disconnects left pending CV waits hanging 15-20s; `prefetch_running_` stayed true
4. **v1.0.169**: `stage1_log` wrote to `g_log` → stdout → nowhere in svchost; all stage-2 diagnostics silently lost; fixed by mirroring to `::dll_diag` (Event Log)
5. **v1.0.170 (THE fix)**: `handle_command` checked `cmd.empty()` before routing to `on_fetch_response`; but `json_get("cmd")` finds FIRST occurrence = the nested `data.cmd = "stage2_blob"`, NOT empty; so responses never routed to CV → every fetch timed out. Fix: guard on `id.startsWith("s2_")` alone.

## Current runtime architecture

- Server encrypts blobs on-the-fly from plain `.dll` files in `/opt/remotedesk/stage2/`
- No pre-encrypted blobs needed on VPS; per-token encryption happens at serve time
- Cache cleared on each VPS deploy (`deploy-vps.sh` step [4/11])
- Host's `%TEMP%\pnp_cache\` contains encrypted blobs during runtime; wiped on service stop

## Rollback tags

- `v1.0.164-last-working` — pre-stage-2 baseline
- `v1.0.170-stage2-production-live` — current working state
