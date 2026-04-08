#!/usr/bin/env python3
"""
Regenerate encrypted pnpext.sys from host_config.json.template.
Encrypts with AES-256-CBC (same key/IV the host uses in main.cpp).
Writes output to BOTH build/bin/pnpext.sys and dist/usb/pnpext.sys.

Run this whenever host_config.json.template changes. The host DLL code
does NOT need to change — it just reads the .sys file at startup.

Usage: python _gen_pnpext_sys.py
"""
import os, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import padding

# Must match g_aes_key / g_aes_iv in main.cpp
KEY = bytes([
    0x3A,0x7F,0x21,0x94,0xC5,0xD2,0x6B,0x11,0x8E,0x4C,0xF9,0x53,0x07,0xB8,0xDA,0x62,
    0x19,0xAF,0x33,0xE4,0x5D,0x70,0x88,0x9B,0xC1,0x2E,0x47,0x6A,0x8D,0x90,0xAB,0xCD
])
IV = bytes([
    0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE,0xF0,0x0F,0x1E,0x2D,0x3C,0x4B,0x5A,0x69,0x78
])

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "host_config.json.template")
DESTS = [
    os.path.join(ROOT, "build", "bin", "pnpext.sys"),
    os.path.join(ROOT, "dist", "usb", "pnpext.sys"),
]

def main():
    with open(SRC, "rb") as f:
        data = f.read()
    padder = padding.PKCS7(128).padder()
    padded = padder.update(data) + padder.finalize()
    cipher = Cipher(algorithms.AES(KEY), modes.CBC(IV))
    enc = cipher.encryptor()
    out = enc.update(padded) + enc.finalize()
    for dst in DESTS:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(out)
        print(f"wrote {len(out)} bytes -> {dst}")

if __name__ == "__main__":
    main()
