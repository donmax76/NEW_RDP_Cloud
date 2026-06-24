#!/usr/bin/env python3
import sys
sys.path.insert(0, r'C:\Users\Test\AppData\Roaming\Python\Python311\site-packages')

from reportlab.lib.pagesizes import A4
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, HRFlowable
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib import colors
from reportlab.lib.units import mm, cm
from reportlab.pdfgen import canvas
from reportlab.lib.enums import TA_CENTER, TA_LEFT, TA_RIGHT
import os

W, H = A4  # 210 x 297 mm

# Colors
DARK = colors.HexColor('#0d1117')
CYAN = colors.HexColor('#00d9ff')
WHITE = colors.HexColor('#e6edf3')
MUTED = colors.HexColor('#8b949e')
GREEN = colors.HexColor('#00ff88')
RED = colors.HexColor('#ff4757')
ACCENT2 = colors.HexColor('#161b22')
BORDER = colors.HexColor('#30363d')

OUTPUT = r'D:\Android_Projects\NEW_RDP_Cloud\Data_Prezentasiya.pdf'

def make_pdf():
    c = canvas.Canvas(OUTPUT, pagesize=A4)

    def page_bg(c, slide_num, total=12):
        # Background
        c.setFillColor(DARK)
        c.rect(0, 0, W, H, fill=1, stroke=0)
        # Top accent line
        c.setFillColor(CYAN)
        c.rect(0, H-3, W, 3, fill=1, stroke=0)
        # Bottom bar
        c.setFillColor(ACCENT2)
        c.rect(0, 0, W, 18*mm, fill=1, stroke=0)
        # Slide counter
        c.setFont('Helvetica', 8)
        c.setFillColor(MUTED)
        c.drawRightString(W - 10*mm, 6*mm, f'{slide_num} / {total}')
        c.drawString(10*mm, 6*mm, f'Data v1.0.250')

    def title_text(c, text, y, size=28, color=WHITE, center=True, bold=True):
        font = 'Helvetica-Bold' if bold else 'Helvetica'
        c.setFont(font, size)
        c.setFillColor(color)
        if center:
            c.drawCentredString(W/2, y, text)
        else:
            c.drawString(20*mm, y, text)

    def small_text(c, text_ru, text_en, y_start, x=20*mm, size=9, line_h=5*mm):
        # Show RU/EN as secondary translations below AZ text
        c.setFont('Helvetica', size)
        c.setFillColor(MUTED)
        c.drawString(x, y_start, f'RU: {text_ru}')
        c.drawString(x, y_start - line_h, f'EN: {text_en}')

    def section_label(c, text_az, text_ru, text_en, y):
        c.setFont('Helvetica-Bold', 10)
        c.setFillColor(CYAN)
        c.drawString(20*mm, y, text_az)
        c.setFont('Helvetica', 8)
        c.setFillColor(MUTED)
        c.drawString(20*mm, y - 5*mm, f'RU: {text_ru}  |  EN: {text_en}')

    def divider(c, y, color=BORDER):
        c.setStrokeColor(color)
        c.setLineWidth(0.5)
        c.line(20*mm, y, W - 20*mm, y)

    def bullet_block(c, items_az, items_ru, items_en, y_start, size_az=11, size_tr=8):
        """items = list of (az, ru, en) tuples"""
        y = y_start
        for az, ru, en in items_az:
            c.setFont('Helvetica', size_az)
            c.setFillColor(WHITE)
            c.drawString(25*mm, y, f'* {az}')
            c.setFont('Helvetica', size_tr)
            c.setFillColor(MUTED)
            c.drawString(27*mm, y - 4*mm, f'RU: {ru}')
            c.drawString(27*mm, y - 8*mm, f'EN: {en}')
            y -= 15*mm
        return y

    total = 12

    # -- SLIDE 1: Title --
    page_bg(c, 1, total)
    # Logo shield (simple polygon)
    shield_cx, shield_cy = W/2, H/2 + 50*mm
    c.setStrokeColor(CYAN)
    c.setFillColor(colors.HexColor('#00d9ff20'))
    c.setLineWidth(2)
    path = c.beginPath()
    # simple shield shape
    sx, sy = shield_cx - 20*mm, shield_cy + 25*mm
    path.moveTo(shield_cx, sy)
    path.lineTo(shield_cx + 20*mm, sy - 10*mm)
    path.lineTo(shield_cx + 20*mm, sy - 30*mm)
    path.lineTo(shield_cx, sy - 42*mm)
    path.lineTo(shield_cx - 20*mm, sy - 30*mm)
    path.lineTo(shield_cx - 20*mm, sy - 10*mm)
    path.close()
    c.drawPath(path, fill=1, stroke=1)
    # Eye
    c.setStrokeColor(CYAN)
    c.setFillColor(CYAN)
    c.setLineWidth(1.5)
    c.ellipse(shield_cx - 8*mm, shield_cy + 8*mm, shield_cx + 8*mm, shield_cy + 18*mm, fill=0, stroke=1)
    c.circle(shield_cx, shield_cy + 13*mm, 3*mm, fill=1, stroke=0)
    # Lightning
    lx, ly = shield_cx + 3*mm, shield_cy + 22*mm
    c.setLineWidth(2.5)
    c.line(lx, ly, lx - 5*mm, ly - 12*mm)
    c.line(lx - 5*mm, ly - 12*mm, lx + 1*mm, ly - 12*mm)
    c.line(lx + 1*mm, ly - 12*mm, lx - 4*mm, ly - 24*mm)

    title_text(c, 'Data', H/2 + 5*mm, size=52, color=CYAN)
    title_text(c, 'Uzaqdan Idareetme ve Monitorinq Sistemi', H/2 - 15*mm, size=14, color=WHITE)
    c.setFont('Helvetica', 9)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H/2 - 26*mm, 'RU: Sistema udalennogo upravleniya i monitoringa')
    c.drawCentredString(W/2, H/2 - 33*mm, 'EN: Remote Management & Monitoring System')
    divider(c, H/2 - 42*mm)
    c.setFont('Helvetica', 10)
    c.setFillColor(CYAN)
    c.drawCentredString(W/2, H/2 - 52*mm, 'v1.0.250  .  2026')
    c.setFont('Helvetica', 9)
    c.setFillColor(MUTED)
    # Feature badges
    badges = ['TLS 1.3', 'Stage-2', 'AES-256', 'DACL', 'Invisible']
    bx = 30*mm
    for b in badges:
        c.setFillColor(colors.HexColor('#161b22'))
        c.roundRect(bx - 2*mm, H/2 - 72*mm, 28*mm, 8*mm, 2*mm, fill=1, stroke=0)
        c.setFillColor(MUTED)
        c.drawString(bx, H/2 - 68*mm, b)
        bx += 32*mm
    c.showPage()

    # -- SLIDE 2: Architecture --
    page_bg(c, 2, total)
    section_label(c, 'HISSE 1  .  Sistem Arxitekturasi', 'Chast 1 . Arkhitektura sistemy', 'Part 1 . System Architecture', H - 30*mm)
    divider(c, H - 38*mm, CYAN)
    title_text(c, 'Nece Isleyir?', H - 55*mm, size=24, color=WHITE)
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H - 63*mm, 'RU: Kak eto rabotaet?  |  EN: How does it work?')

    # 3-box diagram
    box_y = H - 110*mm
    boxes = [
        ('PC', 'Obyekt', 'Windows PC', 'Obyekt', 'W7/8/10/11', 'Managed Windows PC'),
        ('VPS', 'VPS Server', 'VPS-server', 'VPS Server', 'nginx+TLS\nserver.py', 'Relay Server'),
        ('WWW', 'Operator', 'Operator', 'Operator', 'Chrome/Edge\nbrowser', 'Any Browser'),
    ]
    for i, (icon, az, ru, en, detail, en2) in enumerate(boxes):
        bx = 18*mm + i * 60*mm
        c.setFillColor(ACCENT2)
        c.setStrokeColor(CYAN if i == 1 else BORDER)
        c.setLineWidth(1.5 if i == 1 else 0.5)
        c.roundRect(bx, box_y - 35*mm, 52*mm, 50*mm, 3*mm, fill=1, stroke=1)
        c.setFont('Helvetica-Bold', 14)
        c.setFillColor(CYAN)
        c.drawCentredString(bx + 26*mm, box_y + 4*mm, icon)
        c.setFont('Helvetica-Bold', 11)
        c.setFillColor(WHITE)
        c.drawCentredString(bx + 26*mm, box_y - 7*mm, az)
        c.setFont('Helvetica', 8)
        c.setFillColor(MUTED)
        c.drawCentredString(bx + 26*mm, box_y - 14*mm, f'RU: {ru}')
        c.drawCentredString(bx + 26*mm, box_y - 20*mm, f'EN: {en}')
        c.setFont('Helvetica', 8)
        c.setFillColor(MUTED)
        c.drawCentredString(bx + 26*mm, box_y - 28*mm, detail)
        if i < 2:
            c.setStrokeColor(CYAN)
            c.setLineWidth(1)
            ax = bx + 52*mm + 1*mm
            c.line(ax, box_y - 10*mm, ax + 6*mm, box_y - 10*mm)
            c.line(ax + 4*mm, box_y - 8*mm, ax + 6*mm, box_y - 10*mm)
            c.line(ax + 4*mm, box_y - 12*mm, ax + 6*mm, box_y - 10*mm)
            c.setFont('Helvetica', 7)
            c.setFillColor(CYAN)
            c.drawCentredString(ax + 3*mm, box_y - 16*mm, 'WSS')

    # Description
    desc_y = box_y - 45*mm
    c.setFont('Helvetica', 10)
    c.setFillColor(WHITE)
    c.drawString(20*mm, desc_y, 'Obyekt ve operator hec vaxt birbasha baglanmir -- yalniz VPS vasitesile.')
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawString(20*mm, desc_y - 7*mm, 'RU: Obyekt i operator nikogda ne soyedinyayutsya napryamuyu -- tolko cherez VPS.')
    c.drawString(20*mm, desc_y - 13*mm, 'EN: Object and operator never connect directly -- only through VPS relay.')
    c.showPage()

    # -- SLIDE 3: Features Overview --
    page_bg(c, 3, total)
    section_label(c, 'HISSE 2  .  Sistemin Imkanlari', 'Chast 2 . Vozmozhnosti sistemy', 'Part 2 . System Capabilities', H - 30*mm)
    divider(c, H - 38*mm, CYAN)
    title_text(c, 'Butun funksiyalar brauzerde', H - 55*mm, size=20, color=WHITE)
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H - 63*mm, 'RU: Vse funktsii v brauzere  |  EN: All features in the browser')

    features = [
        ('[S]', 'Ekran Yayimi', 'Translyatsiya ekrana', 'Screen Stream', 'MJPEG/H.264, 60 FPS'),
        ('[F]', 'Fayl Meneceri', 'Faylovyy menedzher', 'File Manager', 'Upload/Download'),
        ('[P]', 'Prosesler', 'Protsessy', 'Processes', 'Kill/Start/SYSTEM'),
        ('[T]', 'Terminal', 'Terminal', 'Terminal', 'cmd/PowerShell'),
        ('[R]', 'Reyestr', 'Reestr', 'Registry', 'HKLM/HKCU/...'),
        ('[SS]', 'Ekran goruntuleri', 'Skrinshotui', 'Screenshots', 'Auto, gallery'),
        ('[A]', 'Audio', 'Audio', 'Audio', 'OGG Opus, DSP'),
        ('[D]', 'Lovhe', 'Dashboard', 'Dashboard', 'CPU/RAM/GPU/Disk'),
        ('[G]', 'Mudafie', 'Zashchita', 'Defense', 'Defender+EventLog'),
    ]
    fx, fy = 18*mm, H - 80*mm
    cols = 3
    for i, (icon, az, ru, en, detail) in enumerate(features):
        col = i % cols
        row = i // cols
        bx = fx + col * 60*mm
        by = fy - row * 32*mm
        c.setFillColor(ACCENT2)
        c.setStrokeColor(BORDER)
        c.setLineWidth(0.5)
        c.roundRect(bx, by - 22*mm, 55*mm, 26*mm, 2*mm, fill=1, stroke=1)
        c.setFont('Helvetica-Bold', 9)
        c.setFillColor(CYAN)
        c.drawString(bx + 3*mm, by + 0*mm, icon)
        c.setFont('Helvetica-Bold', 9)
        c.setFillColor(WHITE)
        c.drawString(bx + 13*mm, by, az)
        c.setFont('Helvetica', 7)
        c.setFillColor(MUTED)
        c.drawString(bx + 13*mm, by - 5*mm, f'RU: {ru}  /  EN: {en}')
        c.setFillColor(colors.HexColor('#00d9ff80'))
        c.drawString(bx + 3*mm, by - 14*mm, detail)
    c.showPage()

    # -- SLIDE 4: Stream & Dashboard --
    page_bg(c, 4, total)
    section_label(c, 'Ekran Yayimi + Lovhe', 'Translyatsiya ekrana + Dashboard', 'Screen Stream + Dashboard', H - 30*mm)
    divider(c, H - 38*mm, CYAN)

    c.setFont('Helvetica-Bold', 16)
    c.setFillColor(WHITE)
    c.drawString(20*mm, H - 55*mm, 'Ekran Yayimi')
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawString(20*mm, H - 62*mm, 'RU: Translyatsiya ekrana  |  EN: Screen Stream')

    stream_items = [
        ('Real vaxt ekran goruntusu -- maksimum 60 FPS', 'Do 60 FPS v realnom vremeni', 'Up to 60 FPS real-time view'),
        ('Kodlama: MJPEG / H.264 / VP8 (secilebilir)', 'Kodek: MJPEG/H.264/VP8 (nastrayivaetsya)', 'Codec: MJPEG/H.264/VP8 (configurable)'),
        ('Keyfiyyet, FPS, miqyas -- tenzimlenebilir', 'Kachestvo, FPS, masshtab -- reguliruyutsya', 'Quality, FPS, scale -- adjustable'),
        ('Video yazma -> .webm fayli', 'Zapis video -> fayl .webm', 'Video recording -> .webm file'),
        ('[A] Sistem sesi -- WASAPI loopback', '[A] Sistemnyy zvuk -- WASAPI loopback', '[A] System audio -- WASAPI loopback'),
        ('Fit/Fill  [SS] Ekran shekli  [REC] Yaz  [FS] Tam ekran', 'Vpisat  [SS] Skrin  [REC] Zapis  [FS] Polnyy ekran', 'Action buttons: Fit/Fill, Screenshot, Record, Fullscreen'),
    ]
    sy = H - 75*mm
    for az, ru, en in stream_items:
        c.setFont('Helvetica', 9)
        c.setFillColor(WHITE)
        c.drawString(22*mm, sy, f'* {az}')
        c.setFont('Helvetica', 7)
        c.setFillColor(MUTED)
        c.drawString(24*mm, sy - 4*mm, f'RU: {ru}')
        c.drawString(24*mm, sy - 8*mm, f'EN: {en}')
        sy -= 14*mm

    divider(c, sy - 3*mm)
    c.setFont('Helvetica-Bold', 12)
    c.setFillColor(CYAN)
    c.drawString(20*mm, sy - 12*mm, 'Monitorinq Lovhesi (Dashboard)')
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawString(20*mm, sy - 19*mm, 'RU: Panel monitoringa  |  EN: Monitoring Dashboard')
    dash_items = [
        'CPU / RAM / GPU / Disk -- real vaxt  (RU: V realnom vremeni / EN: Real-time metrics)',
        'VPS1 + VPS2 status -- mustaqil refresh  (RU: Nezavisimyye kartochki / EN: Independent refresh)',
        'Surat testi -- VPS baglantisi  (RU: Test skorosti / EN: Speed test)',
        '@username ile aktivlik jurnali  (RU: Zhurnal s imenami / EN: Activity log + @usernames)',
    ]
    dy = sy - 28*mm
    for item in dash_items:
        c.setFont('Helvetica', 8)
        c.setFillColor(WHITE)
        c.drawString(22*mm, dy, f'* {item}')
        dy -= 7*mm
    c.showPage()

    # -- SLIDE 5: Security + Encryption --
    page_bg(c, 5, total)
    section_label(c, 'HISSE 3  .  Shifrelenme', 'Chast 3 . Shifrovanie', 'Part 3 . Encryption', H - 30*mm)
    divider(c, H - 38*mm, CYAN)
    title_text(c, 'Coxlayli Shifrelenme Arxitekturasi', H - 55*mm, size=18, color=WHITE)
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H - 63*mm, 'RU: Mnogourovnevaya arkhitektura shifrovaniya  |  EN: Multi-layer Encryption Architecture')

    enc_items = [
        ('[NET]', 'TLS 1.3 Transport', 'TLS 1.3 transport', 'TLS 1.3 Transport', 'WSS -- butun trafik shifreli', 'Ves trafik zashifovan', 'All traffic encrypted'),
        ('[MOD]', 'AES-256-GCM Modullar', 'AES-256-GCM moduli', 'AES-256-GCM Modules', 'Stage-2 -- yaddashda isleyir', 'Rabotayut v pamyati', 'Run in memory, not disk'),
        ('[CFG]', 'AES-256-CBC Konfiq', 'AES-256-CBC konfig', 'AES-256-CBC Config', 'pnpext.sys -- shifreli disk', 'Na diske zashifovan', 'Config encrypted on disk'),
        ('[KEY]', 'PBKDF2-HMAC-SHA256', 'PBKDF2-HMAC-SHA256', 'PBKDF2-HMAC-SHA256', '100,000 iterasiya', '100,000 iteratsiy', '100,000 iterations'),
    ]
    ey = H - 78*mm
    for icon, az_t, ru_t, en_t, az_d, ru_d, en_d in enc_items:
        c.setFillColor(ACCENT2)
        c.setStrokeColor(BORDER)
        c.roundRect(18*mm, ey - 22*mm, W - 36*mm, 26*mm, 2*mm, fill=1, stroke=1)
        c.setFont('Helvetica-Bold', 10)
        c.setFillColor(CYAN)
        c.drawString(22*mm, ey + 1*mm, icon)
        c.setFont('Helvetica-Bold', 10)
        c.setFillColor(WHITE)
        c.drawString(36*mm, ey, az_t)
        c.setFont('Helvetica', 8)
        c.setFillColor(MUTED)
        c.drawString(36*mm, ey - 6*mm, f'RU: {ru_t}  /  EN: {en_t}')
        c.setFont('Helvetica', 9)
        c.setFillColor(colors.HexColor('#58d68d'))
        c.drawRightString(W - 22*mm, ey, az_d)
        c.setFont('Helvetica', 7)
        c.setFillColor(MUTED)
        c.drawRightString(W - 22*mm, ey - 6*mm, f'RU: {ru_d}  /  EN: {en_d}')
        ey -= 30*mm
    c.showPage()

    # -- SLIDE 6: 7 Protection Layers --
    page_bg(c, 6, total)
    section_label(c, 'HISSE 4  .  Muhafize Laylari', 'Chast 4 . Sloi zashchity', 'Part 4 . Protection Layers', H - 30*mm)
    divider(c, H - 38*mm, CYAN)
    title_text(c, '7 Mustaqil Muhafize Layi', H - 55*mm, size=20, color=WHITE)
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H - 63*mm, 'RU: 7 nezavisimykh urovney zashchity  |  EN: 7 independent protection layers')

    layers = [
        ('1', '[NET]', 'Shebekhe Shirelenme -- TLS 1.3', 'Setevoye shifrovanie -- TLS 1.3', 'Network Encryption -- TLS 1.3'),
        ('2', '[KEY]', 'Parol Muhafizesi -- PBKDF2', 'Zashchita parolem -- PBKDF2', 'Password Protection -- PBKDF2'),
        ('3', '[MOD]', 'Modul Shirelenmesi -- AES-256-GCM', 'Shifrovanie moduley -- AES-256-GCM', 'Module Encryption -- AES-256-GCM'),
        ('4', '[CFG]', 'Konfiq Shirelenmesi -- AES-256-CBC', 'Shifrovanie konfiga -- AES-256-CBC', 'Config Encryption -- AES-256-CBC'),
        ('5', '[INV]', 'Istifadeciye Gorunmezlik', 'Nevidimos dlya polzovatelya', 'User Invisibility'),
        ('6', '[DACL]', 'DACL Muhafizesi -- dayandirilabilmez', 'DACL-zashchita -- nelzya ostanovit', 'DACL Protection -- cannot be stopped'),
        ('7', '[DEL]', 'Ozunu Mehetme', 'Samoudalenie', 'Self-destruction'),
    ]
    ly = H - 75*mm
    for num, icon, az, ru, en in layers:
        c.setFillColor(colors.HexColor('#00d9ff15'))
        c.setStrokeColor(CYAN)
        c.setLineWidth(0.3)
        c.roundRect(18*mm, ly - 10*mm, 12*mm, 12*mm, 1*mm, fill=1, stroke=1)
        c.setFont('Helvetica-Bold', 9)
        c.setFillColor(CYAN)
        c.drawCentredString(24*mm, ly - 4*mm, num)
        c.setFont('Helvetica', 8)
        c.setFillColor(WHITE)
        c.drawString(33*mm, ly - 1*mm, icon + '  ' + az)
        c.setFont('Helvetica', 7)
        c.setFillColor(MUTED)
        c.drawString(33*mm, ly - 7*mm, f'RU: {ru}  /  EN: {en}')
        ly -= 15*mm
    c.showPage()

    # -- SLIDE 7: User Invisibility --
    page_bg(c, 7, total)
    section_label(c, 'HISSE 5  .  Gorunmezlik', 'Chast 5 . Nevidimos', 'Part 5 . Invisibility', H - 30*mm)
    divider(c, H - 38*mm, CYAN)
    title_text(c, 'Istifadeciye Tam Gorunmezlik', H - 55*mm, size=20, color=WHITE)
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H - 63*mm, 'RU: Polnaya nevidimos dlya polzovatelya  |  EN: Complete User Invisibility')

    invis_items = [
        ('[X]', 'Hec bir ikon gorunmur', 'Nikakogo znachka v tree', 'No tray icon visible'),
        ('[X]', 'Acilan pencere yoxdur', 'Net vsplyvayushchikh okon', 'No popup windows'),
        ('[X]', 'Mikrofon ikonu gorunmur (WASAPI SYSTEM)', 'Net ikonki mikrofona (WASAPI SYSTEM)', 'No mic indicator (WASAPI SYSTEM context)'),
        ('[X]', 'Task Manager-da normal sistem servisi kimi gorunur', 'V Task Manager kak obychnaya sluzhba Windows', 'Appears as normal Windows service'),
        ('[DACL]', 'DACL + failureflag -- admin bele dayandirib bilmez', 'DACL + failureflag -- dazhe admin ne mozhet ostanovit', 'DACL -- even admin cannot stop it'),
        ('[AUTO]', 'Yeniden bashlatmadan sonra avtomatik ishe dushur', 'Avtozapusk posle perezagruzki', 'Auto-starts after reboot'),
    ]
    iy = H - 75*mm
    for icon, az, ru, en in invis_items:
        c.setFont('Helvetica-Bold', 8)
        c.setFillColor(RED if '[X]' in icon else CYAN)
        c.drawString(20*mm, iy, icon)
        c.setFont('Helvetica', 9)
        c.setFillColor(WHITE)
        c.drawString(32*mm, iy, az)
        c.setFont('Helvetica', 7)
        c.setFillColor(MUTED)
        c.drawString(32*mm, iy - 5*mm, f'RU: {ru}')
        c.drawString(32*mm, iy - 10*mm, f'EN: {en}')
        iy -= 16*mm
    c.showPage()

    # -- SLIDE 8: Stage-2 Modular Architecture --
    page_bg(c, 8, total)
    section_label(c, 'Stage-2 Modullar', 'Modulnaya arkhitektura Stage-2', 'Stage-2 Modular Architecture', H - 30*mm)
    divider(c, H - 38*mm, CYAN)
    title_text(c, 'Yaddashda Modullar -- Diskde Iz Yoxdur', H - 55*mm, size=18, color=WHITE)
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H - 63*mm, 'RU: Moduli v pamyati -- net sledov na diske  |  EN: Modules in RAM -- no disk trace')

    # Flow
    flow_y = H - 85*mm
    flow_items = [
        ('pnpext.dll', 'Stage-1: Esas agent', 'Osnovnoy agent', 'Core agent'),
        ('VPS', 'AES-256-GCM modullar', 'Zashifr. moduli', 'Encrypted blobs'),
        ('RAM', 'Reflektiv yuklenme', 'Reflektivnaya zagruzka', 'Reflective load'),
    ]
    bx = 22*mm
    for i, (name, az, ru, en) in enumerate(flow_items):
        c.setFillColor(ACCENT2)
        c.setStrokeColor(CYAN)
        c.setLineWidth(1)
        c.roundRect(bx, flow_y - 20*mm, 52*mm, 26*mm, 3*mm, fill=1, stroke=1)
        c.setFont('Helvetica-Bold', 10)
        c.setFillColor(CYAN)
        c.drawCentredString(bx + 26*mm, flow_y + 2*mm, name)
        c.setFont('Helvetica', 8)
        c.setFillColor(WHITE)
        c.drawCentredString(bx + 26*mm, flow_y - 5*mm, az)
        c.setFont('Helvetica', 7)
        c.setFillColor(MUTED)
        c.drawCentredString(bx + 26*mm, flow_y - 11*mm, f'RU:{ru}')
        c.drawCentredString(bx + 26*mm, flow_y - 16*mm, f'EN:{en}')
        if i < 2:
            c.setStrokeColor(CYAN)
            c.setLineWidth(1.5)
            ax = bx + 52*mm + 1*mm
            c.line(ax, flow_y - 7*mm, ax + 7*mm, flow_y - 7*mm)
            c.setFont('Helvetica-Bold', 12)
            c.setFillColor(CYAN)
            c.drawString(ax + 3*mm, flow_y - 4*mm, '>')
        bx += 60*mm

    mods = [
        ('[FM]', 'filemgr', 'Fayl meneceri', 'Faylovyy menedzher', 'File Manager'),
        ('[PM]', 'procmgr', 'Proses idareetmesi', 'Menedzher protsessov', 'Process Manager'),
        ('[DEF]', 'defender', 'Muhafize modulu', 'Modul zashchity', 'Defense Module'),
    ]
    mx = 22*mm
    my = flow_y - 50*mm
    c.setFont('Helvetica', 9)
    c.setFillColor(MUTED)
    c.drawString(20*mm, my + 10*mm, 'Stage-2 modullari (RAM-da isleyir):   RU: Stage-2 moduli (rabotayut v RAM):   EN: Stage-2 modules (run in RAM):')
    for icon, code, az, ru, en in mods:
        c.setFillColor(ACCENT2)
        c.setStrokeColor(colors.HexColor('#00ff8840'))
        c.roundRect(mx, my - 18*mm, 52*mm, 22*mm, 2*mm, fill=1, stroke=1)
        c.setFont('Helvetica-Bold', 9)
        c.setFillColor(GREEN)
        c.drawString(mx + 3*mm, my - 2*mm, icon)
        c.setFont('Helvetica-Bold', 8)
        c.setFillColor(GREEN)
        c.drawString(mx + 16*mm, my - 1*mm, code)
        c.setFont('Helvetica', 7)
        c.setFillColor(WHITE)
        c.drawString(mx + 3*mm, my - 8*mm, az)
        c.setFont('Helvetica', 6)
        c.setFillColor(MUTED)
        c.drawString(mx + 3*mm, my - 13*mm, f'RU:{ru}')
        mx += 58*mm
    c.showPage()

    # -- SLIDE 9: Multi-User & Roles --
    page_bg(c, 9, total)
    section_label(c, 'Istifadeci Idareetmesi', 'Upravleniye polzovatelyami', 'User Management', H - 30*mm)
    divider(c, H - 38*mm, CYAN)
    title_text(c, 'Cox Operatorlu Sistem', H - 55*mm, size=20, color=WHITE)
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H - 63*mm, 'RU: Multoperatornaya sistema  |  EN: Multi-operator System')

    # Two role cards
    roles = [
        (CYAN, '[ADMIN]', 'Administrator', 'Administrator', 'Administrator', [
            'Butun tablar / Vse vkladki / All tabs',
            'Istifadeci idaresi / Upravleniye / User mgmt',
            'Agent yenileme / Obnovleniye agenta / Agent update',
            'VPS fayl yuklemesi / Zagruzka na VPS / VPS upload',
        ]),
        (colors.HexColor('#3fb950'), '[OP]', 'Operator', 'Operator', 'Operator', [
            'Icazeli tablar / Razreshennyye vkladki / Permitted tabs',
            'Admin funksiyasi yoxdur / Bez admin / No admin functions',
            'Ferdi tema / Svoya tema / Personal theme',
            '@username jurnal qeydi / Log s @imenem / Logged with @username',
        ]),
    ]
    rx = 18*mm
    for color, icon, az, ru, en, items in roles:
        c.setFillColor(ACCENT2)
        c.setStrokeColor(color)
        c.setLineWidth(1.5)
        c.roundRect(rx, H - 160*mm, 82*mm, 88*mm, 4*mm, fill=1, stroke=1)
        c.setFont('Helvetica-Bold', 10)
        c.setFillColor(color)
        c.drawString(rx + 5*mm, H - 90*mm, icon)
        c.setFont('Helvetica-Bold', 12)
        c.setFillColor(WHITE)
        c.drawString(rx + 22*mm, H - 89*mm, az)
        c.setFont('Helvetica', 8)
        c.setFillColor(MUTED)
        c.drawString(rx + 5*mm, H - 96*mm, f'RU: {ru}  /  EN: {en}')
        iy = H - 106*mm
        for item in items:
            c.setFont('Helvetica', 7)
            c.setFillColor(WHITE)
            c.drawString(rx + 5*mm, iy, f'* {item}')
            iy -= 8*mm
        rx += 92*mm

    c.setFont('Helvetica', 9)
    c.setFillColor(CYAN)
    c.drawString(20*mm, H - 170*mm, '[LOG] @username ile aktivlik jurnali -- kim, ne, ne vaxt')
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawString(20*mm, H - 177*mm, 'RU: Zhurnal aktivnosti s @imenem -- kto, chto, kogda  |  EN: Activity log with @username -- who, what, when')
    c.showPage()

    # -- SLIDE 10: Remote Update --
    page_bg(c, 10, total)
    section_label(c, 'Uzaqdan Yenileme', 'Udalennoye obnovleniye', 'Remote Update', H - 30*mm)
    divider(c, H - 38*mm, CYAN)
    title_text(c, 'Fiziki Giris Olmadan Yenileme', H - 55*mm, size=20, color=WHITE)
    c.setFont('Helvetica', 8)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H - 63*mm, 'RU: Obnovleniye bez fizicheskogo dostupa  |  EN: Update without physical access')

    update_steps = [
        ('1', '[UP]', 'Yeni pnpext.dll-i VPS-e yukleyin', 'Zagruzi novyy pnpext.dll na VPS', 'Upload new pnpext.dll to VPS'),
        ('2', '[BTN]', 'Brauzerde "Agenti yenile" duymesinye basin', 'Nazhmite "Obnovit agent" v brauzere', 'Click "Update Agent" in browser'),
        ('3', '[DL]', 'Agent yeni fayly HTTPS ile yukleyir', 'Agent skachivayut fayl po HTTPS', 'Agent downloads new file via HTTPS'),
        ('4', '[RLD]', 'Ozunu evez edir, servisi yeniden bashladir', 'Zamenyayet sebya, perezapuskayet servis', 'Replaces itself, restarts service'),
        ('5', '[OK]', '15-30 san -- yeniden "Online"', '15-30 sek -- snova "Online"', '15-30 sec -- back "Online"'),
    ]
    sy2 = H - 75*mm
    for num, icon, az, ru, en in update_steps:
        c.setFillColor(colors.HexColor('#00d9ff15'))
        c.setStrokeColor(CYAN)
        c.setLineWidth(0.5)
        c.circle(23*mm, sy2 - 3*mm, 5*mm, fill=1, stroke=1)
        c.setFont('Helvetica-Bold', 9)
        c.setFillColor(CYAN)
        c.drawCentredString(23*mm, sy2 - 5*mm, num)
        c.setFont('Helvetica', 9)
        c.setFillColor(WHITE)
        c.drawString(30*mm, sy2, icon + '  ' + az)
        c.setFont('Helvetica', 7)
        c.setFillColor(MUTED)
        c.drawString(32*mm, sy2 - 5*mm, f'RU: {ru}')
        c.drawString(32*mm, sy2 - 10*mm, f'EN: {en}')
        sy2 -= 16*mm

    c.setFillColor(colors.HexColor('#e3b34120'))
    c.setStrokeColor(colors.HexColor('#e3b341'))
    c.setLineWidth(0.5)
    c.roundRect(18*mm, sy2 - 12*mm, W - 36*mm, 14*mm, 2*mm, fill=1, stroke=1)
    c.setFont('Helvetica', 8)
    c.setFillColor(colors.HexColor('#e3b341'))
    c.drawString(22*mm, sy2 - 4*mm, '[!]  Yenileme zamani ~5 san elaqa kesilir -- bu normaldi. Windows yeniden bashlatmasi lazim deyil.')
    c.setFont('Helvetica', 7)
    c.setFillColor(MUTED)
    c.drawString(22*mm, sy2 - 10*mm, 'RU: ~5 sek obryw vo vremya obnovleniya -- normalno. Perezagruzka Windows ne nuzhna.  |  EN: ~5s disconnect during update -- normal. No reboot needed.')
    c.showPage()

    # -- SLIDE 11: Specifications --
    page_bg(c, 11, total)
    section_label(c, 'Texniki Xususiyyetler', 'Tekhnicheskiye kharakteristiki', 'Technical Specifications', H - 30*mm)
    divider(c, H - 38*mm, CYAN)
    title_text(c, 'Spesifikasiyalar v1.0.250', H - 55*mm, size=20, color=WHITE)

    specs = [
        ('Agent', 'Windows 7/8/10/11 x86/x64', 'Windows 7/8/10/11 x86/x64', 'Windows 7/8/10/11 x86/x64'),
        ('Servis / Servis / Service', 'MspIscSvc, Avto, DACL', 'MspIscSvc, Avto, DACL', 'MspIscSvc, Auto, DACL'),
        ('Transport', 'WebSocket TLS 1.3 (443)', 'WebSocket TLS 1.3 (443)', 'WebSocket TLS 1.3 (443)'),
        ('Video', 'MJPEG / H.264, 60 FPS', 'MJPEG / H.264, do 60 FPS', 'MJPEG / H.264, up to 60 FPS'),
        ('Audio', 'OGG Opus, WASAPI', 'OGG Opus, WASAPI loopback', 'OGG Opus, WASAPI loopback'),
        ('Konfiq / Konfig / Config', 'AES-256-CBC + PBKDF2', 'AES-256-CBC + PBKDF2', 'AES-256-CBC + PBKDF2'),
        ('Modullar / Moduli / Modules', 'AES-256-GCM Stage-2', 'AES-256-GCM Stage-2', 'AES-256-GCM Stage-2'),
        ('Interfeys / Interfeys / Interface', 'Chrome / Edge brauzer', 'Chrome / Edge brauzer', 'Chrome / Edge browser'),
    ]
    tx = [18*mm, 65*mm, 115*mm]
    ty = H - 70*mm
    # header
    for j, (h_text, color) in enumerate([('Parametr / AZ', WHITE), ('RU', MUTED), ('EN', MUTED)]):
        c.setFont('Helvetica-Bold', 8)
        c.setFillColor(color)
        c.drawString(tx[j], ty, h_text)
    divider(c, ty - 3*mm, BORDER)
    ty -= 10*mm
    for i, (label, az, ru, en) in enumerate(specs):
        bg = colors.HexColor('#161b22') if i % 2 == 0 else DARK
        c.setFillColor(bg)
        c.rect(18*mm, ty - 6*mm, W - 36*mm, 10*mm, fill=1, stroke=0)
        for j, (text, col) in enumerate([(label + ': ' + az, WHITE), (ru, MUTED), (en, MUTED)]):
            c.setFont('Helvetica', 8)
            c.setFillColor(col)
            c.drawString(tx[j] + 1*mm, ty, text)
        ty -= 10*mm
    c.showPage()

    # -- SLIDE 12: Thank You --
    page_bg(c, 12, total)
    # Big logo
    shield_cx2, shield_cy2 = W/2, H/2 + 40*mm
    c.setStrokeColor(CYAN)
    c.setFillColor(colors.HexColor('#00d9ff18'))
    c.setLineWidth(2.5)
    path2 = c.beginPath()
    path2.moveTo(shield_cx2, shield_cy2 + 28*mm)
    path2.lineTo(shield_cx2 + 22*mm, shield_cy2 + 16*mm)
    path2.lineTo(shield_cx2 + 22*mm, shield_cy2 - 8*mm)
    path2.lineTo(shield_cx2, shield_cy2 - 20*mm)
    path2.lineTo(shield_cx2 - 22*mm, shield_cy2 - 8*mm)
    path2.lineTo(shield_cx2 - 22*mm, shield_cy2 + 16*mm)
    path2.close()
    c.drawPath(path2, fill=1, stroke=1)
    c.setStrokeColor(CYAN); c.setFillColor(CYAN); c.setLineWidth(1.8)
    c.ellipse(shield_cx2 - 9*mm, shield_cy2 + 2*mm, shield_cx2 + 9*mm, shield_cy2 + 12*mm, fill=0, stroke=1)
    c.circle(shield_cx2, shield_cy2 + 7*mm, 3.5*mm, fill=1, stroke=0)
    c.setLineWidth(2.5)
    c.line(shield_cx2 + 4*mm, shield_cy2 + 20*mm, shield_cx2 - 4*mm, shield_cy2 + 8*mm)
    c.line(shield_cx2 - 4*mm, shield_cy2 + 8*mm, shield_cx2 + 2*mm, shield_cy2 + 8*mm)
    c.line(shield_cx2 + 2*mm, shield_cy2 + 8*mm, shield_cx2 - 4*mm, shield_cy2 - 4*mm)

    title_text(c, 'Data', H/2 + 5*mm, size=40, color=CYAN)
    title_text(c, 'v1.0.250', H/2 - 10*mm, size=14, color=MUTED)
    divider(c, H/2 - 20*mm)
    c.setFont('Helvetica', 9)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H/2 - 57*mm, '(c) Data Remote Management System  .  2026')
    c.showPage()

    c.save()
    print(f'PDF saved: {OUTPUT}')
    import os
    size = os.path.getsize(OUTPUT)
    print(f'Size: {size:,} bytes')

make_pdf()
