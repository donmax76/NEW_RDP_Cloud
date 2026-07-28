# VPS deployment: two parallel paths, always upload to BOTH

The VPS has **three** locations that each need the latest files:

| Path | Contents | Served by | Trigger |
|---|---|---|---|
| `/var/www/remote-desktop/` | `index.html`, `MANUAL.html`, `PRESENTATION.html`, `server.py` | **nginx** (HTTPS :443) — `root /var/www/remote-desktop` | Browser fetches HTML |
| `/var/www/remote-desktop/files/` | `pnpext.dll`, `pnpext.sys`, `install*.bat/ps1`, `uninstall*.bat/ps1`, `diagnose.ps1`, `INSTRUCTIONS.md`, `refresh-stage2.bat` | **nginx** (HTTPS :443 /files/) | Host-update pulls DLL; operators download installers |
| `/opt/remotedesk/` | `server.py`, `host_events.log`, `users.json`, `stage2/`, `venv/`, `screenshots/`, `audio/` | **rdp-relay systemd service** — `ExecStart=/opt/remotedesk/venv/bin/python3 /opt/remotedesk/server.py`, `WorkingDirectory=/opt/remotedesk` | Service starts |

## After a new DLL build you MUST also sync `dist/usb/` → `/var/www/remote-desktop/files/`

Otherwise the `host_update` command (URL `https://VPS/files/pnpext.dll`) keeps
downloading the PREVIOUS build, the target host "updates" to the same old
version, and the viewer panel keeps showing `Host: 1.0.<old>` while
`Client:` and `VPS:` show the new one.

## Symptom if you forget

Browser `Client:` field shows old version while `VPS:` shows new version. Caused by uploading `index.html` only to `/opt/remotedesk/` — nginx doesn't read that directory, so the HTML served over HTTPS is still the previous build.

## Correct deployment (always both)

```python
sftp.put('index.html', '/var/www/remote-desktop/index.html')   # nginx serves this
sftp.put('index.html', '/opt/remotedesk/index.html')           # keep in sync (optional)
sftp.put('server.py',  '/var/www/remote-desktop/server.py')    # symmetry
sftp.put('server.py',  '/opt/remotedesk/server.py')            # rdp-relay runs this
sftp.put('MANUAL.html', '/var/www/remote-desktop/MANUAL.html') # nginx serves
sftp.put('PRESENTATION.html', '/var/www/remote-desktop/PRESENTATION.html')
# Then: systemctl restart rdp-relay
```

## Verify after deploy

```bash
# What nginx is actually serving right now:
curl -sk https://localhost/ | grep -oE 'meta name="build" content="[^"]+'
# Should match the HOST_VERSION in host.h.
```

## Nginx cache

Cache-Control is already `no-cache, no-store, must-revalidate` in the nginx config, so nginx-side caching is NOT the issue — the only way to get stale content is to put the file in the wrong directory. Browser cache is the other trap: operators must Ctrl+Shift+R after each deploy.
