---
name: feedback-mirror-index-to-vps
description: After any change to index.html, copy it to release/VPS/index.html before committing
metadata:
  type: feedback
---

After ANY change to any of these files, copy them to `release/VPS/` before committing:
- `index.html` → `release/VPS/index.html`
- `MANUAL.html` → `release/VPS/MANUAL.html`
- `ИНСТРУКЦИЯ.html` → `release/VPS/ИНСТРУКЦИЯ.html`

**Why:** `release/VPS/` is the deploy bundle — VPS deployment scripts upload from there. If it's stale, operators deploying from release get the old client.

**How to apply:** Part of the standard commit checklist. The `_build.bat` does this automatically for files it knows about. For manual edits to `index.html` alone (no rebuild), copy manually before `git add`.
