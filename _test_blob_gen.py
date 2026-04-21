#!/usr/bin/env python3
"""Test _stage2_get_blob directly for each module, to verify server.py can
actually produce the encrypted blob. Bypasses the WSS layer."""
import sys, hashlib, importlib.util

spec = importlib.util.spec_from_file_location("svr", "/opt/remotedesk/server.py")
mod = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(mod)
except Exception as e:
    print(f"IMPORT FAILED: {type(e).__name__}: {e}")
    sys.exit(1)

token = "my-room-token-123"
for m in ["filemgr", "procmgr", "defender", "sysinfo"]:
    try:
        data = mod._stage2_get_blob(token, m)
        if data is None:
            print(f"  {m}: RETURNED NONE (likely dll_path missing or encrypt failed)")
        else:
            sha = hashlib.sha256(data).hexdigest()[:12]
            print(f"  {m}: OK  bytes={len(data)}  sha256[:12]={sha}")
    except Exception as e:
        print(f"  {m}: EXCEPTION {type(e).__name__}: {e}")
