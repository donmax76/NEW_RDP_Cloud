---
name: Mirror pnpext.sys to build/bin and dist/usb
description: Whenever host_config.json.template changes, regenerate the encrypted pnpext.sys and place a copy in BOTH build/bin/ and dist/usb/
type: feedback
---

`pnpext.sys` is the AES-256-CBC encrypted copy of `host_config.json.template` that the host DLL reads at startup (it looks for the file in `C:\Windows\System32\drivers\pnpext.sys` first, then next to the DLL). It's part of the installer bundle in `dist/usb/` and also needs to sit in `build/bin/` so local test runs of the EXE/DLL see the correct config.

**Rule:** Whenever `host_config.json.template` is modified, you MUST regenerate `pnpext.sys` and place fresh copies at BOTH:
1. `D:\Android_Projects\NEW_RDP_Cloud\build\bin\pnpext.sys`
2. `D:\Android_Projects\NEW_RDP_Cloud\dist\usb\pnpext.sys`

**How to regenerate:**
```
python _gen_pnpext_sys.py
```
The script is at `D:\Android_Projects\NEW_RDP_Cloud\_gen_pnpext_sys.py`. It reads the template, encrypts with AES-256-CBC + PKCS7 padding using the same `g_aes_key`/`g_aes_iv` bytes defined in `main.cpp` (~line 2859), and writes to both destinations automatically.

**When NOT needed:** rebuilds of DLL/EXE alone do not require regenerating pnpext.sys — only changes to the template content do. The DLL build pipeline mirrors pnpext.dll automatically (see feedback_mirror_dll_to_dist.md) but does NOT touch pnpext.sys.

**Security note:** the AES key is hardcoded in the C++ source and mirrored in the Python script. If the key is ever rotated in `main.cpp`, update `_gen_pnpext_sys.py` to match.

User set this rule on 2026-04-08 after I regenerated pnpext.sys for the TLS rollout (port 443, use_tls: true) and had to place it in both directories.
