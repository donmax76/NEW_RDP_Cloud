#!/usr/bin/env python3
"""
Bump patch version across all three components:
  - host.h:    HOST_VERSION "1.0.XX"
  - index.html: <meta name="build" content="1.0.XX" />
  - server.py:  SERVER_VERSION = "1.0.XX"

All three stay in sync. Run before building.
Usage: python _bump_version.py
"""
import re, sys, os

ROOT = os.path.dirname(os.path.abspath(__file__))

files = {
    'host.h':     (r'#define HOST_VERSION "1\.0\.(\d+)"', '#define HOST_VERSION "1.0.{}"'),
    'index.html': (r'<meta name="build" content="1\.0\.(\d+)"', '<meta name="build" content="1.0.{}"'),
    'server.py':  (r'SERVER_VERSION = "1\.0\.(\d+)"', 'SERVER_VERSION = "1.0.{}"'),
}

# Find current max version across all files
max_ver = 0
for fname, (pattern, _) in files.items():
    fpath = os.path.join(ROOT, fname)
    with open(fpath, 'r', encoding='utf-8') as f:
        m = re.search(pattern, f.read())
        if m:
            v = int(m.group(1))
            if v > max_ver:
                max_ver = v

new_ver = max_ver + 1

# Update all files
for fname, (pattern, template) in files.items():
    fpath = os.path.join(ROOT, fname)
    with open(fpath, 'r', encoding='utf-8') as f:
        content = f.read()
    new_content = re.sub(pattern, template.format(new_ver), content, count=1)
    with open(fpath, 'w', encoding='utf-8') as f:
        f.write(new_content)
    print(f"  {fname}: 1.0.{max_ver} -> 1.0.{new_ver}")

print(f"\nAll versions bumped to 1.0.{new_ver}")
