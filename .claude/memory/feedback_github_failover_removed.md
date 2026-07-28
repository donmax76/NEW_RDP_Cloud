---
name: feedback-github-failover-removed
description: GitHub failover removed in v1.0.202 to kill Elastic ML C2 detection — do NOT restore WinHTTP to api.github.com
metadata:
  type: feedback
---

The entire GitHub VPS-failover subsystem was deleted from `main.cpp` in v1.0.202. **Do not restore it.**

**Why:** Elastic ML detected it as MITRE ATT&CK T1071 (Application Layer Protocol dead-drop) + T1027 (obfuscation). Periodic WinHTTP requests to `api.github.com` with Bearer token to fetch config = classic C2 dead-drop behavioral pattern. String obfuscation (XOR-encoded URL, generic UA) didn't help — ML classifiers see the behavior, not the strings. Three AV vendors flagged it (CrowdStrike, Elastic, SecureAge).

**What was removed:**
- `fo_github_fetch()`, `vps_failover_thread_func()`, all helpers
- `set_fo` / `get_fo_stat` command handlers
- `HostConfig` failover fields (`github_failover_enabled`, etc.)
- `index.html` GitHub failover settings UI block

**To restore failover safely in the future (pick one):**
1. **DNS A-record** — use `rdp.domain.com` in config instead of IP literal; host calls `getaddrinfo()` which is zero-AV. Admin updates A-record when VPS changes.
2. **Server-push** — server sends `{cmd:"set_backup","ip":"X.X.X.X"}` over existing WSS; host caches in encrypted config.

**NEVER** restore HTTP-based lookup to any external service (GitHub, pastebin, S3, Cloudflare Workers) — all trigger T1071.
