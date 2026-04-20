#!/usr/bin/env python3
"""
_deploy_stage2.py — encrypt stage-2 modules for a given room_token and
stage them under deploy/stage2/<token>/ ready to upload to the VPS.

Usage:
    python _deploy_stage2.py <room_token>
    python _deploy_stage2.py --from-config build/bin/host_config.json

The output tree matches what server.py serves from STAGE2_DIR:

    deploy/stage2/<room_token>/filemgr.bin
    deploy/stage2/<room_token>/procmgr.bin
    deploy/stage2/<room_token>/defender.bin
    deploy/stage2/<room_token>/README.txt

To deploy to your VPS:
    scp -r deploy/stage2/<room_token> root@<vps>:/opt/remotedesk/stage2/
    sudo systemctl restart rdp-server    # or whatever runs server.py
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent.resolve()
BUILD_STAGE2_DIR = HERE / "build" / "stage2"
DEPLOY_ROOT = HERE / "deploy" / "stage2"
GEN_SCRIPT = HERE / "_gen_stage2_blob.py"

# Modules produced by the build (name matches the CMake target name).
# Add new modules here as they get extracted.
MODULES = ["filemgr", "procmgr", "defender"]


def load_token_from_config(path: Path) -> str:
    if not path.is_file():
        sys.exit(f"ERROR: config file not found: {path}")
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    token = str(data.get("room_token", "")).strip()
    if not token:
        sys.exit(f"ERROR: room_token empty in {path}")
    return token


def main() -> int:
    p = argparse.ArgumentParser()
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("room_token", nargs="?",
                   help="Room token (must match what the host uses).")
    g.add_argument("--from-config",
                   help="Path to host_config.json that contains room_token.")
    args = p.parse_args()

    token = args.room_token or load_token_from_config(Path(args.from_config))
    print(f"Deploying stage-2 modules for token: {token[:8]}...{token[-4:]}  (len={len(token)})")

    if not BUILD_STAGE2_DIR.is_dir():
        sys.exit(f"ERROR: {BUILD_STAGE2_DIR} does not exist — run the build first "
                 f"(cmake --build build).")

    out_dir = DEPLOY_ROOT / token
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Encrypt each module DLL
    encrypted = []
    for mod in MODULES:
        dll = BUILD_STAGE2_DIR / f"{mod}.dll"
        if not dll.is_file():
            print(f"  skip {mod}: {dll} not built")
            continue
        bin_out = out_dir / f"{mod}.bin"
        rc = subprocess.call([
            sys.executable, str(GEN_SCRIPT), token, str(dll), str(bin_out)
        ])
        if rc != 0:
            sys.exit(f"ERROR: encryption failed for {mod}")
        # quick sanity check: entropy should be near max
        data = bin_out.read_bytes()
        if len(data) < 100 or data[:2] == b"MZ":
            sys.exit(f"ERROR: {bin_out} looks wrong (first 2 bytes or size)")
        sha = hashlib.sha256(data).hexdigest()[:12]
        encrypted.append((mod, len(data), sha))

    # README for human verification (NOT shipped to VPS)
    readme = out_dir / "README.txt"
    with readme.open("w", encoding="utf-8") as f:
        f.write(f"Stage-2 deployment bundle\n")
        f.write(f"Room token (first/last 8 shown): {token[:8]}...{token[-8:]}\n")
        f.write(f"Generated: {os.path.basename(str(HERE))} @ {os.environ.get('COMPUTERNAME','')}\n\n")
        f.write(f"Modules:\n")
        for mod, sz, sha in encrypted:
            f.write(f"  {mod}.bin  {sz:>10,} bytes  sha256[0:12]={sha}\n")
        f.write(f"\nUpload:\n")
        f.write(f"    scp {out_dir.name}/*.bin root@<vps>:/opt/remotedesk/stage2/{token}/\n")
        f.write(f"\nOr tar-pipe it:\n")
        f.write(f"    tar -cf - -C {out_dir} . | ssh root@<vps> "
                f"'cd /opt/remotedesk/stage2 && mkdir -p {token} && tar -xf - -C {token}'\n")
        f.write(f"\nAfter upload, restart the server so it picks up new blobs.\n")

    # Summary
    print(f"\n=== Deployment bundle ready: {out_dir} ===")
    for mod, sz, sha in encrypted:
        print(f"  {mod}.bin  {sz:>10,} bytes  sha256[0:12]={sha}")
    print(f"\nUpload with:")
    print(f"  scp -r {out_dir} root@<vps>:/opt/remotedesk/stage2/")
    print(f"\nSee {readme} for details.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
