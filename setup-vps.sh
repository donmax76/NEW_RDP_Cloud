#!/bin/bash
# Full VPS setup: coturn + web deploy + relay server as systemd service
# Usage: sudo bash setup-vps.sh [TURN_USER] [TURN_PASSWORD]
#
# What it does:
#   1. Installs/configures coturn (TURN/STUN) with tuned UDP buffers
#   2. Deploys web files (index.html) to nginx with HTTPS
#   3. Creates systemd service for server.py (auto-start on boot)
#
# After setup:
#   systemctl status rdp-relay        — check relay server
#   journalctl -u rdp-relay -f        — live logs
#   systemctl restart rdp-relay       — restart relay
#   systemctl restart coturn           — restart TURN

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TURN_USER="${1:-rdp}"
TURN_PASS="${2:-secret-password}"

echo "============================================"
echo "  Remote Desktop VPS Setup"
echo "============================================"
echo "Dir: $SCRIPT_DIR"
echo ""

# ── 1. coturn ──
echo ">>> Step 1/3: coturn (TURN/STUN server)"
bash "$SCRIPT_DIR/install-coturn.sh" "$TURN_USER" "$TURN_PASS"
echo ""

# ── 2. Web deploy ──
echo ">>> Step 2/3: Web files (nginx + HTTPS)"
bash "$SCRIPT_DIR/deploy-web-https-ip.sh"
echo ""

# ── 3. Relay server systemd service ──
echo ">>> Step 3/3: Relay server (server.py)"

# Find python3
PYTHON=$(which python3 2>/dev/null || which python 2>/dev/null)
if [ -z "$PYTHON" ]; then
    echo "Installing python3..."
    apt-get install -y python3
    PYTHON=$(which python3)
fi

# Install websockets if missing
$PYTHON -c "import websockets" 2>/dev/null || $PYTHON -m pip install websockets

cat > /etc/systemd/system/rdp-relay.service << EOF
[Unit]
Description=Remote Desktop Relay Server
After=network.target coturn.service nginx.service
Wants=coturn.service

[Service]
Type=simple
WorkingDirectory=$SCRIPT_DIR
ExecStart=$PYTHON $SCRIPT_DIR/server.py
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal

# Performance
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable rdp-relay
systemctl restart rdp-relay

echo ""
echo "============================================"
echo "  Setup complete!"
echo "============================================"
echo ""
echo "Services:"
echo "  coturn     — systemctl status coturn"
echo "  nginx      — systemctl status nginx"
echo "  rdp-relay  — systemctl status rdp-relay"
echo ""
echo "Logs:"
echo "  journalctl -u rdp-relay -f"
echo "  tail -f /var/log/turnserver.log"
echo ""
echo "All services auto-start on boot."
