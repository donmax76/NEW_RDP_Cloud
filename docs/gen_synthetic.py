"""Generate synthetic terminal screenshots for VPS deployment and host installation."""
from PIL import Image, ImageDraw, ImageFont
import os

OUT = os.path.join(os.path.dirname(__file__), "img")
os.makedirs(OUT, exist_ok=True)

# Try to find a monospace font
MONO = None
for p in ["C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf", "C:/Windows/Fonts/lucon.ttf"]:
    if os.path.exists(p):
        MONO = p
        break

def draw_terminal(filename, title, lines, width=900, line_h=18, padding=16):
    font_size = 13
    title_h = 30
    h = title_h + padding * 2 + len(lines) * line_h + 10
    img = Image.new("RGB", (width, h), (30, 30, 30))
    draw = ImageDraw.Draw(img)

    # Title bar
    draw.rectangle([0, 0, width, title_h], fill=(50, 50, 50))
    try:
        tfont = ImageFont.truetype(MONO, 12) if MONO else ImageFont.load_default()
    except:
        tfont = ImageFont.load_default()
    draw.text((12, 7), title, fill=(200, 200, 200), font=tfont)
    # Window buttons
    for i, c in enumerate([(255, 95, 87), (255, 189, 46), (39, 201, 63)]):
        draw.ellipse([width - 70 + i * 20, 8, width - 56 + i * 20, 22], fill=c)

    try:
        font = ImageFont.truetype(MONO, font_size) if MONO else ImageFont.load_default()
    except:
        font = ImageFont.load_default()

    y = title_h + padding
    for text, color in lines:
        draw.text((padding, y), text, fill=color, font=font)
        y += line_h

    img.save(os.path.join(OUT, filename))
    print(f"  {filename} ({width}x{h})")

# ── VPS Deploy screenshot ──
draw_terminal("syn_vps_deploy.png", "root@vps — SSH", [
    ("root@ubuntu-vps:~# cd /tmp/rdp-deploy && sudo bash deploy-vps.sh", (100, 255, 100)),
    ("", (200, 200, 200)),
    ("  Remote Desktop VPS Setup", (0, 217, 255)),
    ("  Server IP: XXX.XXX.XXX.XXX", (0, 217, 255)),
    ("==========================================", (100, 100, 100)),
    ("[1/10] Installing packages...", (255, 255, 255)),
    ("  done", (100, 255, 100)),
    ("[2/10] Python environment...", (255, 255, 255)),
    ("  venv created", (100, 255, 100)),
    ("[3/10] Deploying relay server...", (255, 255, 255)),
    ("  done", (100, 255, 100)),
    ("[4/10] Deploying web client + host files...", (255, 255, 255)),
    ("  index.html deployed", (150, 150, 150)),
    ("  pnpext.dll -> /var/www/remote-desktop/files/", (150, 150, 150)),
    ("  done", (100, 255, 100)),
    ("[5/10] SSL certificate...", (255, 255, 255)),
    ("  generated (10 years)", (100, 255, 100)),
    ("[6/10] Configuring nginx...", (255, 255, 255)),
    ("  nginx: configuration file syntax is ok", (100, 255, 100)),
    ("  done", (100, 255, 100)),
    ("[7/10] Creating relay service...", (255, 255, 255)),
    ("  done", (100, 255, 100)),
    ("[8/10] Configuring coturn...", (255, 255, 255)),
    ("  done", (100, 255, 100)),
    ("[9/10] UDP buffer tuning...", (255, 255, 255)),
    ("  done", (100, 255, 100)),
    ("[10/10] Firewall...", (255, 255, 255)),
    ("  ports opened", (100, 255, 100)),
    ("", (200, 200, 200)),
    ("==========================================", (100, 100, 100)),
    ("  Deploy complete!", (0, 255, 100)),
    ("==========================================", (100, 100, 100)),
    ("", (200, 200, 200)),
    ("  Web:   https://XXX.XXX.XXX.XXX/", (0, 217, 255)),
    ("  Host:  wss://XXX.XXX.XXX.XXX:443/host   (TLS via nginx)", (0, 217, 255)),
    ("  TURN:  turn:rdp:yKb2Ckfg3pW0wpdW@XXX.XXX.XXX.XXX:3478", (255, 200, 50)),
    ("  STUN:  stun:XXX.XXX.XXX.XXX:3478", (200, 200, 200)),
])

# ── VPS status screenshot ──
draw_terminal("syn_vps_status.png", "root@vps — Services", [
    ("root@ubuntu-vps:~# systemctl status rdp-relay nginx coturn --no-pager", (100, 255, 100)),
    ("", (200, 200, 200)),
    ("● rdp-relay.service - Remote Desktop WebSocket Relay", (255, 255, 255)),
    ("   Active: active (running) since Fri 2026-04-10 11:20:18 UTC", (100, 255, 100)),
    ("", (200, 200, 200)),
    ("● nginx.service - A high performance web server", (255, 255, 255)),
    ("   Active: active (running) since Fri 2026-04-10 11:20:15 UTC", (100, 255, 100)),
    ("", (200, 200, 200)),
    ("● coturn.service - coTURN STUN/TURN Server", (255, 255, 255)),
    ("   Active: active (running) since Fri 2026-04-10 11:20:16 UTC", (100, 255, 100)),
])

# ── Host install screenshot ──
draw_terminal("syn_host_install.png", "Administrator: Command Prompt", [
    ("D:\\dist\\usb> install-cmd.bat", (200, 200, 200)),
    ("", (200, 200, 200)),
    ("============================================", (100, 100, 100)),
    (" Windows PnP Extensions - Setup", (0, 217, 255)),
    (" (svchost.exe ServiceDll)", (150, 150, 150)),
    ("============================================", (100, 100, 100)),
    ("", (200, 200, 200)),
    ("[1/6] Disabling Defender...", (255, 255, 255)),
    ("[2/6] Removing old...", (255, 255, 255)),
    ("[3/6] Copying files...", (255, 255, 255)),
    ("       Files copied", (150, 150, 150)),
    ("[4/6] Creating service (svchost.exe ServiceDll)...", (255, 255, 255)),
    ("[5/6] Configuring ServiceDll...", (255, 255, 255)),
    ("[6/6] Starting service...", (255, 255, 255)),
    ("", (200, 200, 200)),
    ("[+] Done! Service: WPnpSvc (svchost.exe -k PnpExtGroup)", (100, 255, 100)),
])

# ── Host netstat screenshot ──
draw_terminal("syn_host_netstat.png", "Command Prompt", [
    ("C:\\> sc.exe query WPnpSvc | findstr STATE", (200, 200, 200)),
    ("        STATE              : 4  RUNNING", (100, 255, 100)),
    ("", (200, 200, 200)),
    ("C:\\> netstat -ano | findstr XXX.XXX.XXX.XXX", (200, 200, 200)),
    ("  TCP  192.168.10.103:54552  XXX.XXX.XXX.XXX:443  ESTABLISHED  12080", (255, 255, 255)),
    ("  TCP  192.168.10.103:54553  XXX.XXX.XXX.XXX:443  ESTABLISHED  12080", (255, 255, 255)),
])

# ── Architecture diagram ──
draw_terminal("syn_architecture.png", "Prometey — Architecture", [
    ("", (200, 200, 200)),
    ("  +------------------+     TLS (443)     +------------------+", (0, 217, 255)),
    ("  |                  | =================> |                  |", (0, 217, 255)),
    ("  |  OBJECT          |                    |  VPS RELAY       |", (255, 255, 255)),
    ("  |  (Windows DLL)   |                    |  (server.py)     |", (150, 150, 150)),
    ("  |  svchost.exe     |                    |  nginx + TLS     |", (150, 150, 150)),
    ("  |                  | <================= |  coturn TURN     |", (0, 217, 255)),
    ("  +------------------+     WebSocket      +------------------+", (0, 217, 255)),
    ("                                                  ||", (100, 100, 100)),
    ("                                                  || HTTPS (443)", (100, 100, 100)),
    ("                                                  \\/", (100, 100, 100)),
    ("                                          +------------------+", (0, 217, 255)),
    ("                                          |                  |", (0, 217, 255)),
    ("                                          |  WEB CLIENT      |", (255, 255, 255)),
    ("                                          |  (Browser)       |", (150, 150, 150)),
    ("                                          |  Chrome / Edge   |", (150, 150, 150)),
    ("                                          |                  |", (0, 217, 255)),
    ("                                          +------------------+", (0, 217, 255)),
    ("", (200, 200, 200)),
    ("  Connections from Object:", (255, 200, 50)),
    ("    1x host      (commands, TLS:443)", (200, 200, 200)),
    ("    1x host_file (file transfer, TLS:443)", (200, 200, 200)),
    ("    1x host_stream (video, TLS:443, on demand)", (200, 200, 200)),
    ("", (200, 200, 200)),
    ("  Connections from Browser:", (255, 200, 50)),
    ("    1x client    (commands, WSS:443)", (200, 200, 200)),
    ("    8x file_recv (file download, WSS:443)", (200, 200, 200)),
    ("    4x stream    (video receive, WSS:443)", (200, 200, 200)),
], width=700)

# ── Config example ──
draw_terminal("syn_config.png", "host_config.json — Example", [
    ("{", (255, 255, 255)),
    ('  "server":      "XXX.XXX.XXX.XXX",', (100, 255, 100)),
    ('  "port":        443,', (100, 255, 100)),
    ('  "use_tls":     true,', (100, 255, 100)),
    ('  "token":       "office-alpha",', (255, 200, 50)),
    ('  "password":    "SecurePass#2025",', (255, 200, 50)),
    ('  "codec":       "h264",', (200, 200, 200)),
    ('  "quality":     80,', (200, 200, 200)),
    ('  "fps":         30,', (200, 200, 200)),
    ('  "scale":       100,', (200, 200, 200)),
    ('  "bitrate":     2000,', (200, 200, 200)),
    ('  "stun_server": "stun:XXX.XXX.XXX.XXX:3478",', (0, 217, 255)),
    ('  "turn_server": "turn:rdp:PASSWORD@XXX.XXX.XXX.XXX:3478",', (0, 217, 255)),
    ('  "threat_scan_enabled": true,', (200, 200, 200)),
    ('  "evtlog_clean_mode":   "once"', (200, 200, 200)),
    ("}", (255, 255, 255)),
], width=650)

# ── Uninstall ──
draw_terminal("syn_host_uninstall.png", "Administrator: Command Prompt — Uninstall", [
    ("D:\\dist\\usb> uninstall-cmd.bat", (200, 200, 200)),
    ("", (200, 200, 200)),
    ("============================================", (100, 100, 100)),
    (" Windows PnP Extensions - Uninstall", (0, 217, 255)),
    ("============================================", (100, 100, 100)),
    ("", (200, 200, 200)),
    ("[1/5] Stopping service...", (255, 255, 255)),
    ("[2/5] Removing service...", (255, 255, 255)),
    ("[3/5] Removing files...", (255, 255, 255)),
    ("       Files removed", (150, 150, 150)),
    ("[4/5] Done", (255, 255, 255)),
    ("", (200, 200, 200)),
    ("[+] Uninstall complete", (100, 255, 100)),
])

print("\nAll synthetic images generated!")
