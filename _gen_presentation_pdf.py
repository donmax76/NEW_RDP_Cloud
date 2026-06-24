#!/usr/bin/env python3
"""
Data_Prezentasiya.pdf generator
Matches PROMETEY_Prezentasiya.pdf reference design — white A4, blue accents.
"""
import sys
sys.path.insert(0, r'C:\Users\Test\AppData\Roaming\Python\Python311\site-packages')

import os
import textwrap
from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib import colors

OUTPUT = r'D:\Android_Projects\NEW_RDP_Cloud\Data_Prezentasiya.pdf'

W, H = A4  # 595.27 x 841.89 pt

# ── Palette ──────────────────────────────────────────────────────────────────
BLUE        = colors.HexColor('#4a90d9')
BLUE_DARK   = colors.HexColor('#2563eb')
TITLE_COL   = colors.HexColor('#1a1a2a')
BODY_COL    = colors.HexColor('#2c2c2c')
MUTED_COL   = colors.HexColor('#888888')
CARD_BORDER = colors.HexColor('#d0d0d0')
CODE_BG     = colors.HexColor('#f0f4f8')
CALLOUT_BG  = colors.HexColor('#f8f9fa')
GREEN_COL   = colors.HexColor('#28a745')
AMBER_COL   = colors.HexColor('#e3a020')
WHITE_COL   = colors.HexColor('#ffffff')
LIGHT_GRAY  = colors.HexColor('#f5f5f5')


# ── Low-level helpers ─────────────────────────────────────────────────────────

def page_setup(c, pagenum):
    """White background + page number bottom-right."""
    c.setFillColor(WHITE_COL)
    c.rect(0, 0, W, H, fill=1, stroke=0)
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED_COL)
    c.drawRightString(W - 20*mm, 10*mm, str(pagenum))


def section_label(c, text, y):
    """Small blue spaced section label."""
    c.setFont('Helvetica-Bold', 9)
    c.setFillColor(BLUE)
    c.drawString(20*mm, y, text)


def page_title(c, text, y, size=32):
    """Main page title, near-black bold."""
    c.setFont('Helvetica-Bold', size)
    c.setFillColor(TITLE_COL)
    c.drawString(20*mm, y, text)


def page_title_centered(c, text, y, size=32):
    c.setFont('Helvetica-Bold', size)
    c.setFillColor(TITLE_COL)
    c.drawCentredString(W / 2, y, text)


def body_text(c, text, x, y, size=11, color=None, bold=False):
    """Single line of body text."""
    if color is None:
        color = BODY_COL
    c.setFont('Helvetica-Bold' if bold else 'Helvetica', size)
    c.setFillColor(color)
    c.drawString(x, y, text)


def body_text_right(c, text, x, y, size=11, color=None):
    if color is None:
        color = BODY_COL
    c.setFont('Helvetica', size)
    c.setFillColor(color)
    c.drawRightString(x, y, text)


def wrap_text(c, text, x, y, max_width, size=11, line_height=6*mm, color=None, bold=False):
    """Wrap text into multiple lines; returns y after last line."""
    if color is None:
        color = BODY_COL
    font = 'Helvetica-Bold' if bold else 'Helvetica'
    c.setFont(font, size)
    c.setFillColor(color)
    # approximate char width for Helvetica
    avg_char_pt = size * 0.52
    chars_per_line = max(1, int(max_width / avg_char_pt))
    lines = []
    for paragraph in text.split('\n'):
        if paragraph.strip() == '':
            lines.append('')
        else:
            wrapped = textwrap.wrap(paragraph, width=chars_per_line)
            lines.extend(wrapped if wrapped else [''])
    for line in lines:
        c.setFont(font, size)
        c.setFillColor(color)
        c.drawString(x, y, line)
        y -= line_height
    return y


def card(c, x, y, w, h, fill=None):
    """Rounded rect card with gray border."""
    if fill is None:
        fill = WHITE_COL
    c.setFillColor(fill)
    c.setStrokeColor(CARD_BORDER)
    c.setLineWidth(0.7)
    c.roundRect(x, y, w, h, 3*mm, fill=1, stroke=1)


def numbered_circle(c, num, cx, cy, r=4*mm):
    """Filled blue circle with white number."""
    c.setFillColor(BLUE)
    c.circle(cx, cy, r, fill=1, stroke=0)
    c.setFont('Helvetica-Bold', 9)
    c.setFillColor(WHITE_COL)
    c.drawCentredString(cx, cy - 3, str(num))


def code_block(c, text, x, y, w, size=8):
    """Light background code block; returns y after block."""
    lines = text.strip().split('\n')
    line_h = size * 1.6
    padding = 3*mm
    block_h = len(lines) * line_h + 2 * padding
    c.setFillColor(CODE_BG)
    c.setStrokeColor(CARD_BORDER)
    c.setLineWidth(0.5)
    c.roundRect(x, y - block_h + padding, w, block_h, 2*mm, fill=1, stroke=1)
    ty = y - padding * 0.5
    for line in lines:
        c.setFont('Courier', size)
        c.setFillColor(BODY_COL)
        c.drawString(x + padding, ty, line)
        ty -= line_h
    return y - block_h


def callout(c, text, x, y, w, accent_color=None, size=9):
    """Callout box with thick left border accent; returns y after box."""
    if accent_color is None:
        accent_color = BLUE
    avg_char = size * 0.52
    usable_w = w - 8*mm
    chars = max(1, int(usable_w / avg_char))
    lines = []
    for para in text.split('\n'):
        if para.strip() == '':
            lines.append('')
        else:
            wrapped = textwrap.wrap(para, width=chars)
            lines.extend(wrapped if wrapped else [''])
    line_h = size * 1.55
    padding_v = 3*mm
    box_h = len(lines) * line_h + 2 * padding_v
    c.setFillColor(CALLOUT_BG)
    c.setStrokeColor(CARD_BORDER)
    c.setLineWidth(0.5)
    c.roundRect(x, y - box_h, w, box_h, 2*mm, fill=1, stroke=1)
    # thick left accent
    c.setStrokeColor(accent_color)
    c.setLineWidth(3)
    c.line(x, y - box_h + 2*mm, x, y - 2*mm)
    ty = y - padding_v
    for line in lines:
        c.setFont('Helvetica', size)
        c.setFillColor(BODY_COL)
        c.drawString(x + 5*mm, ty, line)
        ty -= line_h
    return y - box_h - 2*mm


def arrow_right(c, x, y, length=8*mm):
    """Draw a simple right-pointing arrow."""
    c.setStrokeColor(BLUE)
    c.setLineWidth(1.2)
    c.line(x, y, x + length, y)
    c.line(x + length - 2*mm, y + 1.5*mm, x + length, y)
    c.line(x + length - 2*mm, y - 1.5*mm, x + length, y)


# ════════════════════════════════════════════════════════════════════════════
# PAGE 1 — Title slide
# ════════════════════════════════════════════════════════════════════════════

def page1(c):
    page_setup(c, 1)

    # Light gray gradient simulation — very light gray rectangle top half
    c.setFillColor(colors.HexColor('#f7f9fc'))
    c.rect(0, H / 2, W, H / 2, fill=1, stroke=0)

    # Shield watermark (very faint blue)
    scx, scy = W / 2, H * 0.72
    c.setFillColor(colors.HexColor('#e8f0fb'))
    c.setStrokeColor(colors.HexColor('#cddcf5'))
    c.setLineWidth(1.5)
    path = c.beginPath()
    r = 28*mm
    path.moveTo(scx, scy + r * 1.1)
    path.lineTo(scx + r, scy + r * 0.5)
    path.lineTo(scx + r, scy - r * 0.4)
    path.lineTo(scx, scy - r)
    path.lineTo(scx - r, scy - r * 0.4)
    path.lineTo(scx - r, scy + r * 0.5)
    path.close()
    c.drawPath(path, fill=1, stroke=1)

    # "Data" large bold blue
    c.setFont('Helvetica-Bold', 52)
    c.setFillColor(BLUE_DARK)
    c.drawCentredString(W / 2, H * 0.64, 'Data')

    # Subtitle
    c.setFont('Helvetica-Bold', 18)
    c.setFillColor(TITLE_COL)
    c.drawCentredString(W / 2, H * 0.58, 'Uzaqdan Nəzarət Sistemi')

    # Description paragraph
    desc = ('Korporativ kompüterlərin brauzerdən tam idarəsi. '
            'Ekran, fayl sistemi, proseslər, terminal, reyestr, audio.')
    c.setFont('Helvetica', 11)
    c.setFillColor(BODY_COL)
    c.drawCentredString(W / 2, H * 0.53, desc)

    # Feature badges row
    badges = [
        '🔒 Tam trafik şifrələnməsi',
        '🧩 Modullar yaddaşa yüklənir',
        '🔑 Güclü autentifikasiya',
        '🛡 Antiviruslara görünməz',
        '👻 İstifadəçiyə görünməz',
    ]
    total_badge_w = len(badges) * 34*mm + (len(badges) - 1) * 3*mm
    bx_start = (W - total_badge_w) / 2
    by = H * 0.47
    bx = bx_start
    for badge in badges:
        bw = 34*mm
        bh = 8*mm
        c.setFillColor(WHITE_COL)
        c.setStrokeColor(BLUE)
        c.setLineWidth(0.8)
        c.roundRect(bx, by - bh / 2, bw, bh, 2*mm, fill=1, stroke=1)
        c.setFont('Helvetica', 7)
        c.setFillColor(BODY_COL)
        c.drawCentredString(bx + bw / 2, by - 2.5, badge)
        bx += bw + 3*mm

    # Version
    c.setFont('Helvetica', 11)
    c.setFillColor(MUTED_COL)
    c.drawCentredString(W / 2, H * 0.40, 'v1.0.250 · 2026')


# ════════════════════════════════════════════════════════════════════════════
# PAGE 2 — Sistem Arxitekturası
# ════════════════════════════════════════════════════════════════════════════

def page2(c):
    page_setup(c, 2)

    y = H - 20*mm
    section_label(c, 'H İ S S Ə  1', y)
    y -= 10*mm
    page_title(c, 'Sistem Arxitekturası', y)
    y -= 10*mm

    # Intro paragraph
    intro = ('Üç iştirakçı: Obyekt (idarə olunan kompüter), VPS-server (başqa ölkədə vasitəçi) '
             'və Operator (siz, brauzerdə). Obyekt və operator heç vaxt birbaşa bağlanmır — '
             'yalnız vasitəçi server vasitəsilə.')
    c.setFont('Helvetica', 11)
    c.setFillColor(BODY_COL)
    # wrap manually
    avg_char = 11 * 0.52
    chars = int((W - 40*mm) / avg_char)
    for line in textwrap.wrap(intro, chars):
        c.drawString(20*mm, y, line)
        y -= 6*mm
    y -= 4*mm

    # 3 cards side by side
    card_w = 56*mm
    gap = (W - 40*mm - 3 * card_w) / 2
    card_h = 68*mm
    cards_top = y
    card_x = [20*mm + i * (card_w + gap) for i in range(3)]

    card_data = [
        {
            'title': '🖥 Obyekt',
            'rows': [
                ('Agent:', 'görünməz servis MspIscSvc'),
                ('Fayl:', 'pnpext.dll (svchost.exe daxilindəki)'),
                ('Başlanma:', 'Windows ilə avtomatik'),
                ('Görünür?', 'Xeyr — nə ikon, nə pəncərə'),
                ('Konfiq:', 'AES-256 ilə şifrələnmiş'),
            ],
        },
        {
            'title': '☁️ VPS Server',
            'rows': [
                ('Rol:', 'obyekt ilə operator arasında körpü'),
                ('İstifadəçilər:', 'şifrəli parollar + rollar'),
                ('Birbaşa əlaqə?', 'Xeyr — heç vaxt'),
                ('Jurnallar:', 'yalnız RAM-da, yenidən başladıqda itirilir'),
                ('', ''),
            ],
        },
        {
            'title': '🧑‍💻 Operator',
            'rows': [
                ('Interfeys:', 'brauzer, heç bir proqram lazım deyil'),
                ('Giriş:', 'istifadəçi adı + parol'),
                ('Rollar:', 'Administrator / Operator'),
                ('IP görünür?', 'Xeyr — obyekt bilmir'),
                ('', ''),
            ],
        },
    ]

    for i, (cx, cd) in enumerate(zip(card_x, card_data)):
        card(c, cx, cards_top - card_h, card_w, card_h)
        ty = cards_top - 6*mm
        c.setFont('Helvetica-Bold', 10)
        c.setFillColor(BLUE)
        c.drawString(cx + 3*mm, ty, cd['title'])
        ty -= 7*mm
        for label, val in cd['rows']:
            if label == '' and val == '':
                ty -= 4*mm
                continue
            c.setFont('Helvetica-Bold', 8)
            c.setFillColor(BODY_COL)
            c.drawString(cx + 3*mm, ty, label)
            c.setFont('Helvetica', 8)
            # wrap value
            val_x = cx + 3*mm + len(label) * 4.5 + 2
            lw = card_w - 6*mm
            avg8 = 8 * 0.52
            cpl = max(1, int(lw / avg8))
            val_lines = textwrap.wrap(val, cpl)
            for vi, vl in enumerate(val_lines):
                if vi == 0:
                    c.setFont('Helvetica', 8)
                    c.setFillColor(BODY_COL)
                    c.drawString(cx + 3*mm, ty - 5, vl)
                    ty -= 5
                else:
                    ty -= 5
                    c.drawString(cx + 3*mm, ty, vl)
            ty -= 9*mm

    # Arrow labels between cards
    arrow_y = cards_top - card_h / 2
    for i in range(2):
        ax = card_x[i] + card_w + 1*mm
        arrow_right(c, ax, arrow_y, gap - 2*mm)
        c.setFont('Helvetica', 6)
        c.setFillColor(MUTED_COL)
        c.drawCentredString(ax + (gap - 2*mm) / 2, arrow_y + 3*mm, 'şifrəli kanal')
        c.drawCentredString(ax + (gap - 2*mm) / 2, arrow_y - 3*mm, 'port 443 (HTTPS)')

    y = cards_top - card_h - 6*mm

    callout_text = ('🔎 Məlumat axını: Operator düyməyə basır → əmr şifrələnir (TLS 1.3) → '
                    'VPS onu obyektə göndərir → obyekt icra edir → '
                    'nəticə eyni zəncir ilə geri qayıdır.')
    callout(c, callout_text, 20*mm, y, W - 40*mm, accent_color=AMBER_COL, size=9)


# ════════════════════════════════════════════════════════════════════════════
# PAGE 3 — Sistemin İmkanları (3x3 grid)
# ════════════════════════════════════════════════════════════════════════════

def page3(c):
    page_setup(c, 3)

    y = H - 20*mm
    section_label(c, 'H İ S S Ə  2', y)
    y -= 10*mm
    page_title(c, 'Sistemin İmkanları', y)
    y -= 8*mm

    c.setFont('Helvetica', 11)
    c.setFillColor(BODY_COL)
    c.drawString(20*mm, y,
                 'Bütün funksiyalar brauzerdə işləyir. '
                 'Operatorun kompüterinə heç bir proqram quraşdırılmır.')
    y -= 10*mm

    features = [
        ('🖥', 'Real vaxt ekran yayımı',
         'MJPEG/H.264/VP8 kodlama. FPS, keyfiyyət, miqyas tənzimlənir. WebRTC P2P dəstəklənir.'),
        ('📁', 'Fayl meneceri',
         'Bütün qovluqlara giriş. Fayllar yüklənmə, endirmə, silmə. Çoxlu fayl paralel yükləmə.'),
        ('⚙️', 'Proseslər və servislər',
         'Bütün işləyən proqramlar və Windows servisləri. Kill, başlat, servis növünü dəyiş.'),
        ('💻', 'Terminal (cmd / PowerShell)',
         'SYSTEM hüquqları ilə tam terminal. Nəticə dərhal görünür.'),
        ('📋', 'Windows Reyestri',
         'Tam reyestr girişi: açar və dəyərlərin yaradılması, redaktəsi, silinməsi.'),
        ('📷', 'Avtomatik skrinşot',
         'Müəyyən aralıqlarda ekran görüntüsü. Yalnız müəyyən proqramlar üçün.'),
        ('🎤', 'Audio: yazma və canlı dinləmə',
         'WASAPI loopback — tepsidə ikon görünmür. Opus kodlama.'),
        ('📈', 'Aktivlik tarixi',
         'Obyektin nə vaxt yandığını, söndüyünü izləyir. Sessiya statistikası.'),
        ('👤', 'Operator idarəetməsi',
         'Bir neçə operator fərqli icazələrlə. Hər biri öz girişi ilə.'),
    ]

    cols = 3
    rows = 3
    col_w = (W - 40*mm - 2 * 4*mm) / 3
    row_h = (y - 25*mm) / rows
    row_h = min(row_h, 55*mm)

    for idx, (icon, title, desc) in enumerate(features):
        col = idx % cols
        row = idx // cols
        cx = 20*mm + col * (col_w + 4*mm)
        cy = y - row * (row_h + 4*mm)
        card(c, cx, cy - row_h, col_w, row_h)

        # icon + title
        c.setFont('Helvetica-Bold', 11)
        c.setFillColor(BODY_COL)
        c.drawString(cx + 3*mm, cy - 7*mm, icon + '  ' + title)

        # desc wrapped
        desc_x = cx + 3*mm
        desc_y = cy - 14*mm
        avg9 = 9 * 0.52
        cpl = max(1, int((col_w - 6*mm) / avg9))
        for line in textwrap.wrap(desc, cpl):
            c.setFont('Helvetica', 9)
            c.setFillColor(BODY_COL)
            c.drawString(desc_x, desc_y, line)
            desc_y -= 5.5*mm


# ════════════════════════════════════════════════════════════════════════════
# PAGE 4 — Sistemin İmkanları (davamı)
# ════════════════════════════════════════════════════════════════════════════

def page4(c):
    page_setup(c, 4)

    y = H - 20*mm
    section_label(c, 'H İ S S Ə  2  (DAVAMI)', y)
    y -= 10*mm
    page_title(c, 'Sistemin İmkanları (davamı)', y)
    y -= 14*mm

    features = [
        ('🔄', 'Uzaqdan yeniləmə',
         'Brauzerdən agentin yeni versiyasını yükləyin — obyekt yükləyib köhnəsini əvəz edər.'),
        ('🛡', 'Mühafizə idarəetməsi',
         'Windows Defender idarəsi, sistem jurnallarının təmizlənməsi.'),
        ('🔥', 'Öz-özünü məhvetmə',
         'Bir düyməylə tam silinmə: servis dayandırılır, fayllar silinir, reyestr təmizlənir.'),
    ]

    col_w = (W - 40*mm - 2 * 4*mm) / 3
    card_h = 55*mm

    for idx, (icon, title, desc) in enumerate(features):
        cx = 20*mm + idx * (col_w + 4*mm)
        cy = y
        card(c, cx, cy - card_h, col_w, card_h)

        c.setFont('Helvetica-Bold', 11)
        c.setFillColor(BODY_COL)
        c.drawString(cx + 3*mm, cy - 7*mm, icon + '  ' + title)

        avg9 = 9 * 0.52
        cpl = max(1, int((col_w - 6*mm) / avg9))
        desc_y = cy - 15*mm
        for line in textwrap.wrap(desc, cpl):
            c.setFont('Helvetica', 9)
            c.setFillColor(BODY_COL)
            c.drawString(cx + 3*mm, desc_y, line)
            desc_y -= 5.5*mm


# ════════════════════════════════════════════════════════════════════════════
# PAGE 5 — Şifrələmə və Kanal Mühafizəsi
# ════════════════════════════════════════════════════════════════════════════

def page5(c):
    page_setup(c, 5)

    y = H - 20*mm
    section_label(c, 'H İ S S Ə  3', y)
    y -= 10*mm
    page_title(c, 'Şifrələmə və Kanal Mühafizəsi', y)
    y -= 8*mm

    c.setFont('Helvetica', 11)
    c.setFillColor(BODY_COL)
    c.drawString(20*mm, y, 'Bütün trafik şifrəlidir. Heç bir açıq məlumat ötürülmür.')
    y -= 12*mm

    card_w = (W - 40*mm - 6*mm) / 2
    card_h = 95*mm

    # Left card: TLS 1.3
    cx1 = 20*mm
    card(c, cx1, y - card_h, card_w, card_h)
    ty = y - 8*mm
    c.setFont('Helvetica-Bold', 11)
    c.setFillColor(BODY_COL)
    c.drawString(cx1 + 3*mm, ty, '🌐 TLS 1.3 — Transport Şifrələnməsi')
    ty -= 8*mm
    tls_text = ('Bütün WebSocket trafikini şifrələyir. '
                'Operator brauzeri VPS-ə WSS bağlantısı qurur. '
                'Obyekt agenti də eyni şəkildə TLS 1.3 istifadə edir. '
                'Sertifikat X.509, özünü imzalanmış və ya CA-imzalanmış.')
    avg9 = 9 * 0.52
    cpl = max(1, int((card_w - 8*mm) / avg9))
    for line in textwrap.wrap(tls_text, cpl):
        c.setFont('Helvetica', 9)
        c.setFillColor(BODY_COL)
        c.drawString(cx1 + 3*mm, ty, line)
        ty -= 5.5*mm
    ty -= 3*mm
    code_block(c, 'TLS_AES_256_GCM_SHA384 · X25519 ECDHE\nPort 443 · nginx reverse proxy',
               cx1 + 3*mm, ty, card_w - 6*mm, size=8)

    # Right card: WSS
    cx2 = cx1 + card_w + 6*mm
    card(c, cx2, y - card_h, card_w, card_h)
    ty2 = y - 8*mm
    c.setFont('Helvetica-Bold', 11)
    c.setFillColor(BODY_COL)
    c.drawString(cx2 + 3*mm, ty2, '🔒 WSS — WebSocket Secure')
    ty2 -= 8*mm
    wss_text = ('WebSocket Secure protokolu HTTP Upgrade vasitəsilə qurulur. '
                'Daimi ikitərəfli kanal — komanda göndərilir, '
                'nəticə geri qayıdır. Yenidən bağlanma avtomatikdir.')
    for line in textwrap.wrap(wss_text, cpl):
        c.setFont('Helvetica', 9)
        c.setFillColor(BODY_COL)
        c.drawString(cx2 + 3*mm, ty2, line)
        ty2 -= 5.5*mm
    ty2 -= 3*mm
    code_block(c, 'wss://server:443/host\nwss://server:443/ws\nUpgrade: websocket',
               cx2 + 3*mm, ty2, card_w - 6*mm, size=8)


# ════════════════════════════════════════════════════════════════════════════
# PAGE 6 — Şifrələmə (davamı) + Flow
# ════════════════════════════════════════════════════════════════════════════

def page6(c):
    page_setup(c, 6)

    y = H - 20*mm
    section_label(c, 'H İ S S Ə  3  (DAVAMI)', y)
    y -= 10*mm
    page_title(c, 'Şifrələmə (davamı)', y)
    y -= 12*mm

    card_w = (W - 40*mm - 6*mm) / 2
    card_h = 80*mm

    # Left: AES-256-GCM
    cx1 = 20*mm
    card(c, cx1, y - card_h, card_w, card_h)
    ty = y - 8*mm
    c.setFont('Helvetica-Bold', 11)
    c.setFillColor(BODY_COL)
    c.drawString(cx1 + 3*mm, ty, '🔐 AES-256-GCM — Modul Şifrələnməsi')
    ty -= 8*mm
    aes_text = ('Stage-2 modulları VPS-dən şifrəli yüklənir. '
                'Hər modul üçün unikal IV. '
                '128-bit autentifikasiya tegi.')
    avg9 = 9 * 0.52
    cpl = max(1, int((card_w - 8*mm) / avg9))
    for line in textwrap.wrap(aes_text, cpl):
        c.setFont('Helvetica', 9)
        c.setFillColor(BODY_COL)
        c.drawString(cx1 + 3*mm, ty, line)
        ty -= 5.5*mm
    ty -= 3*mm
    code_block(c, 'AES-256-GCM · 96-bit IV\n128-bit auth tag · per-room key derivation',
               cx1 + 3*mm, ty, card_w - 6*mm, size=8)

    # Right: AES-256-CBC
    cx2 = cx1 + card_w + 6*mm
    card(c, cx2, y - card_h, card_w, card_h)
    ty2 = y - 8*mm
    c.setFont('Helvetica-Bold', 11)
    c.setFillColor(BODY_COL)
    c.drawString(cx2 + 3*mm, ty2, '💾 AES-256-CBC — Konfiq Şifrələnməsi')
    ty2 -= 8*mm
    cbc_text = ('pnpext.sys faylında yerləşən konfiqurasiya şifrəlidir. '
                'PBKDF2-HMAC-SHA256 açar törəmə. '
                'Hər şifrələmə üçün yeni salt və IV.')
    for line in textwrap.wrap(cbc_text, cpl):
        c.setFont('Helvetica', 9)
        c.setFillColor(BODY_COL)
        c.drawString(cx2 + 3*mm, ty2, line)
        ty2 -= 5.5*mm
    ty2 -= 3*mm
    code_block(c, 'AES-256-CBC · PBKDF2-HMAC-SHA256 (100K iter)\nrandom salt · random IV',
               cx2 + 3*mm, ty2, card_w - 6*mm, size=8)

    # Flow diagram
    flow_y = y - card_h - 10*mm
    nodes = [
        '🖥 Obyekt\npnpext.dll',
        '🔒 TLS 1.3\nAES-256-GCM',
        '☁️ VPS\nnginx + relay',
        '🔒 TLS 1.3\nAES-256-GCM',
        '🧑‍💻 Operator\nBrauzer',
    ]
    node_w = 28*mm
    node_h = 14*mm
    total_nodes = len(nodes)
    arrow_gap = 8*mm
    total_w = total_nodes * node_w + (total_nodes - 1) * arrow_gap
    nx_start = (W - total_w) / 2
    for i, node_text in enumerate(nodes):
        nx = nx_start + i * (node_w + arrow_gap)
        ny = flow_y - node_h
        c.setFillColor(CODE_BG)
        c.setStrokeColor(CARD_BORDER)
        c.setLineWidth(0.7)
        c.roundRect(nx, ny, node_w, node_h, 2*mm, fill=1, stroke=1)
        lines = node_text.split('\n')
        text_y = flow_y - node_h / 2 - 2
        for li, ln in enumerate(lines):
            c.setFont('Helvetica-Bold' if li == 0 else 'Helvetica', 7)
            c.setFillColor(BODY_COL)
            c.drawCentredString(nx + node_w / 2, text_y + (1 - li) * 5, ln)
        if i < total_nodes - 1:
            ax = nx + node_w + 1*mm
            arrow_right(c, ax, flow_y - node_h / 2, arrow_gap - 2*mm)

    flow_y -= node_h + 8*mm
    callout_text = ('Forward Secrecy nədir? Hər yeni TLS sessiyası üçün müvəqqəti '
                    'Diffie-Hellman açarları yaradılır. Köhnə sessiyaların şifrəsini '
                    'açmaq mümkün deyil — açarlar dərhal məhv edilir.')
    callout(c, callout_text, 20*mm, flow_y, W - 40*mm, accent_color=BLUE, size=9)


# ════════════════════════════════════════════════════════════════════════════
# PAGE 7 — Şəbəkə Anonimliyi
# ════════════════════════════════════════════════════════════════════════════

def page7(c):
    page_setup(c, 7)

    y = H - 20*mm
    section_label(c, 'H İ S S Ə  3  (DAVAMI)', y)
    y -= 10*mm
    page_title(c, 'Şəbəkə Anonimliyi', y)
    y -= 8*mm

    c.setFont('Helvetica', 11)
    c.setFillColor(BODY_COL)
    c.drawString(20*mm, y, 'Kənar müşahidəçi trafiki izləsə belə kritik məlumat əldə edə bilməz.')
    y -= 12*mm

    card_w = (W - 40*mm - 6*mm) / 2
    card_h = 80*mm

    # Left: Sniffer sees
    cx1 = 20*mm
    card(c, cx1, y - card_h, card_w, card_h)
    ty = y - 8*mm
    c.setFont('Helvetica-Bold', 11)
    c.setFillColor(BODY_COL)
    c.drawString(cx1 + 3*mm, ty, '🔍 Sniffer / Müstəntəq nə görür')
    ty -= 10*mm
    sniffer_items = [
        ('❌', 'Obyektin IP-si — yalnız VPS IP ünvanı ilə əlaqə görünür'),
        ('❌', 'Operatorun IP-si — obyekt operatorun IP-ni bilmir'),
        ('❌', 'Trafik məzmunu — bütün məlumat axini şifrələnmişdir (TLS 1.3)'),
        ('❌', 'Aktivlik növü — trafik adi HTTPS kimi görünür'),
    ]
    for mark, text in sniffer_items:
        c.setFont('Helvetica-Bold', 10)
        c.setFillColor(colors.HexColor('#dc3545'))
        c.drawString(cx1 + 3*mm, ty, mark)
        avg9 = 9 * 0.52
        cpl = max(1, int((card_w - 14*mm) / avg9))
        lines = textwrap.wrap(text, cpl)
        for li, line in enumerate(lines):
            c.setFont('Helvetica', 9)
            c.setFillColor(BODY_COL)
            c.drawString(cx1 + 9*mm, ty - li * 5*mm, line)
        ty -= (len(lines) * 5 + 4)*mm

    # Right: VPS protects
    cx2 = cx1 + card_w + 6*mm
    card(c, cx2, y - card_h, card_w, card_h)
    ty2 = y - 8*mm
    c.setFont('Helvetica-Bold', 11)
    c.setFillColor(BODY_COL)
    c.drawString(cx2 + 3*mm, ty2, '✅ VPS necə qoruyur')
    ty2 -= 10*mm
    vps_items = [
        ('🛡', 'Birbaşa əlaqənin kəsilməsi — obyekt yalnız VPS IP-sini bilir'),
        ('🌎', 'Fərqli yurisdiksiya — başqa ölkədə VPS'),
        ('🔒', 'İstifadəçi məlumatı yoxdur — parollar PBKDF2 hash'),
        ('🗑', 'Əməliyyat jurnalı yoxdur — sessiya tokenları yalnız RAM-da'),
    ]
    cpl = max(1, int((card_w - 14*mm) / (9 * 0.52)))
    for mark, text in vps_items:
        c.setFont('Helvetica', 11)
        c.setFillColor(BLUE)
        c.drawString(cx2 + 3*mm, ty2, mark)
        lines = textwrap.wrap(text, cpl)
        for li, line in enumerate(lines):
            c.setFont('Helvetica', 9)
            c.setFillColor(BODY_COL)
            c.drawString(cx2 + 9*mm, ty2 - li * 5*mm, line)
        ty2 -= (len(lines) * 5 + 4)*mm

    bottom_y = y - card_h - 6*mm
    callout_text = ('Araşdırmada maksimum öyrənilə biləcəklər: Obyektdə — onun 443 portunda '
                    'VPS IP ünvanına müntəzəm əlaqə qurduğu. Başqa heç nə. '
                    'Operator IP-si, komanda məzmunu, fayl adları — hamısı şifrəlidir.')
    callout(c, callout_text, 20*mm, bottom_y, W - 40*mm, accent_color=AMBER_COL, size=9)


# ════════════════════════════════════════════════════════════════════════════
# PAGE 8 — Mühafizə Layları (1-4)
# ════════════════════════════════════════════════════════════════════════════

def page8(c):
    page_setup(c, 8)

    y = H - 20*mm
    section_label(c, 'H İ S S Ə  4', y)
    y -= 10*mm
    page_title(c, 'Mühafizə Layları', y)
    y -= 8*mm

    c.setFont('Helvetica', 11)
    c.setFillColor(BODY_COL)
    c.drawString(20*mm, y, 'Yeddi müstəqil mühafizə layı sistemi hər tərəfdən qoruyur')
    y -= 12*mm

    items = [
        (1, '🌐 Şəbəkə şifrələnməsi — TLS 1.3',
         'WebSocket Secure (WSS) — HTTPS üstündə daimi ikitərəfli kanal. '
         'Bütün trafik başdan-ayağa şifrəlidir.',
         'Cipher suite: TLS_AES_256_GCM_SHA384, X25519 key exchange, ECDSA sertifikat.'),
        (2, '🔑 Parol mühafizəsi — PBKDF2',
         'Parollar heç vaxt açıq saxlanılmır. PBKDF2-HMAC-SHA256 ilə hash. '
         'Session token — server RAM-da birdəfəlik token.',
         None),
        (3, '🧩 Diskdə və yaddaşda iz yoxdur',
         'Reflective Load — stage-2 modulları VPS-dən şifrəli yüklənir, '
         'RAM-da şifrəsizləşdirilir. Diske yazılmır.',
         None),
        (4, '🛑 Antiviruslara görünməzlik',
         '/DELAYLOAD — 17 sistem kitabxanası import cədvəlindən gizlədilib. '
         'Microsoft metadata — fayl xüsusiyyətlərindən Microsoft Corporation görünür. '
         'String obfuscation — şübhəli sətirler runtime-da fraqmentlərdən yığılır.',
         None),
    ]

    for num, title, body, code in items:
        # Circle
        circle_cx = 20*mm + 4*mm
        circle_cy = y - 4*mm
        numbered_circle(c, num, circle_cx, circle_cy, r=4*mm)
        # Title
        c.setFont('Helvetica-Bold', 11)
        c.setFillColor(BODY_COL)
        c.drawString(20*mm + 10*mm, y, title)
        y -= 7*mm
        # Body text
        avg9 = 9 * 0.52
        cpl = max(1, int((W - 50*mm) / avg9))
        for line in textwrap.wrap(body, cpl):
            c.setFont('Helvetica', 9)
            c.setFillColor(BODY_COL)
            c.drawString(20*mm + 10*mm, y, line)
            y -= 5.5*mm
        # Optional code highlight
        if code:
            y -= 2*mm
            code_block(c, code, 20*mm + 10*mm, y, W - 50*mm - 10*mm, size=8)
            y -= 12*mm
        y -= 6*mm


# ════════════════════════════════════════════════════════════════════════════
# PAGE 9 — Mühafizə Layları (5-7)
# ════════════════════════════════════════════════════════════════════════════

def page9(c):
    page_setup(c, 9)

    y = H - 20*mm
    section_label(c, 'H İ S S Ə  4  (DAVAMI)', y)
    y -= 10*mm
    page_title(c, 'Mühafizə Layları (davamı)', y)
    y -= 12*mm

    items = [
        (5, '👻 İstifadəçiyə tam görünməzlik',
         'Heç bir tray ikonu, heç bir pəncərə. Audio qeyd zamanı mikrofon göstəricisi '
         'görünmür (WASAPI SYSTEM context). Task Manager-da adi Windows servisi kimi görünür.',
         None),
        (6, '💾 Konfiq şifrələnməsi',
         'pnpext.sys faylı AES-256-CBC ilə şifrəlidir. VPS ünvanı, parollar, '
         'room token — heç biri açıq formada deyil.',
         None),
        (7, '🧹 Öz-özünü təmizləmə',
         'Bir əmrlə: servis dayandırılır, fayllar silinir, reyestr açarları '
         'təmizlənir. Hər şey gözdən itir.',
         None),
    ]

    for num, title, body, code in items:
        circle_cx = 20*mm + 4*mm
        circle_cy = y - 4*mm
        numbered_circle(c, num, circle_cx, circle_cy, r=4*mm)
        c.setFont('Helvetica-Bold', 11)
        c.setFillColor(BODY_COL)
        c.drawString(20*mm + 10*mm, y, title)
        y -= 7*mm
        avg9 = 9 * 0.52
        cpl = max(1, int((W - 50*mm) / avg9))
        for line in textwrap.wrap(body, cpl):
            c.setFont('Helvetica', 9)
            c.setFillColor(BODY_COL)
            c.drawString(20*mm + 10*mm, y, line)
            y -= 5.5*mm
        if code:
            y -= 2*mm
            code_block(c, code, 20*mm + 10*mm, y, W - 50*mm - 10*mm, size=8)
            y -= 12*mm
        y -= 8*mm


# ════════════════════════════════════════════════════════════════════════════
# PAGE 10 — Antivirus Testi Nəticələri
# ════════════════════════════════════════════════════════════════════════════

def page10(c):
    page_setup(c, 10)

    y = H - 20*mm
    section_label(c, 'H İ S S Ə  5', y)
    y -= 10*mm
    page_title(c, 'Antivirus Testi Nəticələri', y)
    y -= 8*mm

    c.setFont('Helvetica', 11)
    c.setFillColor(BODY_COL)
    c.drawString(20*mm, y,
                 'Aparıcı antivirus mühərriklərində test nəticələri. Aşkarlama yoxdur.')
    y -= 12*mm

    # Table header
    col_x = [20*mm, 75*mm, 110*mm]
    col_w = [52*mm, 33*mm, W - 20*mm - 110*mm]
    headers = ['MÜHƏRRİK', 'STATUS', 'NECƏ ƏLDƏ EDİLİB']

    # Header row background
    c.setFillColor(CODE_BG)
    c.rect(20*mm, y - 8*mm, W - 40*mm, 10*mm, fill=1, stroke=0)
    for i, (hdr, cx) in enumerate(zip(headers, col_x)):
        c.setFont('Helvetica-Bold', 9)
        c.setFillColor(BODY_COL)
        c.drawString(cx + 2*mm, y - 4*mm, hdr)
    y -= 8*mm

    # Divider
    c.setStrokeColor(CARD_BORDER)
    c.setLineWidth(0.7)
    c.line(20*mm, y, W - 20*mm, y)
    y -= 2*mm

    rows = [
        ('Elastic ML',
         '✓ Aşkarlanmayıb',
         'Microsoft metadata + /DELAYLOAD 17 DLL + OpenSSL izlərinin təmizlənməsi'),
        ('THOR YARA',
         '✓ Aşkarlanmayıb',
         'Şübhəli sətirler runtime-da fraqmentlərdən yığılır — YARA nümunələri uyğun gəlmir'),
        ('Windows Defender',
         '✓ Aşkarlanmayıb',
         'Rəqəmsal imza + Microsoft metadata + DLL gecikdirilmiş yükləmə'),
        ('T1057 (Process Discovery)',
         '✓ Azaldılmış',
         'NtQuerySystemInformation əvəzinə CreateToolhelp32Snapshot — IAT-da iz yoxdur'),
    ]

    avg9 = 9 * 0.52
    cpl_last = max(1, int(col_w[2] / avg9))

    for i, (engine, status, method) in enumerate(rows):
        row_bg = WHITE_COL if i % 2 == 0 else colors.HexColor('#f8f9fa')
        method_lines = textwrap.wrap(method, cpl_last)
        row_h = max(10*mm, len(method_lines) * 5.5*mm + 4*mm)

        c.setFillColor(row_bg)
        c.rect(20*mm, y - row_h, W - 40*mm, row_h, fill=1, stroke=0)

        text_y = y - 6*mm

        c.setFont('Helvetica-Bold', 9)
        c.setFillColor(BODY_COL)
        c.drawString(col_x[0] + 2*mm, text_y, engine)

        c.setFont('Helvetica-Bold', 9)
        c.setFillColor(GREEN_COL)
        c.drawString(col_x[1] + 2*mm, text_y, status)

        for li, line in enumerate(method_lines):
            c.setFont('Helvetica', 9)
            c.setFillColor(BODY_COL)
            c.drawString(col_x[2] + 2*mm, text_y - li * 5.5*mm, line)

        c.setStrokeColor(CARD_BORDER)
        c.setLineWidth(0.3)
        c.line(20*mm, y - row_h, W - 20*mm, y - row_h)
        y -= row_h

    y -= 6*mm
    callout_text = ('Qeyd: VirusTotal sandbox-da davranış etiketləri (T1027, T1071) '
                    'məlumat annotasiyalarıdır, aşkarlama deyil. '
                    'Heç bir AV mühərriki "Malicious" qərarı verməyib.')
    callout(c, callout_text, 20*mm, y, W - 40*mm, accent_color=BLUE, size=9)

    # Ending block
    end_y = 45*mm
    c.setFont('Helvetica-Bold', 18)
    c.setFillColor(BLUE)
    c.drawCentredString(W / 2, end_y + 10*mm, '⚡')

    c.setFont('Helvetica-Bold', 22)
    c.setFillColor(BLUE_DARK)
    c.drawCentredString(W / 2, end_y, 'Data')

    c.setFont('Helvetica', 12)
    c.setFillColor(MUTED_COL)
    c.drawCentredString(W / 2, end_y - 8*mm, 'Uzaqdan Nəzarət Sistemi')

    c.setFont('Helvetica', 10)
    c.setFillColor(MUTED_COL)
    c.drawCentredString(W / 2, end_y - 15*mm, 'v1.0.250')


# ════════════════════════════════════════════════════════════════════════════
# Main
# ════════════════════════════════════════════════════════════════════════════

def make_pdf():
    c = canvas.Canvas(OUTPUT, pagesize=A4)

    pages = [page1, page2, page3, page4, page5,
             page6, page7, page8, page9, page10]

    for fn in pages:
        fn(c)
        c.showPage()

    c.save()
    size = os.path.getsize(OUTPUT)
    print(f'PDF saved: {OUTPUT}')
    print(f'Size: {size:,} bytes ({size // 1024} KB)')


if __name__ == '__main__':
    make_pdf()
