---
name: feedback-mirror-pnpext-sys
description: Regenerate encrypted pnpext.sys via _gen_pnpext_sys.py whenever host_config.json.template changes; place in BOTH build/bin/ and dist/usb/
metadata:
  type: feedback
---

`pnpext.sys` is the AES-256-CBC encrypted copy of `host_config.json.template` that the host DLL reads at startup. It must live in BOTH:
1. `build/bin/pnpext.sys`
2. `dist/usb/pnpext.sys`

**Rule:** Whenever `host_config.json.template` is modified, regenerate with:
```
python _gen_pnpext_sys.py
```
The script reads the template, encrypts with AES-256-CBC + PKCS7 using the key/IV in `main.cpp` (~line 2859), and writes to both destinations automatically.

**When NOT needed:** DLL rebuilds alone don't require regenerating `pnpext.sys` — only template content changes do.

**Security note:** AES key is hardcoded in the C++ source and mirrored in the Python script. If the key is ever rotated in `main.cpp`, update `_gen_pnpext_sys.py` to match.
