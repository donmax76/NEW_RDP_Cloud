#!/usr/bin/env python3
"""Bootstrap users.json by importing server.py and calling _load_users.
Run on VPS via: python3 /tmp/_bootstrap_users.py"""
import sys, importlib.util, json
from pathlib import Path

spec = importlib.util.spec_from_file_location("svr", "/opt/remotedesk/server.py")
mod = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(mod)
    mod._load_users()
    uf = Path("/opt/remotedesk/users.json")
    if uf.is_file():
        data = json.loads(uf.read_text())
        users = data.get("users", [])
        print(f"OK: users.json bootstrapped, {len(users)} user(s)")
        for u in users:
            print(f"  {u['username']}  role={u['role']}  created={u['created_at']}")
    else:
        print("ERROR: users.json not created")
except Exception as e:
    print(f"IMPORT FAILED: {type(e).__name__}: {e}")
    raise
