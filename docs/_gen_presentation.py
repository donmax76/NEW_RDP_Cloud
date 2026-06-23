import sys
sys.path.insert(0, r'C:\Users\Test\AppData\Roaming\Python\Python311\site-packages')

from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN
from pptx.util import Inches, Pt
import os

OUT = r'D:\Android_Projects\NEW_RDP_Cloud\docs\Prometey_Presentation.pptx'

# Colors
C_BG      = RGBColor(0x0d, 0x11, 0x17)
C_PANEL   = RGBColor(0x16, 0x1b, 0x22)
C_CARD    = RGBColor(0x21, 0x26, 0x2d)
C_CYAN    = RGBColor(0x00, 0xd9, 0xff)
C_WHITE   = RGBColor(0xe6, 0xed, 0xf3)
C_MUTED   = RGBColor(0x8b, 0x94, 0x9e)
C_GREEN   = RGBColor(0x00, 0xff, 0x88)
C_DANGER  = RGBColor(0xff, 0x47, 0x57)

prs = Presentation()
prs.slide_width  = Inches(13.33)
prs.slide_height = Inches(7.5)

blank_layout = prs.slide_layouts[6]  # blank

def add_slide():
    return prs.slides.add_slide(blank_layout)

def bg(slide, color=C_BG):
    from pptx.util import Inches
    sp = slide.shapes.add_shape(1, 0, 0, prs.slide_width, prs.slide_height)
    sp.fill.solid(); sp.fill.fore_color.rgb = color
    sp.line.fill.background()

def txb(slide, text, x, y, w, h, size=18, bold=False, color=C_WHITE, align=PP_ALIGN.LEFT, italic=False, wrap=True):
    tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = tb.text_frame; tf.word_wrap = wrap
    p = tf.paragraphs[0]; p.alignment = align
    run = p.add_run(); run.text = text
    run.font.size = Pt(size); run.font.bold = bold
    run.font.italic = italic; run.font.color.rgb = color
    return tb

def txb_ml(slide, lines, x, y, w, h, default_size=14, default_color=C_WHITE):
    """Multi-line textbox: lines = list of (text, size, bold, color, align)"""
    tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = tb.text_frame; tf.word_wrap = True
    first = True
    for (text, size, bold, color, align) in lines:
        if first:
            p = tf.paragraphs[0]; first = False
        else:
            p = tf.add_paragraph()
        p.alignment = align
        run = p.add_run(); run.text = text
        run.font.size = Pt(size); run.font.bold = bold
        run.font.color.rgb = color
    return tb

def rect(slide, x, y, w, h, fill=C_PANEL, line_color=None, line_w=Pt(0)):
    sp = slide.shapes.add_shape(1, Inches(x), Inches(y), Inches(w), Inches(h))
    sp.fill.solid(); sp.fill.fore_color.rgb = fill
    if line_color:
        sp.line.color.rgb = line_color; sp.line.width = line_w
    else:
        sp.line.fill.background()
    return sp

def footer(slide, slide_num, total=14):
    # Bottom bar
    rect(slide, 0, 7.1, 13.33, 0.4, C_PANEL)
    txb(slide, f'Data v1.0.250', 0.3, 7.15, 5, 0.3, size=9, color=C_MUTED)
    txb(slide, f'{slide_num} / {total}', 12, 7.15, 1.2, 0.3, size=9, color=C_MUTED, align=PP_ALIGN.RIGHT)

def top_bar(slide, color=C_CYAN):
    sp = slide.shapes.add_shape(1, 0, 0, prs.slide_width, Inches(0.06))
    sp.fill.solid(); sp.fill.fore_color.rgb = color
    sp.line.fill.background()

def section_tag(slide, text, x=0.4, y=0.15):
    txb(slide, text, x, y, 12, 0.4, size=10, color=C_CYAN, bold=True)

def bullet_block(slide, items, x, y, w, h, size_main=13, size_sub=10):
    """items = list of (main_text, sub_text_or_None)"""
    tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = tb.text_frame; tf.word_wrap = True
    first = True
    for (main, sub) in items:
        if first:
            p = tf.paragraphs[0]; first = False
        else:
            p = tf.add_paragraph()
        p.alignment = PP_ALIGN.LEFT
        run = p.add_run()
        run.text = main
        run.font.size = Pt(size_main); run.font.bold = True; run.font.color.rgb = C_WHITE
        if sub:
            p2 = tf.add_paragraph(); p2.alignment = PP_ALIGN.LEFT
            from pptx.util import Pt as PT2
            r2 = p2.add_run(); r2.text = '    ' + sub
            r2.font.size = Pt(size_sub); r2.font.color.rgb = C_MUTED

# --- SLIDE 1: TITLE ---
sl = add_slide()
bg(sl)
top_bar(sl)
footer(sl, 1)

# Shield shape (pentagon approximation with rounded rect)
shield = sl.shapes.add_shape(1, Inches(5.4), Inches(0.8), Inches(2.5), Inches(2.8))
shield.fill.solid(); shield.fill.fore_color.rgb = RGBColor(0x00, 0x25, 0x35)
shield.line.color.rgb = C_CYAN; shield.line.width = Pt(2)

txb(sl, 'Data', 0.5, 3.7, 12.33, 1.2, size=64, bold=True, color=C_CYAN, align=PP_ALIGN.CENTER)
txb(sl, 'v1.0.250  ·  2026', 0.5, 4.75, 12.33, 0.5, size=14, color=C_MUTED, align=PP_ALIGN.CENTER)

txb_ml(sl, [
    ('Система удалённого управления и мониторинга  (RU)', 13, False, C_WHITE, PP_ALIGN.CENTER),
    ('Remote Management & Monitoring System  (EN)', 13, False, C_MUTED, PP_ALIGN.CENTER),
    ('Uzaqdan İdarəetmə və Monitorinq Sistemi  (AZ)', 13, False, C_MUTED, PP_ALIGN.CENTER),
], 0.5, 5.3, 12.33, 1.4)

# Feature tags row
tags = ['TLS 1.3', 'AES-256', 'Stage-2', 'DACL', 'WASAPI']
tx = 1.8
for tag in tags:
    sp = sl.shapes.add_shape(1, Inches(tx), Inches(6.7), Inches(1.7), Inches(0.35))
    sp.fill.solid(); sp.fill.fore_color.rgb = RGBColor(0x00, 0x25, 0x35)
    sp.line.color.rgb = C_CYAN; sp.line.width = Pt(0.5)
    txb(sl, tag, tx + 0.05, 6.72, 1.6, 0.3, size=10, color=C_CYAN, align=PP_ALIGN.CENTER)
    tx += 2.0

# --- SLIDE 2: ARCHITECTURE ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 2)
section_tag(sl, 'HİSSƌ 1  ·  Sistem Arxitekturaı / Архитектура / Architecture')
txb(sl, 'Necə İşləyir?  /  Как это работает?  /  How it Works', 0.4, 0.55, 12.5, 0.5, size=22, bold=True, color=C_WHITE)

# 3 boxes
boxes = [
    ('Obyekt / Объект\nObject', 'Windows PC\npnpext.dll\nMspIscSvc service', C_CYAN),
    ('VPS Server\nVPS-сервер', 'server.py relay\nnginx + TLS 1.3\nPort 443', C_CYAN),
    ('Operator / Оператор', 'Chrome / Edge\nNo install needed\nAny device', C_CYAN),
]
bx = 0.4
for (title, detail, accent) in boxes:
    sp = sl.shapes.add_shape(1, Inches(bx), Inches(1.3), Inches(3.6), Inches(3.8))
    sp.fill.solid(); sp.fill.fore_color.rgb = C_PANEL
    sp.line.color.rgb = accent; sp.line.width = Pt(1.5)
    txb(sl, title, bx+0.15, 1.45, 3.3, 0.7, size=14, bold=True, color=C_CYAN, align=PP_ALIGN.CENTER)
    txb(sl, detail, bx+0.15, 2.2, 3.3, 2.5, size=12, color=C_WHITE, align=PP_ALIGN.CENTER)
    if bx < 9:
        arrow = sl.shapes.add_shape(9, Inches(bx+3.6), Inches(2.8), Inches(0.9), Inches(0))
        arrow.line.color.rgb = C_CYAN; arrow.line.width = Pt(2)
        txb(sl, 'WSS', bx+3.65, 2.55, 0.8, 0.3, size=9, color=C_CYAN, align=PP_ALIGN.CENTER)
    bx += 4.5

txb(sl, 'Obyekt ve operator hecer birbacha baglenmır — yalnız VPS vasitesiyle. (AZ)', 0.4, 5.3, 12.5, 0.35, size=10, color=C_MUTED)
txb(sl, 'RU: Объект и оператор никогда не соединяются напрямую — только через VPS.  |  EN: Never direct — only through VPS relay.', 0.4, 5.65, 12.5, 0.35, size=10, color=C_MUTED)

# --- SLIDE 3: CAPABILITIES ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 3)
section_tag(sl, 'HİSSƌ 2  ·  İmkanlar / Возможности / Capabilities')
txb(sl, 'Bütün funksiyalar brauzerdə  /  Все функции в браузере  /  All features in browser', 0.4, 0.55, 12.5, 0.5, size=20, bold=True, color=C_WHITE)

features = [
    ('Ekran Yayımı', 'Tрансляция экрана / Screen Stream', 'MJPEG/H.264, 60 FPS'),
    ('Fayl Meneceri', 'Файловый менеджер / File Manager', 'Upload / Download'),
    ('Prosesler', 'Процессы / Processes', 'Kill / Start / SYSTEM'),
    ('Terminal', 'Терминал / Terminal', 'cmd + PowerShell'),
    ('Reyestr', 'Реестр / Registry', 'HKLM/HKCU/HKCR/...'),
    ('Ekran Goruntusu', 'Скриншоты / Screenshots', 'Auto, gallery, preview'),
    ('Audio', 'Аудио / Audio', 'OGG Opus, DSP chain'),
    ('Lövhə', 'Дашборд / Dashboard', 'CPU/RAM/GPU/Disk'),
    ('Mudafiə', 'Защита / Defense', 'Defender + EventLog'),
]
cols = 3
for i, (az, ruen, detail) in enumerate(features):
    col = i % cols
    row = i // cols
    fx = 0.4 + col * 4.3
    fy = 1.2 + row * 2.0
    sp = sl.shapes.add_shape(1, Inches(fx), Inches(fy), Inches(4.0), Inches(1.75))
    sp.fill.solid(); sp.fill.fore_color.rgb = C_PANEL
    sp.line.color.rgb = RGBColor(0x30, 0x36, 0x3d); sp.line.width = Pt(0.5)
    txb(sl, az, fx+0.12, fy+0.1, 3.7, 0.45, size=13, bold=True, color=C_CYAN)
    txb(sl, ruen, fx+0.12, fy+0.55, 3.7, 0.4, size=10, color=C_MUTED)
    txb(sl, detail, fx+0.12, fy+0.95, 3.7, 0.4, size=11, color=C_WHITE)

# --- SLIDE 4: DASHBOARD ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 4)
section_tag(sl, 'Monitorinq Lövhəsi / Панель мониторинга / Dashboard')
txb(sl, 'Real vaxt monitoring  /  Мониторинг в реальном времени  /  Real-time monitoring', 0.4, 0.55, 12.5, 0.5, size=20, bold=True, color=C_WHITE)

dash_items = [
    ('CPU / RAM / GPU / Disk', 'Real vaxt sistemin metrikalari\nRU: Метрики системы в реальном времени\nEN: Real-time system metrics'),
    ('VPS1 + VPS2 Status', 'Mustaqil ↻ yenileme duymeleri\nRU: Независимые кнопки обновления\nEN: Independent ↻ refresh buttons'),
    ('Suret Testi', 'VPS baglantisinin sureti\nRU: Скорость соединения с VPS\nEN: VPS connection speed test'),
    ('@username Aktivlik Jurnali', 'Kim, ne, ne vaxt\nRU: Кто, что, когда — @имя\nEN: Who, what, when — @username'),
]
for i, (title, desc) in enumerate(dash_items):
    col = i % 2; row = i // 2
    dx = 0.4 + col * 6.5
    dy = 1.25 + row * 2.7
    sp = sl.shapes.add_shape(1, Inches(dx), Inches(dy), Inches(6.1), Inches(2.4))
    sp.fill.solid(); sp.fill.fore_color.rgb = C_PANEL
    sp.line.color.rgb = C_CYAN; sp.line.width = Pt(1)
    txb(sl, title, dx+0.2, dy+0.1, 5.7, 0.5, size=14, bold=True, color=C_CYAN)
    txb(sl, desc, dx+0.2, dy+0.65, 5.7, 1.5, size=11, color=C_WHITE)

# --- SLIDE 5: SCREEN STREAM ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 5)
section_tag(sl, 'Ekran Yayımı / Трансляция экрана / Screen Stream')
txb(sl, 'Live stream + action buttons', 0.4, 0.55, 12.5, 0.5, size=20, bold=True, color=C_WHITE)

# Left: stream params
txb(sl, 'Stream Parametrləri / Параметры:', 0.4, 1.2, 5.5, 0.4, size=13, bold=True, color=C_CYAN)
params = [
    ('FPS 1-60', 'Kadr sureti / Частота кадров / Frame rate'),
    ('Keyfiyyət 1-100', 'JPEG sıxışdırma / JPEG quality'),
    ('Miqyas %', 'Obyektdə kiciltme / Downscale on target'),
    ('Kodek', 'MJPEG / H.264 (secilir / выбирается)'),
]
py = 1.7
for name, desc in params:
    txb(sl, name, 0.5, py, 2.5, 0.35, size=12, bold=True, color=C_WHITE)
    txb(sl, desc, 3.0, py, 3.2, 0.35, size=10, color=C_MUTED)
    py += 0.5

# Right: action buttons
txb(sl, 'Emeliyyat Duymeleri (aşağı-sağda üzen panel):', 6.5, 1.2, 6.5, 0.4, size=13, bold=True, color=C_CYAN)
txb(sl, 'RU: Кнопки управления (плавающая панель)  /  EN: Action buttons pill (bottom-right)', 6.5, 1.65, 6.5, 0.4, size=9, color=C_MUTED)

btns = [
    ('[FIT]', 'Fit/Fill — tam ekran / вписать/заполнить / fit or fill'),
    ('[CAM]', 'Screenshot — PNG → Yuklemeler / Загрузки / Downloads'),
    ('[SND]', 'Sistem sesi — WASAPI loopback / Системный звук'),
    ('[REC]', 'Video yazma → .webm / Запись видео / Record video'),
    ('[FS]',  'Tam ekran rejimi / Полный экран / Fullscreen'),
]
by2 = 2.15
for icon, desc in btns:
    sp = sl.shapes.add_shape(1, Inches(6.5), Inches(by2), Inches(0.7), Inches(0.45))
    sp.fill.solid(); sp.fill.fore_color.rgb = RGBColor(0x00, 0x25, 0x35)
    sp.line.color.rgb = C_CYAN; sp.line.width = Pt(0.75)
    txb(sl, icon, 6.55, by2+0.07, 0.6, 0.3, size=10, bold=True, color=C_CYAN, align=PP_ALIGN.CENTER)
    txb(sl, desc, 7.3, by2+0.07, 6.0, 0.4, size=10, color=C_WHITE)
    by2 += 0.57

# Warning
rect(sl, 0.4, 6.2, 12.5, 0.6, fill=RGBColor(0x1a, 0x14, 0x00))
txb(sl, 'IMPORTANT: [SND] sistem sesini [REC]-DEN EVVEL aktivlesdirin ki video sesle yazilsin.', 0.5, 6.27, 12.2, 0.4, size=11, bold=True, color=RGBColor(0xe3, 0xb3, 0x41))

# --- SLIDE 6: FILE MANAGER ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 6)
section_tag(sl, 'Fayl Meneceri / Файловый менеджер / File Manager')
txb(sl, 'Tam fayl sistemi girisi  /  Полный доступ к файловой системе  /  Full filesystem access', 0.4, 0.55, 12.5, 0.5, size=20, bold=True, color=C_WHITE)

fm_items = [
    ('Butun suruculer, gizli ve sistem fayllar', 'RU: Все диски, скрытые и системные файлы\nEN: All drives, hidden/system files visible'),
    ('Yukle: fayl adina klik → segment kocurme', 'RU: Скачать: кликнуть по имени файла\nEN: Download: click filename (segments + progress)'),
    ('Gondər: surukle-burax ve ya Yukle duymesi', 'RU: Загрузить: перетащить или кнопка\nEN: Upload: drag & drop or Upload button'),
    ('Naviqasiya: qovluglara klik, ↑ duymesi, yol', 'RU: Навигация: папки, кнопка ↑, хлебные крошки\nEN: Navigate: click folders, ↑ button, breadcrumbs'),
]
for i, (az, ruen) in enumerate(fm_items):
    fy = 1.3 + i * 1.4
    sp = sl.shapes.add_shape(1, Inches(0.4), Inches(fy), Inches(12.5), Inches(1.2))
    sp.fill.solid(); sp.fill.fore_color.rgb = C_PANEL
    sp.line.color.rgb = RGBColor(0x30, 0x36, 0x3d); sp.line.width = Pt(0.3)
    txb(sl, az, 0.6, fy+0.1, 12.1, 0.45, size=13, bold=True, color=C_WHITE)
    txb(sl, ruen, 0.6, fy+0.6, 12.1, 0.5, size=10, color=C_MUTED)

# --- SLIDE 7: PROCESSES ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 7)
section_tag(sl, 'Prosesler ve Servisler / Процессы и Сервисы / Processes & Services')
txb(sl, 'SYSTEM huquqlari ile tam idareetme  /  Полный контроль с правами SYSTEM', 0.4, 0.55, 12.5, 0.5, size=19, bold=True, color=C_WHITE)

proc_items = [
    ('Proses siyahisi: PID, Ad, CPU%, RAM', 'RU: Список процессов: PID, имя, CPU%, RAM\nEN: Process list: PID, name, CPU%, RAM — all running'),
    ('Secin → Bitir duymesi (tesdiq yoxdur)', 'RU: Выбрать → кнопка Завершить (без подтверждения)\nEN: Select → Kill button (no confirmation prompt)'),
    ('"Baslat" sahesi + Enter → SYSTEM-den proqram', 'RU: Поле "Запустить" + Enter → от SYSTEM\nEN: Run field + Enter → launches as SYSTEM'),
    ('Servisler tabi: Başlat/Dayandır/Yeniden Başlat', 'RU: Сервисы: Запуск/Стоп, тип запуска Авто/Ручной\nEN: Services: Start/Stop/Restart, change startup type'),
]
for i, (az, ruen) in enumerate(proc_items):
    py2 = 1.25 + i * 1.5
    sp = sl.shapes.add_shape(1, Inches(0.4), Inches(py2), Inches(12.5), Inches(1.3))
    sp.fill.solid(); sp.fill.fore_color.rgb = C_PANEL
    sp.line.color.rgb = RGBColor(0x30, 0x36, 0x3d); sp.line.width = Pt(0.3)
    txb(sl, az, 0.6, py2+0.1, 12.1, 0.45, size=13, bold=True, color=C_WHITE)
    txb(sl, ruen, 0.6, py2+0.6, 12.1, 0.5, size=10, color=C_MUTED)

# --- SLIDE 8: TERMINAL + REGISTRY ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 8)
section_tag(sl, 'Terminal + Reyestr / Терминал + Реестр / Terminal + Registry')
txb(sl, 'SYSTEM seviyyesinde tam erar / Полный контроль на уровне SYSTEM / Full SYSTEM-level control', 0.4, 0.55, 12.5, 0.5, size=18, bold=True, color=C_WHITE)

# Terminal
rect(sl, 0.4, 1.2, 6.0, 5.6, C_PANEL)
txb(sl, 'TERMINAL', 0.6, 1.35, 5.6, 0.4, size=14, bold=True, color=C_CYAN)
txb(sl, 'cmd + PowerShell, SYSTEM (maks imtiyazlar)\nRU: Максимальные привилегии от SYSTEM\nEN: Maximum privileges as SYSTEM', 0.6, 1.85, 5.6, 0.8, size=11, color=C_WHITE)
# Code example area
rect(sl, 0.55, 2.75, 5.7, 1.5, RGBColor(0x0a, 0x0c, 0x10))
txb(sl, 'dir C:\\Users\ntasklist | findstr chrome\nGet-Process | Sort CPU -Desc | Select -First 5', 0.65, 2.85, 5.5, 1.3, size=10, color=RGBColor(0xd2, 0xa8, 0xff))
txb(sl, 'XEBERDARLIQ: pause, choice kimi emirler donub qala biler.\nRU: Интерактивные команды могут зависнуть.\nEN: Interactive commands may hang — use -Force flags.', 0.6, 4.35, 5.6, 1.3, size=10, color=RGBColor(0xe3, 0xb3, 0x41))

# Registry
rect(sl, 6.8, 1.2, 6.1, 5.6, C_PANEL)
txb(sl, 'REYESTR', 7.0, 1.35, 5.7, 0.4, size=14, bold=True, color=C_CYAN)
hives = 'HKLM  HKCU  HKCR  HKU  HKCC'
txb(sl, hives, 7.0, 1.85, 5.7, 0.4, size=12, bold=True, color=C_WHITE)
txb(sl, 'RU: Все ключи реестра Windows  /  EN: All Windows registry hives', 7.0, 2.3, 5.7, 0.4, size=10, color=C_MUTED)
reg_ops = [
    ('Oxu', 'Acacara klik — dəyərləri sag panelə goster\nRU: Клик по ключу — значения справа\nEN: Click key to view values on right'),
    ('Deyis', 'Dəyərə sag klik → Deyis\nRU: ПКМ по значению → Изменить\nEN: Right-click value → Edit'),
    ('Sil', 'Sag klik → Sil (geri alinamaz!)\nRU: ПКМ → Удалить (необратимо!)\nEN: Right-click → Delete (irreversible!)'),
]
ry = 2.85
for op, desc in reg_ops:
    txb(sl, op, 7.0, ry, 1.2, 0.4, size=12, bold=True, color=C_CYAN)
    txb(sl, desc, 8.3, ry, 4.4, 0.7, size=10, color=C_WHITE)
    ry += 1.0

# --- SLIDE 9: SCREENSHOTS + AUDIO ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 9)
section_tag(sl, 'Ekran Goruntusu + Audio / Скриншоты + Аудио / Screenshots + Audio')
txb(sl, 'Avtomatik izleme  /  Автоматический мониторинг  /  Automatic monitoring', 0.4, 0.55, 12.5, 0.5, size=20, bold=True, color=C_WHITE)

# Screenshots
rect(sl, 0.4, 1.2, 6.0, 5.6, C_PANEL)
txb(sl, 'EKRAN GORUNTUSU', 0.6, 1.35, 5.6, 0.4, size=14, bold=True, color=C_CYAN)
txb(sl, 'RU: Скриншоты  /  EN: Screenshots', 0.6, 1.8, 5.6, 0.3, size=10, color=C_MUTED)
ss_items = [
    ('Avto her 10 san (qurasdirila biler)', 'RU: Авто каждые 10 сек  /  EN: Auto every 10 sec'),
    ('VPS-de saxlanilir, qalereyada baxilir', 'RU: На VPS, просмотр в браузере  /  EN: Stored on VPS, gallery view'),
    ('Klikle → on izleme → tam ekran', 'RU: Клик → предпросмотр → полный экран  /  EN: Click → preview → fullscreen'),
    ('Sildikde aciq goruntu avto temizlenir', 'RU: При удалении предпросмотр очищается  /  EN: Delete auto-clears open preview'),
]
sy2 = 2.2
for az, ruen in ss_items:
    txb(sl, az, 0.6, sy2, 5.6, 0.4, size=11, bold=True, color=C_WHITE)
    txb(sl, ruen, 0.6, sy2+0.42, 5.6, 0.35, size=9, color=C_MUTED)
    sy2 += 1.0

# Audio
rect(sl, 6.8, 1.2, 6.1, 5.6, C_PANEL)
txb(sl, 'AUDIO YAZMA', 7.0, 1.35, 5.7, 0.4, size=14, bold=True, color=C_CYAN)
txb(sl, 'RU: Аудио запись  /  EN: Audio Recording', 7.0, 1.8, 5.7, 0.3, size=10, color=C_MUTED)
au_items = [
    ('Fasilesiz mikrofon yazma', 'RU: Непрерывная запись микрофона  /  EN: Continuous mic recording'),
    ('OGG Opus, 5 deq seqmentler', 'RU: 5-мин сегменты  /  EN: OGG Opus, 5-min segments'),
    ('Daxili audio pleyer — sildikde temizlenir', 'RU: Встроенный плеер — при удалении очищается  /  EN: Built-in player — delete clears it'),
    ('WASAPI SYSTEM — mikrofon ikonu gorünmür', 'RU: Нет индикатора микрофона  /  EN: No mic privacy indicator shown'),
]
ay2 = 2.2
for az, ruen in au_items:
    txb(sl, az, 7.0, ay2, 5.7, 0.4, size=11, bold=True, color=C_WHITE)
    txb(sl, ruen, 7.0, ay2+0.42, 5.7, 0.35, size=9, color=C_MUTED)
    ay2 += 1.0

# --- SLIDE 10: SECURITY ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 10)
section_tag(sl, 'Tehlukesizlik / Безопасность / Security')
txb(sl, '7 mustaqil muhafize layi  /  7 независимых уровней защиты  /  7 independent protection layers', 0.4, 0.55, 12.5, 0.5, size=18, bold=True, color=C_WHITE)

sec_items = [
    ('TLS 1.3', 'Butun trafik sifreli  /  Весь трафик зашифрован  /  All traffic encrypted'),
    ('AES-256-CBC', 'Konfiq diskde sifreli (pnpext.sys)  /  Конфиг зашифрован на диске  /  Config encrypted on disk'),
    ('AES-256-GCM', 'Stage-2 modullar yalniz RAM-da  /  Модули только в RAM  /  Stage-2 modules RAM-only'),
    ('PBKDF2-HMAC-SHA256', '100,000 iterasiya  /  100,000 итераций  /  100,000 iterations'),
    ('DACL muhafizesi', 'Servis dayndırıla bilmez  /  Службу нельзя остановить  /  Service cannot be stopped'),
    ('Rol esasli giris', 'Admin / Operator ayrilmasi  /  Разграничение прав  /  Role-based access control'),
    ('@username audit jurnali', 'Butun emeliyyatlar qeyd edilir  /  Все действия записаны  /  All actions logged'),
]
for i, (label, desc) in enumerate(sec_items):
    col = i % 2; row = i // 2
    sx = 0.4 + col * 6.5
    sy3 = 1.25 + row * 1.6
    if i == 6:
        sx = 0.4; sy3 = 1.25 + 4 * 1.6 - 1.6
    sp = sl.shapes.add_shape(1, Inches(sx), Inches(sy3), Inches(6.1), Inches(1.4))
    sp.fill.solid(); sp.fill.fore_color.rgb = C_PANEL
    sp.line.color.rgb = C_CYAN; sp.line.width = Pt(0.5)
    txb(sl, label, sx+0.15, sy3+0.12, 5.8, 0.4, size=13, bold=True, color=C_CYAN)
    txb(sl, desc, sx+0.15, sy3+0.58, 5.8, 0.65, size=10, color=C_WHITE)

# --- SLIDE 11: MULTI-USER ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 11)
section_tag(sl, 'Istifadeci Idaresi / Управление пользователями / User Management')
txb(sl, 'Coxlu operatorlu sistem  /  Мультиоператорная система  /  Multi-operator system', 0.4, 0.55, 12.5, 0.5, size=20, bold=True, color=C_WHITE)

# Admin card
rect(sl, 0.4, 1.25, 5.8, 5.5, C_PANEL)
txb(sl, 'ADMINISTRATOR', 0.6, 1.45, 5.4, 0.5, size=16, bold=True, color=C_CYAN)
admin_items = [
    ('Butun tablar', 'RU: Все вкладки  /  EN: All tabs'),
    ('İstifadeci idaresi', 'RU: Управление пользователями  /  EN: User management'),
    ('VPS-e fayl yukleme', 'RU: Загрузка файлов на VPS  /  EN: VPS file upload'),
    ('Agent yenilemesi', 'RU: Обновление агента  /  EN: Agent update'),
]
ay3 = 2.05
for az, ruen in admin_items:
    txb(sl, az, 0.6, ay3, 5.4, 0.38, size=12, bold=True, color=C_WHITE)
    txb(sl, ruen, 0.6, ay3+0.4, 5.4, 0.3, size=9, color=C_MUTED)
    ay3 += 0.85

# Operator card
rect(sl, 6.8, 1.25, 6.1, 5.5, C_PANEL)
txb(sl, 'OPERATOR', 7.0, 1.45, 5.7, 0.5, size=16, bold=True, color=C_GREEN)
op_items = [
    ('Yalnız icazeli tablar', 'RU: Только разрешённые вкладки  /  EN: Permitted tabs only'),
    ('Heç bir admin funksiyasi yoxdur', 'RU: Без функций администратора  /  EN: No admin functions'),
    ('Fərdi interfeys teması', 'RU: Индивидуальная тема интерфейса  /  EN: Personal UI theme'),
    ('@username aktivlik jurnalinda', 'RU: @имя в журнале активности  /  EN: @username in activity log'),
]
oy = 2.05
for az, ruen in op_items:
    txb(sl, az, 7.0, oy, 5.7, 0.38, size=12, bold=True, color=C_WHITE)
    txb(sl, ruen, 7.0, oy+0.4, 5.7, 0.3, size=9, color=C_MUTED)
    oy += 0.85

# --- SLIDE 12: REMOTE UPDATE ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 12)
section_tag(sl, 'Uzaqdan Yenileme / Удалённое обновление / Remote Update')
txb(sl, 'Fiziki giris olmadan yenileme  /  Обновление без физического доступа  /  Update without physical access', 0.4, 0.55, 12.5, 0.5, size=18, bold=True, color=C_WHITE)

steps_u = [
    ('1', 'Yeni pnpext.dll-i VPS-e yukle', 'RU: Загрузить новый pnpext.dll на VPS  /  EN: Upload new pnpext.dll to VPS'),
    ('2', '"Agenti yenile" duymesinə bas', 'RU: Нажать "Обновить агент" в браузере  /  EN: Click "Update Agent" button in browser'),
    ('3', 'Agent faylı HTTPS ile yukledir', 'RU: Агент скачивает файл по HTTPS  /  EN: Agent downloads file via HTTPS'),
    ('4', 'Özünü evez edir, servisi yeniden basladır', 'RU: Заменяет себя, перезапускает сервис (15-30s)  /  EN: Replaces itself, restarts service (15-30s)'),
    ('5', 'Status yeniden "Online" — hazır!', 'RU: Статус снова "Online" — готово!  /  EN: Status returns "Online" — done!'),
]
for i, (num, az, ruen) in enumerate(steps_u):
    sy4 = 1.25 + i * 1.15
    # Circle
    circle = sl.shapes.add_shape(9, Inches(0.4), Inches(sy4), Inches(0.7), Inches(0.7))
    circle.fill.solid(); circle.fill.fore_color.rgb = RGBColor(0x00, 0x25, 0x35)
    circle.line.color.rgb = C_CYAN; circle.line.width = Pt(1.5)
    txb(sl, num, 0.4, sy4+0.1, 0.7, 0.5, size=14, bold=True, color=C_CYAN, align=PP_ALIGN.CENTER)
    txb(sl, az, 1.3, sy4, 11.5, 0.45, size=13, bold=True, color=C_WHITE)
    txb(sl, ruen, 1.3, sy4+0.48, 11.5, 0.35, size=10, color=C_MUTED)
    if i < 4:
        line = sl.shapes.add_shape(9, Inches(0.75), Inches(sy4+0.72), Inches(0), Inches(0.42))
        line.line.color.rgb = C_CYAN; line.line.width = Pt(1)

# Warning note
rect(sl, 0.4, 7.0, 12.5, 0.4, RGBColor(0x1a, 0x14, 0x00))
txb(sl, 'Yenileme zamanı ~5 san eleqe kesilir — bu normaldır. Windows yeniden baslatma lazım deyil.  /  RU: ~5 сек обрыв — норма. Перезагрузка не нужна.', 0.5, 7.05, 12.2, 0.3, size=9, color=RGBColor(0xe3, 0xb3, 0x41))

# --- SLIDE 13: TECH SPECS ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 13)
section_tag(sl, 'Texniki Xüsusuiyyətlər / Технические характеристики / Technical Specifications')
txb(sl, 'Data v1.0.250 — spesifikasiyalar / specifications', 0.4, 0.55, 12.5, 0.5, size=20, bold=True, color=C_WHITE)

specs = [
    ('Agent OS', 'Windows 7/8/10/11 x86/x64'),
    ('Servis / Service', 'MspIscSvc, Auto-start, DACL-protected'),
    ('Transport', 'WebSocket TLS 1.3, port 443 only'),
    ('Video', 'MJPEG / H.264, up to 60 FPS'),
    ('Audio', 'OGG Opus, WASAPI loopback, DSP chain'),
    ('Konfiq / Config', 'AES-256-CBC + PBKDF2 (pnpext.sys)'),
    ('Modullar / Modules', 'AES-256-GCM Stage-2 (RAM-only, no disk)'),
    ('İnterfeys / Interface', 'Chrome / Edge browser (no install required)'),
]
ty2 = 1.25
for i, (comp, spec) in enumerate(specs):
    bg_col = C_PANEL if i % 2 == 0 else C_BG
    sp = sl.shapes.add_shape(1, Inches(0.4), Inches(ty2), Inches(12.5), Inches(0.68))
    sp.fill.solid(); sp.fill.fore_color.rgb = bg_col
    sp.line.fill.background()
    txb(sl, comp, 0.55, ty2+0.12, 3.5, 0.45, size=12, bold=True, color=C_CYAN)
    txb(sl, spec, 4.2, ty2+0.12, 8.5, 0.45, size=12, color=C_WHITE)
    ty2 += 0.7

# --- SLIDE 14: CONTACT ---
sl = add_slide()
bg(sl); top_bar(sl); footer(sl, 14)

# Shield
shield2 = sl.shapes.add_shape(1, Inches(5.4), Inches(0.6), Inches(2.5), Inches(2.5))
shield2.fill.solid(); shield2.fill.fore_color.rgb = RGBColor(0x00, 0x25, 0x35)
shield2.line.color.rgb = C_CYAN; shield2.line.width = Pt(2)

txb(sl, 'Data', 0.5, 3.2, 12.33, 1.0, size=52, bold=True, color=C_CYAN, align=PP_ALIGN.CENTER)
txb(sl, 'v1.0.250', 0.5, 4.25, 12.33, 0.5, size=16, color=C_MUTED, align=PP_ALIGN.CENTER)

# Divider
line_div = sl.shapes.add_shape(9, Inches(3.0), Inches(4.9), Inches(7.33), Inches(0))
line_div.line.color.rgb = RGBColor(0x30, 0x36, 0x3d); line_div.line.width = Pt(1)

txb(sl, 'Suallar / Вопросы / Questions:', 0.5, 5.1, 12.33, 0.45, size=14, color=C_WHITE, align=PP_ALIGN.CENTER)
txb(sl, 'rauf.hasanov@gmail.com', 0.5, 5.6, 12.33, 0.55, size=18, bold=True, color=C_CYAN, align=PP_ALIGN.CENTER)
txb(sl, '© Data Remote Management System  ·  2026', 0.5, 6.25, 12.33, 0.45, size=11, color=C_MUTED, align=PP_ALIGN.CENTER)

# --- SAVE ---
os.makedirs(os.path.dirname(OUT), exist_ok=True)
prs.save(OUT)
size = os.path.getsize(OUT)
print(f'Saved: {OUT}')
print(f'Slides: {len(prs.slides)}')
print(f'Size: {size:,} bytes')
