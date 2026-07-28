---
name: feedback-vps-dual-deploy-paths
description: Always upload index.html/server.py to BOTH nginx root AND rdp-relay dir; one-path deploy leaves stale content
metadata:
  type: feedback
---

The VPS has three locations that all need the latest files after a deploy:

| Path | Contents | Served by |
|---|---|---|
| `/var/www/remote-desktop/` | `index.html`, `MANUAL.html`, `PRESENTATION.html`, `server.py` | nginx (HTTPS :443) |
| `/var/www/remote-desktop/files/` | `pnpext.dll`, `pnpext.sys`, install/uninstall scripts | nginx `/files/` location |
| `/opt/remotedesk/` | `server.py` (runtime), `stage2/`, `venv/`, logs | rdp-relay systemd service |

After a new DLL build, also sync `dist/usb/` → `/var/www/remote-desktop/files/` — otherwise `host_update` keeps downloading the previous build.

**Why:** Uploading `index.html` only to `/opt/remotedesk/` causes the browser to keep showing old Client version (nginx reads from `/var/www/remote-desktop/`, not `/opt/remotedesk/`).

**Correct deploy (always both):**
```python
sftp.put('index.html', '/var/www/remote-desktop/index.html')
sftp.put('index.html', '/opt/remotedesk/index.html')
sftp.put('server.py',  '/opt/remotedesk/server.py')
sftp.put('server.py',  '/var/www/remote-desktop/server.py')
# Then: systemctl restart rdp-relay
```

**Verify:** `curl -sk https://localhost/ | grep -oE 'meta name="build" content="[^"]+'`
