#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PROMETEY v1.0.250 — Operator Manual PDF Generator
Generates Guide_RU.pdf, Guide_EN.pdf, Guide_AZ.pdf
Light theme: white bg, dark text, #0f3460 blue headings, #00bcd4 cyan accent
"""

import os
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm, cm
from reportlab.lib.colors import HexColor, white, black, Color
from reportlab.pdfgen import canvas
from reportlab.lib.utils import simpleSplit

# ── Colors ─────────────────────────────────────────────────────────────────
C_BG        = white
C_HEADER_BG = HexColor('#0f3460')
C_FOOTER_BG = HexColor('#1a1a2e')
C_HEADING   = HexColor('#0f3460')
C_ACCENT    = HexColor('#00bcd4')
C_TEXT      = HexColor('#1a1a2e')
C_TEXT_BODY = HexColor('#2d2d2d')
C_ROW_ALT   = HexColor('#f0f7ff')
C_ROW_EVEN  = white
C_SECTION_BOX = HexColor('#e8f4f8')
C_BORDER    = HexColor('#ccddee')

PAGE_W, PAGE_H = A4  # 595 x 842 pt
MARGIN_L = 2.0 * cm
MARGIN_R = 2.0 * cm
MARGIN_T = 3.5 * cm  # leaves room for header
MARGIN_B = 2.5 * cm  # leaves room for footer
CONTENT_W = PAGE_W - MARGIN_L - MARGIN_R


# ── Draw shield logo (simplified, text-based) ──────────────────────────────
def draw_logo(c, x, y, size=30):
    """Draw a simple shield shape with PROMETEY initial."""
    # Shield body (pentagon-like)
    shield_pts = [
        (x, y + size),
        (x + size * 0.8, y + size),
        (x + size * 0.8, y + size * 0.4),
        (x + size * 0.4, y),
        (x, y + size * 0.4),
    ]
    c.setFillColor(C_ACCENT)
    p = c.beginPath()
    p.moveTo(*shield_pts[0])
    for pt in shield_pts[1:]:
        p.lineTo(*pt)
    p.close()
    c.drawPath(p, fill=1, stroke=0)

    # Inner shield (darker)
    inner_margin = size * 0.12
    c.setFillColor(C_HEADER_BG)
    inner_pts = [
        (x + inner_margin, y + size - inner_margin),
        (x + size * 0.8 - inner_margin, y + size - inner_margin),
        (x + size * 0.8 - inner_margin, y + size * 0.42),
        (x + size * 0.4, y + inner_margin * 1.5),
        (x + inner_margin, y + size * 0.42),
    ]
    p2 = c.beginPath()
    p2.moveTo(*inner_pts[0])
    for pt in inner_pts[1:]:
        p2.lineTo(*pt)
    p2.close()
    c.drawPath(p2, fill=1, stroke=0)

    # "P" letter in shield
    c.setFillColor(C_ACCENT)
    c.setFont("Helvetica-Bold", size * 0.45)
    c.drawCentredString(x + size * 0.4, y + size * 0.28, "P")


# ── Page header ────────────────────────────────────────────────────────────
def draw_header(c, page_data):
    section_name = page_data.get('section', '')
    # Header bar
    c.setFillColor(C_HEADER_BG)
    c.rect(0, PAGE_H - 1.8 * cm, PAGE_W, 1.8 * cm, fill=1, stroke=0)

    # Logo
    draw_logo(c, MARGIN_L, PAGE_H - 1.65 * cm, size=22)

    # PROMETEY title
    c.setFillColor(white)
    c.setFont("Helvetica-Bold", 13)
    c.drawString(MARGIN_L + 1.8 * cm, PAGE_H - 0.8 * cm, "PROMETEY")

    # Version
    c.setFillColor(C_ACCENT)
    c.setFont("Helvetica", 9)
    c.drawString(MARGIN_L + 1.8 * cm, PAGE_H - 1.35 * cm, "v1.0.250")

    # Section name (right-aligned)
    if section_name:
        c.setFillColor(white)
        c.setFont("Helvetica", 9)
        c.drawRightString(PAGE_W - MARGIN_R, PAGE_H - 0.75 * cm, section_name)

    # Accent line under header
    c.setStrokeColor(C_ACCENT)
    c.setLineWidth(1.5)
    c.line(0, PAGE_H - 1.82 * cm, PAGE_W, PAGE_H - 1.82 * cm)


# ── Page footer ────────────────────────────────────────────────────────────
def draw_footer(c, page_num, total_pages, contact="rauf.hasanov@gmail.com"):
    # Footer bar
    c.setFillColor(HexColor('#f5f8fc'))
    c.rect(0, 0, PAGE_W, 1.6 * cm, fill=1, stroke=0)
    # Top border
    c.setStrokeColor(C_BORDER)
    c.setLineWidth(0.8)
    c.line(0, 1.62 * cm, PAGE_W, 1.62 * cm)

    c.setFillColor(HexColor('#555555'))
    c.setFont("Helvetica", 8)
    c.drawString(MARGIN_L, 0.9 * cm, "PROMETEY v1.0.250  |  " + contact)
    c.drawRightString(PAGE_W - MARGIN_R, 0.9 * cm, f"{page_num} / {total_pages}")


# ── Cover page ─────────────────────────────────────────────────────────────
def draw_cover(c, lang_data):
    c.setFillColor(C_HEADER_BG)
    c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)

    # Top accent strip
    c.setFillColor(C_ACCENT)
    c.rect(0, PAGE_H - 0.6 * cm, PAGE_W, 0.6 * cm, fill=1, stroke=0)

    # Bottom accent strip
    c.rect(0, 0, PAGE_W, 0.6 * cm, fill=1, stroke=0)

    # Large logo centered
    logo_size = 80
    logo_x = PAGE_W / 2 - logo_size * 0.4
    logo_y = PAGE_H * 0.58
    draw_logo(c, logo_x, logo_y, size=logo_size)

    # PROMETEY title
    c.setFillColor(white)
    c.setFont("Helvetica-Bold", 42)
    c.drawCentredString(PAGE_W / 2, PAGE_H * 0.50, "PROMETEY")

    # Accent underline
    c.setStrokeColor(C_ACCENT)
    c.setLineWidth(2)
    title_w = c.stringWidth("PROMETEY", "Helvetica-Bold", 42)
    c.line(PAGE_W / 2 - title_w / 2, PAGE_H * 0.49,
           PAGE_W / 2 + title_w / 2, PAGE_H * 0.49)

    # Manual title
    c.setFillColor(C_ACCENT)
    c.setFont("Helvetica-Bold", 22)
    c.drawCentredString(PAGE_W / 2, PAGE_H * 0.42, lang_data['cover_title'])

    # Subtitle
    c.setFillColor(HexColor('#aaccee'))
    c.setFont("Helvetica", 13)
    c.drawCentredString(PAGE_W / 2, PAGE_H * 0.36, lang_data['cover_subtitle'])

    # Version box
    box_w = 5 * cm
    box_h = 1.0 * cm
    box_x = PAGE_W / 2 - box_w / 2
    box_y = PAGE_H * 0.27
    c.setFillColor(C_ACCENT)
    c.roundRect(box_x, box_y, box_w, box_h, 5, fill=1, stroke=0)
    c.setFillColor(C_HEADER_BG)
    c.setFont("Helvetica-Bold", 14)
    c.drawCentredString(PAGE_W / 2, box_y + 0.25 * cm, "v1.0.250")

    # Contact
    c.setFillColor(HexColor('#8899aa'))
    c.setFont("Helvetica", 10)
    c.drawCentredString(PAGE_W / 2, PAGE_H * 0.20, "rauf.hasanov@gmail.com")

    # Small decorative dots row
    for i in range(5):
        cx = PAGE_W / 2 + (i - 2) * 15
        c.setFillColor(HexColor('#1a4a7a') if i != 2 else C_ACCENT)
        c.circle(cx, PAGE_H * 0.14, 3, fill=1, stroke=0)


# ── Table of Contents ──────────────────────────────────────────────────────
def draw_toc(c, lang_data, page_map):
    draw_header(c, {'section': lang_data['toc_title']})
    draw_footer(c, 2, lang_data['total_pages'])

    y = PAGE_H - MARGIN_T - 0.5 * cm

    # TOC title
    c.setFillColor(C_HEADING)
    c.setFont("Helvetica-Bold", 20)
    c.drawString(MARGIN_L, y, lang_data['toc_title'])
    y -= 0.5 * cm

    # Underline
    c.setStrokeColor(C_ACCENT)
    c.setLineWidth(2)
    c.line(MARGIN_L, y, MARGIN_L + 6 * cm, y)
    y -= 0.7 * cm

    sections = lang_data['sections']
    for i, sec in enumerate(sections):
        num = i + 1
        pg = page_map.get(num, num + 2)

        # Alternating row bg
        if i % 2 == 0:
            c.setFillColor(C_ROW_ALT)
            c.rect(MARGIN_L - 4, y - 4, CONTENT_W + 8, 0.65 * cm, fill=1, stroke=0)

        # Number badge
        c.setFillColor(C_ACCENT)
        c.circle(MARGIN_L + 0.35 * cm, y + 0.18 * cm, 0.28 * cm, fill=1, stroke=0)
        c.setFillColor(white)
        c.setFont("Helvetica-Bold", 8)
        c.drawCentredString(MARGIN_L + 0.35 * cm, y + 0.13 * cm, str(num))

        # Section name
        c.setFillColor(C_TEXT)
        c.setFont("Helvetica", 11)
        c.drawString(MARGIN_L + 0.85 * cm, y + 0.1 * cm, sec['title'])

        # Dots leader
        c.setFillColor(HexColor('#aaaaaa'))
        c.setFont("Helvetica", 9)
        dot_x = MARGIN_L + 0.85 * cm + c.stringWidth(sec['title'], "Helvetica", 11) + 5
        dot_end = PAGE_W - MARGIN_R - 1.2 * cm
        while dot_x < dot_end:
            c.drawString(dot_x, y + 0.1 * cm, ".")
            dot_x += 5

        # Page number
        c.setFillColor(C_HEADING)
        c.setFont("Helvetica-Bold", 10)
        c.drawRightString(PAGE_W - MARGIN_R, y + 0.1 * cm, str(pg))

        y -= 0.72 * cm

    # Note box at bottom
    y -= 0.5 * cm
    c.setFillColor(HexColor('#fff8e1'))
    c.roundRect(MARGIN_L, y - 1.8 * cm, CONTENT_W, 1.8 * cm, 6, fill=1, stroke=0)
    c.setStrokeColor(HexColor('#ffcc00'))
    c.setLineWidth(1)
    c.roundRect(MARGIN_L, y - 1.8 * cm, CONTENT_W, 1.8 * cm, 6, fill=0, stroke=1)
    c.setFillColor(HexColor('#555500'))
    c.setFont("Helvetica-Bold", 9)
    c.drawString(MARGIN_L + 0.3 * cm, y - 0.5 * cm, lang_data['note_label'])
    c.setFont("Helvetica", 9)
    c.drawString(MARGIN_L + 0.3 * cm, y - 1.0 * cm, lang_data['note_text1'])
    c.drawString(MARGIN_L + 0.3 * cm, y - 1.45 * cm, lang_data['note_text2'])


# ── Text helper: wrap and draw paragraph ──────────────────────────────────
def draw_paragraph(c, text, x, y, width, font="Helvetica", size=10, color=C_TEXT_BODY,
                   line_spacing=14, indent=0):
    """Draw wrapped paragraph, return new y position."""
    c.setFillColor(color)
    c.setFont(font, size)
    lines = simpleSplit(text, font, size, width - indent)
    for line in lines:
        if y < MARGIN_B + 0.5 * cm:
            return y  # caller must handle overflow
        c.drawString(x + indent, y, line)
        y -= line_spacing
    return y


def draw_bullet(c, text, x, y, width, bullet="[->]", font="Helvetica", size=10,
                color=C_TEXT_BODY, line_spacing=14):
    """Draw a bulleted line."""
    bullet_w = c.stringWidth(bullet + "  ", font, size)
    c.setFillColor(C_ACCENT)
    c.setFont(font, size)
    c.drawString(x, y, bullet)
    c.setFillColor(color)
    lines = simpleSplit(text, font, size, width - bullet_w)
    first = True
    for line in lines:
        c.drawString(x + bullet_w, y, line)
        y -= line_spacing
        if first and len(lines) > 1:
            first = False
    return y


# ── Section heading box ────────────────────────────────────────────────────
def draw_section_heading(c, number, title, y):
    """Draw a colored section heading box, return new y."""
    box_h = 0.75 * cm
    c.setFillColor(HexColor('#e8f0fb'))
    c.roundRect(MARGIN_L - 2, y - box_h + 2, CONTENT_W + 4, box_h, 4, fill=1, stroke=0)
    c.setFillColor(C_HEADING)
    c.setLineWidth(2)
    c.line(MARGIN_L - 2, y - box_h + 2, MARGIN_L - 2, y + 2)

    c.setFillColor(C_HEADING)
    c.setFont("Helvetica-Bold", 14)
    num_str = f"{number}. " if number else ""
    c.drawString(MARGIN_L + 0.2 * cm, y - 0.48 * cm, num_str + title)
    return y - box_h - 0.4 * cm


# ── Sub-heading ────────────────────────────────────────────────────────────
def draw_subheading(c, text, y, color=C_ACCENT):
    c.setFillColor(color)
    c.setFont("Helvetica-Bold", 11)
    c.drawString(MARGIN_L, y, text)
    c.setStrokeColor(color)
    c.setLineWidth(0.5)
    c.line(MARGIN_L, y - 2, MARGIN_L + c.stringWidth(text, "Helvetica-Bold", 11), y - 2)
    return y - 0.55 * cm


# ── Two-column key/value table ─────────────────────────────────────────────
def draw_kv_table(c, rows, x, y, col1_w=5.5 * cm, row_h=0.52 * cm):
    """Draw a simple two-column table. Returns new y."""
    total_w = CONTENT_W
    col2_w = total_w - col1_w
    for i, (key, val) in enumerate(rows):
        if y < MARGIN_B + 0.5 * cm:
            break
        bg = C_ROW_ALT if i % 2 == 0 else white
        c.setFillColor(bg)
        c.rect(x, y - row_h, total_w, row_h, fill=1, stroke=0)
        c.setStrokeColor(C_BORDER)
        c.setLineWidth(0.3)
        c.rect(x, y - row_h, total_w, row_h, fill=0, stroke=1)

        c.setFillColor(C_HEADING)
        c.setFont("Helvetica-Bold", 9)
        c.drawString(x + 4, y - row_h + 0.13 * cm, key)

        c.setFillColor(C_TEXT_BODY)
        c.setFont("Helvetica", 9)
        # Wrap value if needed
        val_lines = simpleSplit(str(val), "Helvetica", 9, col2_w - 8)
        c.drawString(x + col1_w + 4, y - row_h + 0.13 * cm, val_lines[0] if val_lines else "")
        y -= row_h
    return y


# ── Troubleshooting table ──────────────────────────────────────────────────
def draw_trouble_table(c, rows, x, y, lang_data):
    """3-column trouble table: Problem | Cause | Solution"""
    col_w = [CONTENT_W * 0.3, CONTENT_W * 0.3, CONTENT_W * 0.4]
    headers = lang_data['trouble_headers']
    row_h = 0.55 * cm

    # Header row
    c.setFillColor(C_HEADING)
    c.rect(x, y - row_h, CONTENT_W, row_h, fill=1, stroke=0)
    c.setFillColor(white)
    c.setFont("Helvetica-Bold", 9)
    cx = x
    for i, h in enumerate(headers):
        c.drawString(cx + 4, y - row_h + 0.15 * cm, h)
        cx += col_w[i]
    y -= row_h

    for idx, (prob, cause, sol) in enumerate(rows):
        # Estimate height needed
        prob_lines = simpleSplit(prob, "Helvetica", 8, col_w[0] - 8)
        cause_lines = simpleSplit(cause, "Helvetica", 8, col_w[1] - 8)
        sol_lines = simpleSplit(sol, "Helvetica", 8, col_w[2] - 8)
        max_lines = max(len(prob_lines), len(cause_lines), len(sol_lines))
        rh = max(row_h, max_lines * 11 + 6)

        if y - rh < MARGIN_B:
            break

        bg = C_ROW_ALT if idx % 2 == 0 else white
        c.setFillColor(bg)
        c.rect(x, y - rh, CONTENT_W, rh, fill=1, stroke=0)
        c.setStrokeColor(C_BORDER)
        c.setLineWidth(0.3)
        c.rect(x, y - rh, CONTENT_W, rh, fill=0, stroke=1)

        # Draw vertical dividers
        cx = x + col_w[0]
        c.line(cx, y - rh, cx, y)
        cx += col_w[1]
        c.line(cx, y - rh, cx, y)

        # Draw text in each column
        for col_idx, (lines, cw) in enumerate(
                [(prob_lines, col_w[0]), (cause_lines, col_w[1]), (sol_lines, col_w[2])]):
            tx = x + sum(col_w[:col_idx]) + 4
            ty = y - 8
            c.setFillColor(C_TEXT_BODY)
            c.setFont("Helvetica", 8)
            for line in lines:
                c.drawString(tx, ty, line)
                ty -= 11

        y -= rh
    return y


# ══════════════════════════════════════════════════════════════════════════
#  CONTENT DATA
# ══════════════════════════════════════════════════════════════════════════

LANGUAGES = {
    'RU': {
        'filename': 'Guide_RU.pdf',
        'cover_title': 'Rukovodstvo Operatora',
        'cover_subtitle': 'Sistema Udalyonnogo Upravleniya i Monitoringa',
        'toc_title': 'Soderzhanie',
        'note_label': 'VAZHNO:',
        'note_text1': 'Vse tekhnicheskiye operatsii vypolnyayutsya ot imeni SYSTEM.',
        'note_text2': 'Trebuyetsya avtorizatsiya vladeltsa ili administratora IT.',
        'total_pages': 18,
        'trouble_headers': ['Problema', 'Prichina', 'Resheniye'],
        'sections': [
            {
                'num': 1, 'title': 'Ustanovka agenta',
                'content': [
                    ('heading', 'Opisaniye'),
                    ('para', 'Agent ustanavlivayetsya na kompyuter obekta odin raz. Avtomaticheski zapuskayetsya kak sluzhba Windows MspIscSvc pri kazhdoy zagruzke sistemy.'),
                    ('heading', 'Trebovaniya'),
                    ('kv', [
                        ('pnpext.dll', 'Agent (osnovnoy modul)'),
                        ('pnpext.sys', 'Zashiprovannyy konfig AES-256'),
                        ('install.bat', 'Ustanovshchik (zapusk ot admina)'),
                        ('uninstall.bat', 'Udaleniye agenta'),
                    ]),
                    ('heading', 'Sposob 1 - USB (rekomenduyetsya)'),
                    ('bullet', 'Skopiruite dist/usb na fleshku'),
                    ('bullet', 'Vstavte v kompyuter obekta'),
                    ('bullet', 'PKM na install.bat -> Zapusk ot administratora'),
                    ('bullet', 'Podtverdite zapros UAC'),
                    ('bullet', 'Okno zakrylos = ustanovka zavershena'),
                    ('heading', 'Sposob 2 - Brauzer'),
                    ('bullet', 'Otkroyte brauzer na komp. obekta'),
                    ('bullet', 'Zaydte na VPS cherez brauzer'),
                    ('bullet', 'Voydte pod uchetnoj zapisyu'),
                    ('bullet', 'Nazhimte "Skachat ustanovshchik"'),
                    ('bullet', 'Zapustite install-web.bat ot administratora'),
                    ('heading', 'Udaleniye'),
                    ('para', 'Zapustite uninstall.bat ot imeni administratora. Sluzhba ostanovitsya i budet udalena.'),
                    ('heading', 'Nazvanie sluzhby Windows'),
                    ('kv', [
                        ('Imya sluzhby', 'MspIscSvc'),
                        ('Tochka vkhoda', 'PnpServiceEntry'),
                        ('Tip zapuska', 'Avtomaticheski'),
                        ('Kontekst', 'LocalSystem (SYSTEM)'),
                    ]),
                ]
            },
            {
                'num': 2, 'title': 'Vkhod v panel',
                'content': [
                    ('heading', 'Dostup k paneli operatora'),
                    ('para', 'Panel operatora - obychnaya veb-stranitsa. Nikakogo PO ustanavlivat ne nuzhno. Rabotayet v Chrome, Edge, Firefox.'),
                    ('heading', 'Poryadok vkhoda'),
                    ('bullet', 'Otkroyte Chrome ili Edge'),
                    ('bullet', 'Vvedite adres: https://[IP-adres VPS]'),
                    ('bullet', 'Brauzer pokazhet preduprezhdenie o sertifikate'),
                    ('bullet', '"Dopolnitelno" -> "Pereyti na sayt" (sertifikat samopodpisannyy - eto normalno)'),
                    ('bullet', 'Vvedite login i parol'),
                    ('bullet', 'Nazhimte "Voyti"'),
                    ('heading', 'Statusy podklyucheniya'),
                    ('kv', [
                        ('[ONLINE] zelenyy', 'Agent podklyuchen i rabotayet'),
                        ('[OFFLINE] krasnyy', 'Agent ne v seti (komandy v ocheredi)'),
                        ('[RECONNECT]', 'Povtornoe podklyucheniye...'),
                    ]),
                    ('heading', 'Zamechaniya po sertifikatu'),
                    ('para', 'Ispolzuyetsya samopodpisannyy TLS-sertifikat - eto normalno dlya vnutrenney infrastruktury. Preduprezhdenie brauzera - ozhidayemoye povedeniye, a ne oshibka.'),
                ]
            },
            {
                'num': 3, 'title': 'Panel monitoringa (Dashbord)',
                'content': [
                    ('heading', 'Obzor'),
                    ('para', 'Vkladka Dashbord pokazyvayet sostoyaniye obekta v realnom vremeni. Vse pokazateli obnovlyayutsya avtomaticheski cherez WebSocket.'),
                    ('heading', 'Pokazateli sistemy'),
                    ('kv', [
                        ('CPU %', 'Nagruzka protsessora v real. vremeni'),
                        ('RAM', 'Ispolzovaniye pamyati (MB / %)'),
                        ('GPU', 'Nagruzka GPU (yesli dostupno)'),
                        ('Disk', 'Aktivnost diska'),
                    ]),
                    ('heading', 'Karty VPS1 i VPS2'),
                    ('para', 'Karty VPS1 i VPS2 otobrazhayt status serverov-retranslyatorov nezavisimo - u kazhdoy svoya knopka [Obnovit]. Ispolzuyte dlya diagnostiki podklyucheniya.'),
                    ('heading', 'Test skorosti'),
                    ('para', 'Izmeryayet skorost soedineniya s VPS. Poleznо dlya otsenki kachestva kanala do obkta.'),
                    ('heading', 'Zhurnal aktivnosti'),
                    ('para', 'Fiksiruyet vse deystviya operatorov s @imenem polzovatelya i vremenem. Khranitsya na VPS. Nedostupno dlya redaktirovaniya.'),
                ]
            },
            {
                'num': 4, 'title': 'Ekran (Strim)',
                'content': [
                    ('heading', 'Nachalo translyatsii'),
                    ('para', 'Vkladka "Ekran". Nazhimte knopku "Start" - translyatsiya nachinaetsya. Zvuk i video peredayutsya otdelno.'),
                    ('heading', 'Parametry strima'),
                    ('kv', [
                        ('FPS (1-60)', '15=norma, 30=plavno, 60=maksimum'),
                        ('Kachestvo (1-100)', '75 po umolchaniyu'),
                        ('Masshtab (%)', 'Umensheniye na storone obekta'),
                        ('Kodek', 'MJPEG ili H.264'),
                    ]),
                    ('heading', 'Knopki upravleniya (plavayushchaya panel)'),
                    ('para', 'Panel nakhoditsya v pravom nizhnem uglu okna strima:'),
                    ('kv', [
                        ('[FIT/FILL]', 'Pereklyucheniye rezhima otobrazheniya'),
                        ('[SCREENSHOT]', 'Sokhranit PNG v papku Zagruzki'),
                        ('[AUDIO]', 'Sistemnyy zvuk cherez WASAPI loopback'),
                        ('[REC]', 'Zapis video v .webm fayl'),
                        ('[FULLSCREEN]', 'Polnoekrannyy rezhim'),
                    ]),
                    ('heading', 'Zapis so zvukom'),
                    ('para', 'Chtoby zapisat video so zvukom: 1) Vklyuchite [AUDIO]. 2) Nazhimte [REC]. Posledovatelnost vazhna!'),
                ]
            },
            {
                'num': 5, 'title': 'Faylovyy menedzher',
                'content': [
                    ('heading', 'Dostup k falovoy sisteme'),
                    ('para', 'Polnyy dostup k falovoy sisteme obekta. Rabota so vsemi diskami, setevymi resursami.'),
                    ('heading', 'Navigatsiya'),
                    ('bullet', 'Klik po papke - otkryt'),
                    ('bullet', 'Knopka [Vverh] ili khlebnye kroshki - naverh'),
                    ('bullet', 'Klik po imeni fayla - skachat'),
                    ('bullet', 'Bolshiye fayly: peredacha po segmentam s progress-bar'),
                    ('heading', 'Zagruzka faylov na obekt'),
                    ('bullet', 'Nazhimte knopku "Zagruzit"'),
                    ('bullet', 'Ili peretashchite fayl na panel'),
                    ('bullet', 'Fayl popadayet v tekushchuyu direktoriu na obekte'),
                    ('heading', 'Operatsii s faylami'),
                    ('kv', [
                        ('Skachat', 'Klik po imeni - segmentnyy skachat s progress-bar'),
                        ('Zagruzit', 'Knopka ili drag&drop'),
                        ('Udalit', 'Knopka Udalit v kontekste'),
                        ('Sozdat papku', 'Knopka Novaya papka'),
                    ]),
                ]
            },
            {
                'num': 6, 'title': 'Protsessy i servisy',
                'content': [
                    ('heading', 'Vkladka Protsessy'),
                    ('para', 'Pokazyvayet vse aktyvnye protsessy s PID, imenem, nagruzkoy CPU i ispolzovaniyem RAM. Obnovleniye - po zaprose.'),
                    ('kv', [
                        ('PID', 'Identifikator protsessa'),
                        ('Imya', 'Imya ispolnyayemogo fayla'),
                        ('CPU%', 'Nagruzka protsessora'),
                        ('RAM', 'Obuem ispolzovannoy pamyati'),
                    ]),
                    ('heading', 'Deystviya s protsessami'),
                    ('bullet', 'Vyberte protsess -> nazhimte "Zavershit"'),
                    ('bullet', 'Pole "Zapustit" + Enter - zapustit programmu ot SYSTEM'),
                    ('heading', 'Vkladka Servisy'),
                    ('kv', [
                        ('Zapustit', 'Startovat servis'),
                        ('Ostanovit', 'Ostanovit servis'),
                        ('Perezapustit', 'Restart servisa'),
                        ('Tip zapuska', 'Avto / Vruchnuyu / Otklyuchen'),
                    ]),
                ]
            },
            {
                'num': 7, 'title': 'Terminal',
                'content': [
                    ('heading', 'Opisaniye'),
                    ('para', 'Polnotsennaya komandnaya stroka. Komandy vypolnyayutsya ot imeni SYSTEM s maksimalnymi pravami (polnye prava administratora).'),
                    ('heading', 'Rezhimy'),
                    ('kv', [
                        ('cmd', 'Windows Command Prompt'),
                        ('PowerShell', 'Windows PowerShell'),
                    ]),
                    ('heading', 'Osobennosti'),
                    ('bullet', 'Interaktivnye komandy (pause, choice) mogut zависnut'),
                    ('bullet', 'Ispolzuyte /Y, -Force, -Confirm:$false'),
                    ('bullet', 'Komanda cd menyayet direktoriu dlya tekushchego seansa'),
                    ('bullet', 'Vse operatsii - ot imeni SYSTEM'),
                    ('heading', 'Primery komand'),
                    ('kv', [
                        ('ipconfig /all', 'Parametry seti'),
                        ('netstat -ano', 'Aktivnye soedineniya'),
                        ('tasklist', 'Spisok protsessov'),
                        ('sc query', 'Sostoyanie servisov'),
                        ('reg query HKLM\\...', 'Zapros reyestra'),
                    ]),
                ]
            },
            {
                'num': 8, 'title': 'Reyestr',
                'content': [
                    ('heading', 'Brauzer reyestra'),
                    ('para', 'Polnyy dostup k reyestru Windows s brauzeram. Navigatsiya identichna provodnikу.'),
                    ('heading', 'Vety (Kuste) reyestra'),
                    ('kv', [
                        ('HKLM', 'HKEY_LOCAL_MACHINE - nastroyki mashiny'),
                        ('HKCU', 'HKEY_CURRENT_USER - nastroyki polzovatelya'),
                        ('HKCR', 'HKEY_CLASSES_ROOT - assotsatsii faylov'),
                        ('HKU', 'HKEY_USERS - profili polzovateley'),
                        ('HKCC', 'HKEY_CURRENT_CONFIG - tekushchaya konf.'),
                    ]),
                    ('heading', 'Deystviya'),
                    ('bullet', 'Klik po klyuchu - pokazat znacheniya sprava'),
                    ('bullet', 'PKM po znacheniyu -> "Izmenit" - redaktirovat'),
                    ('bullet', 'PKM -> "Udalit" - udalit (NEOBRATIMO!)'),
                    ('heading', '[WARN] Preduprezhdenie'),
                    ('para', 'Udaleniye klyuchey reyestra NEOBRATIMO. Nevernye izmeneniya mogut narushat rabotu sistemy. Vsegda sozdavayte rezervnuyu kopiyu pered izmeneniyami.'),
                ]
            },
            {
                'num': 9, 'title': 'Skrinshoты',
                'content': [
                    ('heading', 'Avtomaticheskiye snimki ekrana'),
                    ('para', 'Agent delayet snimki ekrana avtomaticheski. Po umolchaniyu - kazhdye 10 sekund. Khranyatsya na VPS.'),
                    ('heading', 'Galeriya'),
                    ('bullet', 'Miniatyury v osnovnom prostranstve'),
                    ('bullet', 'Klik po miniatyure -> predprosmotr sprava'),
                    ('bullet', 'Klik po prosmatrivaemomu snimku -> polnyy ekran'),
                    ('bullet', 'Vybrat -> Udalit: predprosmotr ochishchaetsya avtomaticheski'),
                    ('heading', 'Parametry'),
                    ('kv', [
                        ('Interval', '10 sekund (nastrayivayetsya)'),
                        ('Kachestvo', '75% (JPEG)'),
                        ('Masshtab', '50% (umensheniye)'),
                        ('Rezhim', 'Vsegda / Filtr prilozheniy'),
                    ]),
                    ('heading', 'Filtr prilozheniy'),
                    ('para', 'Mozhno nastroit zapis tolko kogda na perednems plane nakhoditsya konkretnoye prilosheniye (napr. brauzer ili Excel).'),
                ]
            },
            {
                'num': 10, 'title': 'Audio',
                'content': [
                    ('heading', 'Zapis mikrofona'),
                    ('para', 'Nepreryvnaya zapis mikrofona obekta. Segmenty po 5 minut (format OGG Opus). Khranyatsya na VPS.'),
                    ('heading', 'Pleyer'),
                    ('bullet', 'Klik po faylu - nachinayet vosproizvedeniye'),
                    ('bullet', 'Udaleniye igrayushchego fayla - pleyer ochishchaetsya avtomaticheski'),
                    ('heading', 'Tekhnicheskiye detali'),
                    ('kv', [
                        ('Interfeys', 'WASAPI loopback ot SYSTEM'),
                        ('Indikator Windows', 'Ne poyavlyayetsya'),
                        ('Format', 'OGG Opus'),
                        ('Dlinya segmenta', '300 sekund (5 minut)'),
                        ('Chastota deskritizatsii', '16000 Hz'),
                        ('Bitreyt', '128 kbit/s'),
                        ('Usileniye', '100%'),
                    ]),
                    ('heading', 'DSP-nastroyki'),
                    ('kv', [
                        ('Shumopodavleniye', 'Vklyucheno po umolchaniyu'),
                        ('Kham-filtr', '50 Hz'),
                    ]),
                ]
            },
            {
                'num': 11, 'title': 'Zhurnal sobytiy',
                'content': [
                    ('heading', 'Prosmotr zhurnala Windows'),
                    ('para', 'Prosmotrshchik zhurnala sobytiy Windows. Ispolzuyetsya dlya diagnostiki sboev i problem na obekte.'),
                    ('heading', 'Istochniki'),
                    ('kv', [
                        ('Application', 'Sobytiya prilozheniy'),
                        ('System', 'Sistemnyye sobytiya'),
                        ('Security', 'Sobytiya bezopasnosti'),
                    ]),
                    ('heading', 'Filtry'),
                    ('kv', [
                        ('Oshibka', 'Kriticheskiye oshibki'),
                        ('Preduprezhdenie', 'Preduprezhdayushchiye soobshcheniya'),
                        ('Informatsiya', 'Informatsionnyye sobytiya'),
                    ]),
                    ('heading', 'Ispolzovaniye'),
                    ('bullet', 'Klik po zapisi -> podrobnosti vnizu'),
                    ('bullet', 'Filtr po istochniku i urovnyu'),
                    ('bullet', 'Obnovleniye po zaprose'),
                ]
            },
            {
                'num': 12, 'title': 'Zashchita',
                'content': [
                    ('heading', 'Windows Defender'),
                    ('para', 'Vkladka Zashchita predostavlyayet dostup k Windows Defender i parametram zhurnala sobytiy.'),
                    ('heading', 'Operatsii s Defender'),
                    ('kv', [
                        ('Vklyuchit/otklyuchit', 'Upravleniye zashchitoy v realnom vremeni'),
                        ('Zapustit skanirovanie', 'Polnoye ili bystroye skanirovanie'),
                        ('Karantin', 'Prosmotr i upravleniye'),
                        ('Rezultaty skan.', 'Posledneye skanirovanie + naydennye ugrozy'),
                    ]),
                    ('heading', 'Nastroyki zhurnala sobytiy'),
                    ('para', 'Nastroyki avtomaticheskoy ochistki zhurnala sobytiy Windows. Udalyayet shumnye zapisi vremeni ustanovki dlya ochishcheniya zhurnala.'),
                    ('heading', 'Rekomendatsiya'),
                    ('para', 'Operatsii s Defender trebuut tschatelnosti. Otklucheniye zashchity dolzhno byt vremennym i s ponimaniyem riskov.'),
                ]
            },
            {
                'num': 13, 'title': 'Nastroyki',
                'content': [
                    ('heading', 'Vkladki nastroyek'),
                    ('kv', [
                        ('Obnovleniye', 'Zagruzka i obnovleniye pnpext.dll'),
                        ('Razvertyvanie', 'Adres VPS i token komnaty'),
                        ('Konfig', 'Vse parametry agenta'),
                        ('Ugrozy', 'Monitoring bezopasnosti'),
                    ]),
                    ('heading', 'Konfig agenta (vkladka Konfig)'),
                    ('kv', [
                        ('Strim', 'FPS, kachestvo, kodek, masshtab'),
                        ('Skrinshoты', 'Interval, kachestvo, rezhim'),
                        ('Audio', 'Dliyna segm., chastota, bitreyt, DSP'),
                        ('STUN/TURN', 'Adresa serverov dlya WebRTC'),
                        ('Ochistka zhurnala', 'Avto-ochistka Event Log'),
                    ]),
                    ('heading', 'Razvertyvanie (vkladka Deploy)'),
                    ('para', 'Adres VPS: wss://[IP]/ws  |  Token komnaty: unikaknyy identifikator sessii. Izmenite token pri zamene agenta.'),
                ]
            },
            {
                'num': 14, 'title': 'Obnovleniye',
                'content': [
                    ('heading', 'Udalyonnoe obnovleniye agenta'),
                    ('para', 'Agent obnovlyayetsya udalyonno bez fizicheskogo dostupa k ob\'ektu. Protsess zanimayet 15-30 sekund.'),
                    ('heading', 'Porsyadok obnovleniya'),
                    ('bullet', '1. Parametry -> Obnovleniye -> Vybrat fayl (pnpext.dll)'),
                    ('bullet', '2. Nazhimte "Zagruzit na VPS"'),
                    ('bullet', '3. Nazhimte "Obnovit agent"'),
                    ('bullet', '4. Agent skachyvayet fayl i zamenyayet staryy'),
                    ('bullet', '5. Sluzhba avtomaticheski perezapuskayetsya'),
                    ('bullet', '6. Soedineniye kratkovremennо prerayvayetsya (~5 sek)'),
                    ('bullet', '7. Status "Online" = obnovleniye zaversheno'),
                    ('heading', 'Parametry'),
                    ('kv', [
                        ('Vremya obnovleniya', '15-30 sekund'),
                        ('Pereryv svyazi', '~5 sekund'),
                        ('Rezultat', 'Status Online'),
                        ('Otkat', 'Zagruzite predyduyu versiyu DLL'),
                    ]),
                ]
            },
            {
                'num': 15, 'title': 'Polzovateli',
                'content': [
                    ('heading', 'Roli polzovateley'),
                    ('kv', [
                        ('Administrator', 'Vse vkladki + uprav. polzovatelyami'),
                        ('Operator', 'Tolko razreshennye vkladki'),
                    ]),
                    ('heading', 'Dobavleniye polzovatelya'),
                    ('bullet', 'Vkladka "Polzovateli" (tolko admin)'),
                    ('bullet', 'Nazhimte "Dobavit"'),
                    ('bullet', 'Zapolnite: login / parol / rol / vkladki'),
                    ('bullet', 'Nazhimte "Sokhranit"'),
                    ('heading', 'Zhurnal aktivnosti'),
                    ('para', 'Kazhdyy operator vidit svoyo @imya v zhurnale aktivnosti. Vsye deystviya protokoliruyutsya avtomaticheski.'),
                    ('heading', 'Tema interfeysa'),
                    ('para', 'Kazhdyy polzovatel mozhet vybrat sobstvennuyu tsvetovuyu temu (knopka [TEMA] v shapke paneli).'),
                ]
            },
            {
                'num': 16, 'title': 'Ustraneniye nepoladok',
                'content': [
                    ('heading', 'Tablitsa problem i resheniy'),
                    ('trouble', [
                        ('Status OFFLINE', 'Obekt vyklyuchen / net seti', 'Proverte pitaniye i podklyucheniye Interneta na obuekte'),
                        ('Strim ne startuyut', 'Agent ne zapushchen', 'Proverte sluzhbu MspIscSvc v services.msc'),
                        ('Chёrnyy ekran', 'Blokirovka ekrana / net prav', 'Razblokruyte sessiyu ili perezapustite agent'),
                        ('Fayl ne skachivayetsya', 'Nedo. prava / malen. bufer', 'Ispolzuyte terminal dlya kopirovaniya fayla'),
                        ('Audio net zvuka', 'WASAPI nedostupno', 'Proverte zvukovuye ustroystva Windows'),
                        ('Zapis ne strikhtuvayut', 'Disk polnyy na VPS', 'Ochstite starye fayly na /opt/remotedesk/'),
                        ('Obnovl. zavislo', 'Net prav na zamenu DLL', 'Ostanovite sluzhbu vruchnuyu cherez terminal'),
                        ('Login ne rabotayet', 'Nevern. parol / kuky', 'Ochstite kuki brauzera, povtorite vkhod'),
                    ]),
                ]
            },
        ]
    },

    'EN': {
        'filename': 'Guide_EN.pdf',
        'cover_title': 'Operator Manual',
        'cover_subtitle': 'Remote Management & Monitoring System',
        'toc_title': 'Table of Contents',
        'note_label': 'IMPORTANT:',
        'note_text1': 'All technical operations run as SYSTEM context.',
        'note_text2': 'Requires authorization from device owner or IT administrator.',
        'total_pages': 18,
        'trouble_headers': ['Problem', 'Cause', 'Solution'],
        'sections': [
            {
                'num': 1, 'title': 'Agent Installation',
                'content': [
                    ('heading', 'Description'),
                    ('para', 'The agent is installed on the target computer once and runs automatically as Windows service MspIscSvc on every system boot.'),
                    ('heading', 'Requirements'),
                    ('kv', [
                        ('pnpext.dll', 'Agent (main module)'),
                        ('pnpext.sys', 'AES-256 encrypted config'),
                        ('install.bat', 'Installer (run as admin)'),
                        ('uninstall.bat', 'Agent removal'),
                    ]),
                    ('heading', 'Method 1 - USB (Recommended)'),
                    ('bullet', 'Copy dist/usb to a flash drive'),
                    ('bullet', 'Insert into target computer'),
                    ('bullet', 'Right-click install.bat -> Run as Administrator'),
                    ('bullet', 'Confirm UAC prompt'),
                    ('bullet', 'Window closes = installation complete'),
                    ('heading', 'Method 2 - Browser'),
                    ('bullet', 'Open browser on target machine'),
                    ('bullet', 'Navigate to VPS in browser'),
                    ('bullet', 'Log in with operator credentials'),
                    ('bullet', 'Click "Download installer"'),
                    ('bullet', 'Run install-web.bat as Administrator'),
                    ('heading', 'Uninstall'),
                    ('para', 'Run uninstall.bat as Administrator. The service will stop and be removed cleanly.'),
                    ('heading', 'Windows Service Details'),
                    ('kv', [
                        ('Service name', 'MspIscSvc'),
                        ('Entry point', 'PnpServiceEntry'),
                        ('Startup type', 'Automatic'),
                        ('Context', 'LocalSystem (SYSTEM)'),
                    ]),
                ]
            },
            {
                'num': 2, 'title': 'Panel Login',
                'content': [
                    ('heading', 'Accessing the Operator Panel'),
                    ('para', 'The operator panel is an ordinary web page. No software installation required. Works in Chrome, Edge, Firefox.'),
                    ('heading', 'Login Steps'),
                    ('bullet', 'Open Chrome or Edge browser'),
                    ('bullet', 'Navigate to: https://[VPS IP address]'),
                    ('bullet', 'Browser will show a certificate warning'),
                    ('bullet', 'Click Advanced -> Proceed to site (self-signed certificate - this is normal)'),
                    ('bullet', 'Enter username and password'),
                    ('bullet', 'Click "Log In"'),
                    ('heading', 'Connection Status'),
                    ('kv', [
                        ('[ONLINE] green', 'Agent connected and operational'),
                        ('[OFFLINE] red', 'Agent offline (commands queue up)'),
                        ('[RECONNECT]', 'Reconnecting...'),
                    ]),
                    ('heading', 'Certificate Note'),
                    ('para', 'A self-signed TLS certificate is used - this is standard practice for private infrastructure. The browser warning is expected behavior, not an error.'),
                ]
            },
            {
                'num': 3, 'title': 'Dashboard',
                'content': [
                    ('heading', 'Overview'),
                    ('para', 'The Dashboard tab shows target machine status in real time. All metrics are updated automatically via WebSocket connection.'),
                    ('heading', 'System Metrics'),
                    ('kv', [
                        ('CPU %', 'Processor load in real time'),
                        ('RAM', 'Memory usage (MB / %)'),
                        ('GPU', 'GPU load (if available)'),
                        ('Disk', 'Disk activity'),
                    ]),
                    ('heading', 'VPS1 and VPS2 Cards'),
                    ('para', 'VPS1 and VPS2 cards show relay server status independently - each has its own [Refresh] button. Use to diagnose connection issues.'),
                    ('heading', 'Speed Test'),
                    ('para', 'Measures connection speed to VPS. Useful for assessing channel quality to the target.'),
                    ('heading', 'Activity Log'),
                    ('para', 'Records all operator actions with @username and timestamp. Stored on VPS. Cannot be edited by operators.'),
                ]
            },
            {
                'num': 4, 'title': 'Screen (Stream)',
                'content': [
                    ('heading', 'Starting a Stream'),
                    ('para', 'Screen tab. Click the "Start" button to begin streaming. Audio and video are transmitted separately.'),
                    ('heading', 'Stream Parameters'),
                    ('kv', [
                        ('FPS (1-60)', '15=normal, 30=smooth, 60=maximum'),
                        ('Quality (1-100)', 'Default: 75'),
                        ('Scale (%)', 'Image downscale on target side'),
                        ('Codec', 'MJPEG or H.264'),
                    ]),
                    ('heading', 'Action Buttons (floating pill)'),
                    ('para', 'The floating pill is at the bottom-right corner of the stream window:'),
                    ('kv', [
                        ('[FIT/FILL]', 'Toggle between fit and fill display modes'),
                        ('[SCREENSHOT]', 'Save PNG to Downloads folder'),
                        ('[AUDIO]', 'System audio via WASAPI loopback'),
                        ('[REC]', 'Record video to .webm file'),
                        ('[FULLSCREEN]', 'Toggle fullscreen mode'),
                    ]),
                    ('heading', 'Recording with Audio'),
                    ('para', 'To record video with audio: 1) Enable [AUDIO]. 2) Click [REC]. Order matters!'),
                ]
            },
            {
                'num': 5, 'title': 'File Manager',
                'content': [
                    ('heading', 'File System Access'),
                    ('para', 'Full access to the target file system. Works with all drives and network shares.'),
                    ('heading', 'Navigation'),
                    ('bullet', 'Click folder to open'),
                    ('bullet', '[Up] button or breadcrumbs to go up one level'),
                    ('bullet', 'Click filename to download'),
                    ('bullet', 'Large files: segmented transfer with progress bar'),
                    ('heading', 'Uploading Files to Target'),
                    ('bullet', 'Click "Upload" button'),
                    ('bullet', 'Or drag and drop files onto the panel'),
                    ('bullet', 'File is saved to current directory on target'),
                    ('heading', 'File Operations'),
                    ('kv', [
                        ('Download', 'Click filename - segmented download'),
                        ('Upload', 'Button or drag & drop'),
                        ('Delete', 'Delete button in context'),
                        ('New folder', 'New Folder button'),
                    ]),
                ]
            },
            {
                'num': 6, 'title': 'Processes & Services',
                'content': [
                    ('heading', 'Processes Tab'),
                    ('para', 'Shows all active processes with PID, name, CPU load and RAM usage. Refresh on demand.'),
                    ('kv', [
                        ('PID', 'Process identifier'),
                        ('Name', 'Executable name'),
                        ('CPU%', 'Processor load'),
                        ('RAM', 'Memory usage'),
                    ]),
                    ('heading', 'Process Actions'),
                    ('bullet', 'Select process -> click "Kill" to terminate'),
                    ('bullet', '"Run" field + Enter - launch program as SYSTEM'),
                    ('heading', 'Services Tab'),
                    ('kv', [
                        ('Start', 'Start a service'),
                        ('Stop', 'Stop a service'),
                        ('Restart', 'Restart a service'),
                        ('Startup type', 'Auto / Manual / Disabled'),
                    ]),
                ]
            },
            {
                'num': 7, 'title': 'Terminal',
                'content': [
                    ('heading', 'Description'),
                    ('para', 'Full command line access. Commands run as SYSTEM with maximum privileges (full administrator rights).'),
                    ('heading', 'Shell Modes'),
                    ('kv', [
                        ('cmd', 'Windows Command Prompt'),
                        ('PowerShell', 'Windows PowerShell'),
                    ]),
                    ('heading', 'Important Notes'),
                    ('bullet', 'Interactive commands (pause, choice) may hang'),
                    ('bullet', 'Use /Y, -Force, -Confirm:$false flags'),
                    ('bullet', 'cd command changes directory for current session'),
                    ('bullet', 'All operations run as SYSTEM'),
                    ('heading', 'Example Commands'),
                    ('kv', [
                        ('ipconfig /all', 'Network configuration'),
                        ('netstat -ano', 'Active connections'),
                        ('tasklist', 'Process list'),
                        ('sc query', 'Service status'),
                        ('reg query HKLM\\...', 'Registry query'),
                    ]),
                ]
            },
            {
                'num': 8, 'title': 'Registry',
                'content': [
                    ('heading', 'Registry Browser'),
                    ('para', 'Full access to the Windows registry with a tree browser. Navigation works like Windows Explorer.'),
                    ('heading', 'Registry Hives'),
                    ('kv', [
                        ('HKLM', 'HKEY_LOCAL_MACHINE - machine settings'),
                        ('HKCU', 'HKEY_CURRENT_USER - user settings'),
                        ('HKCR', 'HKEY_CLASSES_ROOT - file associations'),
                        ('HKU', 'HKEY_USERS - user profiles'),
                        ('HKCC', 'HKEY_CURRENT_CONFIG - current config'),
                    ]),
                    ('heading', 'Actions'),
                    ('bullet', 'Click key to show values on right panel'),
                    ('bullet', 'Right-click value -> "Edit" to modify'),
                    ('bullet', 'Right-click -> "Delete" - IRREVERSIBLE!'),
                    ('heading', '[WARN] Warning'),
                    ('para', 'Deleting registry keys is IRREVERSIBLE. Incorrect changes can break the operating system. Always create a backup before making changes.'),
                ]
            },
            {
                'num': 9, 'title': 'Screenshots',
                'content': [
                    ('heading', 'Automatic Screenshot Capture'),
                    ('para', 'Agent captures screenshots automatically. Default interval: every 10 seconds. Stored on VPS.'),
                    ('heading', 'Gallery'),
                    ('bullet', 'Thumbnails displayed in main area'),
                    ('bullet', 'Click thumbnail -> inline preview on right'),
                    ('bullet', 'Click preview image -> fullscreen view'),
                    ('bullet', 'Select -> Delete: preview auto-clears when deleting current image'),
                    ('heading', 'Settings'),
                    ('kv', [
                        ('Interval', '10 seconds (configurable)'),
                        ('Quality', '75% (JPEG)'),
                        ('Scale', '50% (downscaled)'),
                        ('Mode', 'Always / App filter'),
                    ]),
                    ('heading', 'App Filter'),
                    ('para', 'Can be configured to capture only when a specific application is in the foreground (e.g., browser or Excel).'),
                ]
            },
            {
                'num': 10, 'title': 'Audio',
                'content': [
                    ('heading', 'Microphone Recording'),
                    ('para', 'Continuous microphone recording from target. 5-minute segments (OGG Opus format). Stored on VPS.'),
                    ('heading', 'Player'),
                    ('bullet', 'Click file to start playback'),
                    ('bullet', 'Deleting currently playing file auto-clears the player'),
                    ('heading', 'Technical Details'),
                    ('kv', [
                        ('Interface', 'WASAPI loopback from SYSTEM'),
                        ('Windows indicator', 'Does not appear'),
                        ('Format', 'OGG Opus'),
                        ('Segment duration', '300 seconds (5 minutes)'),
                        ('Sample rate', '16000 Hz'),
                        ('Bitrate', '128 kbps'),
                        ('Gain', '100%'),
                    ]),
                    ('heading', 'DSP Settings'),
                    ('kv', [
                        ('Denoise', 'Enabled by default'),
                        ('Hum filter', '50 Hz'),
                    ]),
                ]
            },
            {
                'num': 11, 'title': 'Event Log',
                'content': [
                    ('heading', 'Windows Event Log Viewer'),
                    ('para', 'Viewer for the Windows Event Log. Used to diagnose crashes and problems on the target machine.'),
                    ('heading', 'Sources'),
                    ('kv', [
                        ('Application', 'Application events'),
                        ('System', 'System events'),
                        ('Security', 'Security events'),
                    ]),
                    ('heading', 'Filters'),
                    ('kv', [
                        ('Error', 'Critical errors'),
                        ('Warning', 'Warning messages'),
                        ('Information', 'Informational events'),
                    ]),
                    ('heading', 'Usage'),
                    ('bullet', 'Click entry -> details shown below'),
                    ('bullet', 'Filter by source and level'),
                    ('bullet', 'Refresh on demand'),
                ]
            },
            {
                'num': 12, 'title': 'Defense',
                'content': [
                    ('heading', 'Windows Defender Management'),
                    ('para', 'The Defense tab provides access to Windows Defender controls and event log settings.'),
                    ('heading', 'Defender Operations'),
                    ('kv', [
                        ('Enable/Disable', 'Toggle real-time protection'),
                        ('Run scan', 'Full or quick threat scan'),
                        ('Quarantine', 'View and manage quarantined items'),
                        ('Scan results', 'Last scan + detected threats'),
                    ]),
                    ('heading', 'Event Log Cleanup Settings'),
                    ('para', 'Configure automatic Windows Event Log cleanup. Removes noisy install-time entries to keep the log clean for diagnostics.'),
                    ('heading', 'Recommendation'),
                    ('para', 'Defender operations require care. Disabling protection should be temporary and with full understanding of risks.'),
                ]
            },
            {
                'num': 13, 'title': 'Settings',
                'content': [
                    ('heading', 'Settings Tabs'),
                    ('kv', [
                        ('Update', 'Upload and update pnpext.dll'),
                        ('Deploy', 'VPS address and room token'),
                        ('Config', 'All agent parameters'),
                        ('Threats', 'Security monitoring'),
                    ]),
                    ('heading', 'Agent Config (Config tab)'),
                    ('kv', [
                        ('Stream', 'FPS, quality, codec, scale'),
                        ('Screenshots', 'Interval, quality, mode'),
                        ('Audio', 'Segment length, sample rate, bitrate, DSP'),
                        ('STUN/TURN', 'Server addresses for WebRTC'),
                        ('Log cleanup', 'Auto Event Log cleanup'),
                    ]),
                    ('heading', 'Deploy Tab'),
                    ('para', 'VPS address: wss://[IP]/ws  |  Room token: unique session identifier. Change the token when replacing the agent.'),
                ]
            },
            {
                'num': 14, 'title': 'Remote Update',
                'content': [
                    ('heading', 'Remote Agent Update'),
                    ('para', 'The agent updates remotely without physical access to the target. Process takes 15-30 seconds.'),
                    ('heading', 'Update Steps'),
                    ('bullet', '1. Settings -> Update -> Select file (pnpext.dll)'),
                    ('bullet', '2. Click "Upload to VPS"'),
                    ('bullet', '3. Click "Update Agent"'),
                    ('bullet', '4. Agent downloads the file and replaces old one'),
                    ('bullet', '5. Service restarts automatically'),
                    ('bullet', '6. Connection briefly drops (~5 seconds)'),
                    ('bullet', '7. Status "Online" = update complete'),
                    ('heading', 'Parameters'),
                    ('kv', [
                        ('Update time', '15-30 seconds'),
                        ('Connection drop', '~5 seconds'),
                        ('Result', 'Online status'),
                        ('Rollback', 'Upload previous version DLL'),
                    ]),
                ]
            },
            {
                'num': 15, 'title': 'Users',
                'content': [
                    ('heading', 'User Roles'),
                    ('kv', [
                        ('Administrator', 'All tabs + user management'),
                        ('Operator', 'Permitted tabs only'),
                    ]),
                    ('heading', 'Adding a User'),
                    ('bullet', 'Users tab (admin only)'),
                    ('bullet', 'Click "Add"'),
                    ('bullet', 'Fill in: login / password / role / tabs'),
                    ('bullet', 'Click "Save"'),
                    ('heading', 'Activity Log'),
                    ('para', 'Each operator sees their @username in the activity log. All actions are logged automatically.'),
                    ('heading', 'UI Theme'),
                    ('para', 'Each user can select their own color theme (the [THEME] button in the panel header).'),
                ]
            },
            {
                'num': 16, 'title': 'Troubleshooting',
                'content': [
                    ('heading', 'Common Issues and Solutions'),
                    ('trouble', [
                        ('Status OFFLINE', 'Target powered off / no internet', 'Check power and internet connection on target'),
                        ('Stream will not start', 'Agent not running', 'Check MspIscSvc service in services.msc'),
                        ('Black screen', 'Screen locked / no permission', 'Unlock session or restart agent service'),
                        ('File download fails', 'Insufficient rights / buffer', 'Use terminal to copy file manually'),
                        ('No audio', 'WASAPI unavailable', 'Check Windows audio devices on target'),
                        ('Recordings not saving', 'VPS disk full', 'Clean old files in /opt/remotedesk/'),
                        ('Update hangs', 'No rights to replace DLL', 'Stop service manually via terminal first'),
                        ('Login not working', 'Wrong password / cookies', 'Clear browser cookies, retry login'),
                    ]),
                ]
            },
        ]
    },

    'AZ': {
        'filename': 'Guide_AZ.pdf',
        'cover_title': 'Operator Telimat',
        'cover_subtitle': 'Uzaqdan Idareetme ve Monitorinq Sistemi',
        'toc_title': 'Munden',
        'note_label': 'VAZHIB:',
        'note_text1': 'Butun texniki emeliyyatlar SYSTEM adından icra edilir.',
        'note_text2': 'Cihaz sahibinin ve ya IT adminin icesaze vermesi teleb olunur.',
        'total_pages': 18,
        'trouble_headers': ['Problem', 'Sebeb', 'Hell'],
        'sections': [
            {
                'num': 1, 'title': 'Agent Qurashdirmasi',
                'content': [
                    ('heading', 'Tesvir'),
                    ('para', 'Agent obyekt kompyuterine bir defe qurashdirilar, her Windows bashlangicinda MspIscSvc servisi kimi avtomatik ishe dusher.'),
                    ('heading', 'Teleb olunanlar'),
                    ('kv', [
                        ('pnpext.dll', 'Agent (esas modul)'),
                        ('pnpext.sys', 'AES-256 shifreli konfiq'),
                        ('install.bat', 'Qurashdirici (admin kimi ishe sal)'),
                        ('uninstall.bat', 'Agentin silinmesi'),
                    ]),
                    ('heading', 'Usul 1 - USB (Tovsiye olunur)'),
                    ('bullet', 'dist/usb qovlugunu flashe kopyalayin'),
                    ('bullet', 'Obyekt kompyuterine taxin'),
                    ('bullet', 'install.bat uzerende sag klik -> Administrator olaraq ishe sal'),
                    ('bullet', 'UAC-i tesdigleyin'),
                    ('bullet', 'Pencere oertuldu = qurashdirma tamamlandi'),
                    ('heading', 'Usul 2 - Brauzer'),
                    ('bullet', 'Obyektde brauzer achin'),
                    ('bullet', 'Brauzerden VPS-e kechin'),
                    ('bullet', 'Operatorun etibarlari ile daxil olun'),
                    ('bullet', '"Qurashdirici yukhle" duymesin basin'),
                    ('bullet', 'install-web.bat-i administrator olaraq ishe salin'),
                    ('heading', 'Silme'),
                    ('para', 'uninstall.bat-i Administrator olaraq ishe salin. Servis dayanacaq ve temizce silinecek.'),
                    ('heading', 'Windows Servisi Melumat'),
                    ('kv', [
                        ('Servis adi', 'MspIscSvc'),
                        ('Giris noeqtesi', 'PnpServiceEntry'),
                        ('Bashlangic noevu', 'Avtomatik'),
                        ('Kontekst', 'LocalSystem (SYSTEM)'),
                    ]),
                ]
            },
            {
                'num': 2, 'title': 'Panele Giris',
                'content': [
                    ('heading', 'Operator panelinе giris'),
                    ('para', 'Operator paneli adi veb-sehifedir. Heч bir proqram qurashdirmaq lazim deyil. Chrome, Edge, Firefox-da isheleyir.'),
                    ('heading', 'Giris Addimlar'),
                    ('bullet', 'Chrome ve ya Edge brauzer achin'),
                    ('bullet', 'Unvana kechin: https://[VPS IP unvani]'),
                    ('bullet', 'Brauzer sertifikat xeberdarlighi goesterecedek'),
                    ('bullet', 'Etrafli -> Sayta kechin (oezimzali sertifikat - bu normaldur)'),
                    ('bullet', 'Istifadeci adi ve parol daxil edin'),
                    ('bullet', '"Daxil ol" duymesin basin'),
                    ('heading', 'Elaqa Statusu'),
                    ('kv', [
                        ('[ONLINE] yashil', 'Agent qoshulub ve isheleyir'),
                        ('[OFFLINE] qirmizi', 'Agent oflayndir (emrler noevbeye alinir)'),
                        ('[RECONNECT]', 'Yeniden qoshulur...'),
                    ]),
                    ('heading', 'Sertifikat Qeyd'),
                    ('para', 'Oezimzali TLS sertifikati istifade olunur - bu xususi infrastruktur uchun standart tecrubedir. Brauzer xeberdarlighi gozlenilen davranishdur, xeta deyil.'),
                ]
            },
            {
                'num': 3, 'title': 'Monitorinq Levhesi',
                'content': [
                    ('heading', 'Icmal'),
                    ('para', 'Levhe tabi obyekt mashinin vəziyyetini real vaxtda goesterir. Butun goestericiler WebSocket vasitesile avtomatik yenilenir.'),
                    ('heading', 'Sistem Goestericileri'),
                    ('kv', [
                        ('CPU %', 'Real vaxtda prosessor yuku'),
                        ('RAM', 'Yaddash istifadesi (MB / %)'),
                        ('GPU', 'GPU yuku (mövcudsa)'),
                        ('Disk', 'Disk aktivliyi'),
                    ]),
                    ('heading', 'VPS1 ve VPS2 Kartlari'),
                    ('para', 'VPS1 ve VPS2 kartlari oetUrme serverinin vəziyyetini mustefil olaraq goesterir - her birinin oez [Yenile] duymesi var. Elaqa problemlerini diaqnostika etmek uchun istifade edin.'),
                    ('heading', 'Sureet Testi'),
                    ('para', 'VPS ile elaqa suereetini oelchuр. Obyekte kanalin keyfiyyetini qiymetlendirmek uchun faydalidir.'),
                    ('heading', 'Aktivlik Jurnali'),
                    ('para', 'Butun operator emeliyyatlarini @istifadeci adi ve vaxt damghasi ile qeyd edir. VPS-de saxlanilir. Operatorlar terefinden redakte oluna bilmez.'),
                ]
            },
            {
                'num': 4, 'title': 'Ekran (Strim)',
                'content': [
                    ('heading', 'Yayimi Bashlatmaq'),
                    ('para', 'Ekran tabi. "Bashla" duymesin basin - yayim bashlayin. Ses ve video ayrica oeturulur.'),
                    ('heading', 'Strim Parametrleri'),
                    ('kv', [
                        ('FPS (1-60)', '15=normal, 30=hamar, 60=maksimum'),
                        ('Keyfiyyet (1-100)', 'Standart: 75'),
                        ('Migas (%)', 'Obyekt terefinde shekil kichiltme'),
                        ('Kodek', 'MJPEG ve ya H.264'),
                    ]),
                    ('heading', 'Emeliyyat Duymeleri (uzhaen panel)'),
                    ('para', 'Uzhaen panel strim penceresinin sag alt koeшesindedir:'),
                    ('kv', [
                        ('[YERLESDIR/DOLDUR]', 'Goeruntuleme rejimini kecid edin'),
                        ('[EKRAN GOERUNTU]', 'PNG-ni Yukhlenmeler qovlughuna saxla'),
                        ('[SES]', 'WASAPI loopback vasitesile sistem sesi'),
                        ('[YAZ]', 'Videoyu .webm faylina yaz'),
                        ('[TAM EKRAN]', 'Tam ekran rejimini acib-baghla'),
                    ]),
                    ('heading', 'Sesle Yazi'),
                    ('para', 'Videoyu sesle yazmaq uchun: 1) [SES]-i aktivleshdirin. 2) [YAZ]-a basin. Sira muhimdir!'),
                ]
            },
            {
                'num': 5, 'title': 'Fayl Meneceri',
                'content': [
                    ('heading', 'Fayl Sistemine Giris'),
                    ('para', 'Obyektin fayl sistemine tam giris. Butun diskler ve shebeke resurslari ile isheleyir.'),
                    ('heading', 'Naviqasiya'),
                    ('bullet', 'Qovlugha klik - achmaq'),
                    ('bullet', '[Yukhari] duymesi ve ya corekdesim - bir seviyye yukhari'),
                    ('bullet', 'Fayl adina klik - yukhleme'),
                    ('bullet', 'Boyuk fayllar: ireleleyish chubughu ile seqmentli oeturme'),
                    ('heading', 'Obyekte Fayl Yukhleme'),
                    ('bullet', '"Yukkhle" duymesin basin'),
                    ('bullet', 'Ve ya fayllar surukleyin ve buraxin'),
                    ('bullet', 'Fayl obyektde cari qovlugha saxlanilir'),
                    ('heading', 'Fayl Emeliyyatlari'),
                    ('kv', [
                        ('Yukhhle', 'Fayl adina klik - seqmentli yukhleme'),
                        ('Yukle', 'Duyme ve ya surukle ve burax'),
                        ('Sil', 'Kontekstde Sil duymesi'),
                        ('Yeni qovluq', 'Yeni qovluq duymesi'),
                    ]),
                ]
            },
            {
                'num': 6, 'title': 'Prosesler ve Servisler',
                'content': [
                    ('heading', 'Prosesler Tabi'),
                    ('para', 'PID, ad, CPU yuku ve RAM istifadesi ile butun aktiv prosesleri goesterir. Teleb uzre yenileme.'),
                    ('kv', [
                        ('PID', 'Proses identifikatoru'),
                        ('Ad', 'Icra edilen fayl adi'),
                        ('CPU%', 'Prosessor yuku'),
                        ('RAM', 'Yaddash istifadesi'),
                    ]),
                    ('heading', 'Proses Emeliyyatlari'),
                    ('bullet', 'Proses sechin -> "Bitir" duymesin basin'),
                    ('"Bashla" sahesi + Enter - SYSTEM adina proqram bashlatin'),
                    ('heading', 'Servisler Tabi'),
                    ('kv', [
                        ('Bashla', 'Servisi bashla'),
                        ('Dayandır', 'Servisi dayandır'),
                        ('Yenidən Başlat', 'Servisi yeniden bashla'),
                        ('Bashlangic noevu', 'Avtomatik / El ile / Soendurulmushu'),
                    ]),
                ]
            },
            {
                'num': 7, 'title': 'Terminal',
                'content': [
                    ('heading', 'Tesvir'),
                    ('para', 'Tam emir setri girishi. Emrler maksimum imtiyazlarla SYSTEM adina icra edilir (tam administrator huquqlari).'),
                    ('heading', 'Kabuk Rejimleri'),
                    ('kv', [
                        ('cmd', 'Windows Emir Sori'),
                        ('PowerShell', 'Windows PowerShell'),
                    ]),
                    ('heading', 'Muhum Qeydler'),
                    ('bullet', 'Interaktiv emirler (pause, choice) donub qala biler'),
                    ('bullet', '/Y, -Force, -Confirm:$false bayraqlari istifade edin'),
                    ('bullet', 'cd emri cari sessiyadaki qovlugu deyishir'),
                    ('bullet', 'Butun emeliyyatlar SYSTEM olaraq icra edilir'),
                    ('heading', 'Numune Emirler'),
                    ('kv', [
                        ('ipconfig /all', 'Shebeke konfiqurasyasi'),
                        ('netstat -ano', 'Aktiv elaqeler'),
                        ('tasklist', 'Proses siyahisi'),
                        ('sc query', 'Servis vəziyyeti'),
                        ('reg query HKLM\\...', 'Reyestr sorğusu'),
                    ]),
                ]
            },
            {
                'num': 8, 'title': 'Reyestr',
                'content': [
                    ('heading', 'Reyestr Brauzeri'),
                    ('para', 'Ağac brauzeri ile Windows reyestrine tam giris. Naviqasiya Windows Explorer kimi isheleyir.'),
                    ('heading', 'Reyestr Hive-lari'),
                    ('kv', [
                        ('HKLM', 'HKEY_LOCAL_MACHINE - mashın parametrler'),
                        ('HKCU', 'HKEY_CURRENT_USER - istifadeci parametrler'),
                        ('HKCR', 'HKEY_CLASSES_ROOT - fayl assosiyasiyalari'),
                        ('HKU', 'HKEY_USERS - istifadeci profillar'),
                        ('HKCC', 'HKEY_CURRENT_CONFIG - cari konfiqrasiya'),
                    ]),
                    ('heading', 'Emeliyyatlar'),
                    ('bullet', 'Achara klik - sag panelde deyerleri goesterir'),
                    ('bullet', 'Deyere sag klik -> "Deyish" - redakte'),
                    ('bullet', 'Sag klik -> "Sil" - GERI ALINMAZ!'),
                    ('heading', '[WARN] Xeberdarlig'),
                    ('para', 'Reyestr acharlarini silmek GERI ALINMAZDIR. Yanlis deyishiklikler emeliyyat sistemini poza biler. Deyishiklik etmeden evvel her zaman ehtiyat nusxe yaradiniz.'),
                ]
            },
            {
                'num': 9, 'title': 'Ekran Goruntuler',
                'content': [
                    ('heading', 'Avtomatik Ekran Shekili Cekme'),
                    ('para', 'Agent avtomatik ekran goruntuler ceker. Standart aralig: her 10 saniyede bir. VPS-de saxlanilir.'),
                    ('heading', 'Qalereya'),
                    ('bullet', 'Esas sahede kichik shekillar goesterilir'),
                    ('bullet', 'Kichik shekile klik -> sağda onizleme'),
                    ('bullet', 'Onizleme shekline klik -> tam ekran goruntusu'),
                    ('bullet', 'Sec -> Sil: cari shekli silmek onizlemeni avtomatik temizleyir'),
                    ('heading', 'Parametrler'),
                    ('kv', [
                        ('Aralig', '10 saniye (konfiqurasiya edilir)'),
                        ('Keyfiyyet', '75% (JPEG)'),
                        ('Migas', '50% (kichildilib)'),
                        ('Rejim', 'Her zaman / Tetbiq filtri'),
                    ]),
                    ('heading', 'Tetbiq Filtri'),
                    ('para', 'Yalniz mueyyen bir tetbiq on plana geldikde goeruntuler chekmeleri uchun konfiqurasiya etmek mumkundur (meselon, brauzer ve ya Excel).'),
                ]
            },
            {
                'num': 10, 'title': 'Audio',
                'content': [
                    ('heading', 'Mikrofon Yazisi'),
                    ('para', 'Obyektin mikrofonunun davamlı yazisi. 5 deqiqelik seqmentler (OGG Opus formati). VPS-de saxlanilir.'),
                    ('heading', 'Pleyer'),
                    ('bullet', 'Faylin uzerne klik - oxumağı bashlayır'),
                    ('bullet', 'Oynayan faylı silmek pleyeri avtomatik temizleyir'),
                    ('heading', 'Texniki Detallar'),
                    ('kv', [
                        ('Interfeys', 'SYSTEM-den WASAPI loopback'),
                        ('Windows goestericisi', 'Goerunmur'),
                        ('Format', 'OGG Opus'),
                        ('Seqment muddeti', '300 saniye (5 deqiqe)'),
                        ('Numune tezliyi', '16000 Hz'),
                        ('Bitrate', '128 kbps'),
                        ('Guchlendirme', '100%'),
                    ]),
                    ('heading', 'DSP Parametrleri'),
                    ('kv', [
                        ('Ses-kuy azaltma', 'Standart olaraq aktiv'),
                        ('Hum filtri', '50 Hz'),
                    ]),
                ]
            },
            {
                'num': 11, 'title': 'Hadise Jurnali',
                'content': [
                    ('heading', 'Windows Hadise Jurnali Goruntleyicisi'),
                    ('para', 'Windows Hadise Jurnali uchun goruntleyici. Obyekt mashinindeki qezalari ve problemleri diaqnostika etmek uchun istifade olunur.'),
                    ('heading', 'Menbeler'),
                    ('kv', [
                        ('Application', 'Tetbiq hadiseler'),
                        ('System', 'Sistem hadiseler'),
                        ('Security', 'Tehlukesizlik hadiseler'),
                    ]),
                    ('heading', 'Filtrler'),
                    ('kv', [
                        ('Xeta', 'Kritik xetalar'),
                        ('Xeberdarlig', 'Xeberdarlig mesajları'),
                        ('Melumat', 'Melumat hadiseler'),
                    ]),
                    ('heading', 'Istifade'),
                    ('bullet', 'Qeydin uzerne klik -> asagida gosterilir'),
                    ('bullet', 'Menbeyе ve seviyyeye gore filtr'),
                    ('bullet', 'Teleb uzre yenileme'),
                ]
            },
            {
                'num': 12, 'title': 'Mudafie',
                'content': [
                    ('heading', 'Windows Defender Idaresi'),
                    ('para', 'Mudafie tabi Windows Defender kontrollarına ve hadise jurnali parametrlerine giris tamin edir.'),
                    ('heading', 'Defender Emeliyyatlari'),
                    ('kv', [
                        ('Aktiv/Soendur', 'Real vaxtli muhafizeni acib-baghla'),
                        ('Skan ishe sal', 'Tam ve ya sureetli tehdit skani'),
                        ('Karantin', 'Karantin elementlerini goru ve idar et'),
                        ('Skan neticeleri', 'Son skan + ashlanan tehdidler'),
                    ]),
                    ('heading', 'Hadise Jurnali Temizleme Parametrleri'),
                    ('para', 'Windows Hadise Jurnalinin avtomatik temizlenmesini konfiqurasiya et. Jurnali diaqnostika uchun temiz saxlamaq uchin qurtulma zamanindaki gurultulu qeydleri silir.'),
                    ('heading', 'Tovsiye'),
                    ('para', 'Defender emeliyyatlari diqqet telab edir. Muhafizeni soendurme gecici olmali ve riskleri tam anlayaraq ede bilmek lazimdir.'),
                ]
            },
            {
                'num': 13, 'title': 'Parametrler',
                'content': [
                    ('heading', 'Parametrler Tablari'),
                    ('kv', [
                        ('Yenileme', 'pnpext.dll-i yukle ve yenile'),
                        ('Deploy', 'VPS unvani ve otaq tokeni'),
                        ('Konfiq', 'Butun agent parametrleri'),
                        ('Tehdidler', 'Tehlukesizlik monitorinqi'),
                    ]),
                    ('heading', 'Agent Konfiqurasyasi (Konfiq tabi)'),
                    ('kv', [
                        ('Strim', 'FPS, keyfiyyet, kodek, migas'),
                        ('Ekran goruntuler', 'Aralig, keyfiyyet, rejim'),
                        ('Audio', 'Seqment uzunlughu, tezlik, bitrate, DSP'),
                        ('STUN/TURN', 'WebRTC uchun server unvanları'),
                        ('Jurnal temizleme', 'Avtomatik Hadise Jurnali temizleme'),
                    ]),
                    ('heading', 'Deploy Tabi'),
                    ('para', 'VPS unvani: wss://[IP]/ws  |  Otaq tokeni: unikal sessiyanin identifikatoru. Agenti evez edende tokeni deyishin.'),
                ]
            },
            {
                'num': 14, 'title': 'Uzaq Yenileme',
                'content': [
                    ('heading', 'Uzaqdan Agent Yenilemesi'),
                    ('para', 'Agent obyekte fiziki giris olmadan uzaqdan yenilenir. Proses 15-30 saniye edir.'),
                    ('heading', 'Yenileme Addimlar'),
                    ('bullet', '1. Parametrler -> Yenileme -> Fayl sec (pnpext.dll)'),
                    ('bullet', '2. "VPS-e yukle" duymesin basin'),
                    ('bullet', '3. "Agenti yenile" duymesin basin'),
                    ('bullet', '4. Agent faylı yukleyir ve eskini evez edir'),
                    ('bullet', '5. Servis avtomatik yeniden bashlanir'),
                    ('bullet', '6. Elaqa qisa muddetli kesilir (~5 saniye)'),
                    ('bullet', '7. "Online" statusu = yenileme tamamlandi'),
                    ('heading', 'Parametrler'),
                    ('kv', [
                        ('Yenileme vaxti', '15-30 saniye'),
                        ('Elaqa kesilmesi', '~5 saniye'),
                        ('Netice', 'Online statusu'),
                        ('Geri qayitma', 'Evvelki versiya DLL-i yukle'),
                    ]),
                ]
            },
            {
                'num': 15, 'title': 'Istifadeciler',
                'content': [
                    ('heading', 'Istifadeci Rollari'),
                    ('kv', [
                        ('Administrator', 'Butun tablar + istifadeci idaresi'),
                        ('Operator', 'Yalniz icesaze verilmish tablar'),
                    ]),
                    ('heading', 'Istifadeci Elave Etmek'),
                    ('bullet', 'Istifadeciler tabi (yalniz admin)'),
                    ('bullet', '"Elave et" duymesin basin'),
                    ('bullet', 'Doldurun: giris / parol / rol / tablar'),
                    ('bullet', '"Saxla" duymesin basin'),
                    ('heading', 'Aktivlik Jurnali'),
                    ('para', 'Her operator aktivlik jurnalinda oez @istifadeci adini goeruр. Butun emeliyyatlar avtomatik qeyd edilir.'),
                    ('heading', 'UI Temasi'),
                    ('para', 'Her istifadeci oz reng temasini seche biler (panel bashligindaki [TEMA] duymesi).'),
                ]
            },
            {
                'num': 16, 'title': 'Problemler',
                'content': [
                    ('heading', 'Umumei Problemler ve Helleri'),
                    ('trouble', [
                        ('Status OFFLINE', 'Obyekt soendurulub / internet yoxdur', 'Obyektde qidalanma ve internet elaqesini yoxlayin'),
                        ('Strim bashlamır', 'Agent ishlemiır', 'services.msc-de MspIscSvc servisini yoxlayin'),
                        ('Qara ekran', 'Ekran kilidlenmish / icaze yoxdur', 'Sessiyani kilid achin ve ya agent servisini yeniden bashlatin'),
                        ('Fayl yukhleme almir', 'Kifayet huquq yoxdur', 'Faylı elle kopyalamaq uchun terminal istifade edin'),
                        ('Ses yoxdur', 'WASAPI erishe biler deyil', 'Obiektde Windows ses qurgularini yoxlayin'),
                        ('Yazılar saxlanilmir', 'VPS diski doludur', '/opt/remotedesk/ de kohne faylları temizleyin'),
                        ('Yenileme asılır', 'DLL-i evez etmek uchun huquq yoxdur', 'Evvelen terminal vasitesile servisi dayandirin'),
                        ('Giris ishlemiır', 'Yanlis parol / kukilar', 'Brauzer kukileri temizleyin, girisinizi tekrarlayın'),
                    ]),
                ]
            },
        ]
    }
}


# ══════════════════════════════════════════════════════════════════════════
#  MAIN PDF BUILDER
# ══════════════════════════════════════════════════════════════════════════

def render_content_item(c, item_type, item_data, y, page_data, page_num_ref,
                        total_pages, lang_data):
    """
    Render a single content item. Returns new y.
    page_num_ref is a list [page_num] so we can mutate it.
    Handles page overflow.
    """

    def new_page():
        c.showPage()
        page_num_ref[0] += 1
        c.setPageSize(A4)
        c.setFillColor(C_BG)
        c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
        draw_header(c, page_data)
        draw_footer(c, page_num_ref[0], total_pages)
        return PAGE_H - MARGIN_T

    def check_y(y, needed=0.8 * cm):
        if y < MARGIN_B + needed:
            return new_page()
        return y

    if item_type == 'heading':
        y = check_y(y, 1.5 * cm)
        y -= 0.3 * cm
        c.setFillColor(C_ACCENT)
        c.setFont("Helvetica-Bold", 11)
        c.drawString(MARGIN_L, y, item_data)
        # underline
        c.setStrokeColor(C_ACCENT)
        c.setLineWidth(0.5)
        c.line(MARGIN_L, y - 2, MARGIN_L + CONTENT_W, y - 2)
        y -= 0.5 * cm

    elif item_type == 'para':
        y = check_y(y, 1.0 * cm)
        lines = simpleSplit(item_data, "Helvetica", 10, CONTENT_W)
        for line in lines:
            y = check_y(y, 0.5 * cm)
            c.setFillColor(C_TEXT_BODY)
            c.setFont("Helvetica", 10)
            c.drawString(MARGIN_L, y, line)
            y -= 13
        y -= 4

    elif item_type == 'bullet':
        y = check_y(y, 0.5 * cm)
        bullet_w = 15
        lines = simpleSplit(item_data, "Helvetica", 10, CONTENT_W - bullet_w)
        c.setFillColor(C_ACCENT)
        c.setFont("Helvetica-Bold", 12)
        c.drawString(MARGIN_L, y + 1, "-")
        c.setFillColor(C_TEXT_BODY)
        c.setFont("Helvetica", 10)
        for i, line in enumerate(lines):
            c.drawString(MARGIN_L + bullet_w, y, line)
            y -= 13
        y -= 3

    elif item_type == 'kv':
        rows = item_data
        col1_w = 5.5 * cm
        row_h = 0.52 * cm
        for idx, (key, val) in enumerate(rows):
            y = check_y(y, row_h + 2)
            bg = C_ROW_ALT if idx % 2 == 0 else white
            c.setFillColor(bg)
            c.rect(MARGIN_L, y - row_h, CONTENT_W, row_h, fill=1, stroke=0)
            c.setStrokeColor(C_BORDER)
            c.setLineWidth(0.3)
            c.rect(MARGIN_L, y - row_h, CONTENT_W, row_h, fill=0, stroke=1)
            # key
            c.setFillColor(C_HEADING)
            c.setFont("Helvetica-Bold", 9)
            c.drawString(MARGIN_L + 4, y - row_h + 0.14 * cm, key)
            # val - wrap
            val_lines = simpleSplit(str(val), "Helvetica", 9, CONTENT_W - col1_w - 8)
            c.setFillColor(C_TEXT_BODY)
            c.setFont("Helvetica", 9)
            c.drawString(MARGIN_L + col1_w + 4, y - row_h + 0.14 * cm,
                         val_lines[0] if val_lines else "")
            y -= row_h
        y -= 5

    elif item_type == 'trouble':
        rows = item_data
        headers = lang_data['trouble_headers']
        col_w = [CONTENT_W * 0.3, CONTENT_W * 0.3, CONTENT_W * 0.4]
        row_h_base = 0.55 * cm

        # Header row
        y = check_y(y, row_h_base + 4)
        c.setFillColor(C_HEADING)
        c.rect(MARGIN_L, y - row_h_base, CONTENT_W, row_h_base, fill=1, stroke=0)
        c.setFillColor(white)
        c.setFont("Helvetica-Bold", 9)
        cx = MARGIN_L
        for h_idx, h in enumerate(headers):
            c.drawString(cx + 4, y - row_h_base + 0.15 * cm, h)
            cx += col_w[h_idx]
        y -= row_h_base

        for idx, (prob, cause, sol) in enumerate(rows):
            prob_lines = simpleSplit(prob, "Helvetica", 8, col_w[0] - 8)
            cause_lines = simpleSplit(cause, "Helvetica", 8, col_w[1] - 8)
            sol_lines = simpleSplit(sol, "Helvetica", 8, col_w[2] - 8)
            max_lines = max(len(prob_lines), len(cause_lines), len(sol_lines))
            rh = max(row_h_base, max_lines * 11 + 6)

            y = check_y(y, rh + 4)
            bg = C_ROW_ALT if idx % 2 == 0 else white
            c.setFillColor(bg)
            c.rect(MARGIN_L, y - rh, CONTENT_W, rh, fill=1, stroke=0)
            c.setStrokeColor(C_BORDER)
            c.setLineWidth(0.3)
            c.rect(MARGIN_L, y - rh, CONTENT_W, rh, fill=0, stroke=1)
            # dividers
            cx2 = MARGIN_L + col_w[0]
            c.line(cx2, y - rh, cx2, y)
            cx2 += col_w[1]
            c.line(cx2, y - rh, cx2, y)
            # text
            for col_idx, lines_list in enumerate([prob_lines, cause_lines, sol_lines]):
                tx = MARGIN_L + sum(col_w[:col_idx]) + 4
                ty = y - 8
                c.setFillColor(C_TEXT_BODY)
                c.setFont("Helvetica", 8)
                for line in lines_list:
                    c.drawString(tx, ty, line)
                    ty -= 11
            y -= rh
        y -= 5

    return y


def build_pdf(lang_key, output_path):
    lang_data = LANGUAGES[lang_key]
    total_pages = lang_data['total_pages']

    c = canvas.Canvas(output_path, pagesize=A4)
    c.setTitle(f"PROMETEY v1.0.250 - {lang_data['cover_title']}")
    c.setAuthor("PROMETEY System")
    c.setSubject(lang_data['cover_subtitle'])

    # ── Page 1: Cover ──────────────────────────────────────────────────────
    c.setPageSize(A4)
    c.setFillColor(C_BG)
    c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
    draw_cover(c, lang_data)
    c.showPage()

    # ── Page 2: Table of Contents ──────────────────────────────────────────
    c.setPageSize(A4)
    c.setFillColor(C_BG)
    c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
    # Build page map: section N starts at page N+2
    page_map = {sec['num']: sec['num'] + 2 for sec in lang_data['sections']}
    draw_toc(c, lang_data, page_map)
    c.showPage()

    # ── Pages 3+: Sections ─────────────────────────────────────────────────
    page_num = 2
    for sec in lang_data['sections']:
        page_num += 1
        c.setPageSize(A4)
        c.setFillColor(C_BG)
        c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)

        page_data = {'section': sec['title']}
        draw_header(c, page_data)
        draw_footer(c, page_num, total_pages)

        # Section heading
        y = PAGE_H - MARGIN_T
        y = draw_section_heading(c, sec['num'], sec['title'], y)

        page_num_ref = [page_num]

        for item in sec['content']:
            # Handle tuple items of various arities
            if len(item) == 2:
                itype, idata = item
            else:
                # shouldn't happen but be safe
                itype = item[0]
                idata = item[1] if len(item) > 1 else ''

            # Check if we need a new page before rendering
            if y < MARGIN_B + 1.5 * cm:
                c.showPage()
                page_num_ref[0] += 1
                page_num = page_num_ref[0]
                c.setPageSize(A4)
                c.setFillColor(C_BG)
                c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
                draw_header(c, page_data)
                draw_footer(c, page_num_ref[0], total_pages)
                y = PAGE_H - MARGIN_T

            y = render_content_item(c, itype, idata, y, page_data,
                                    page_num_ref, total_pages, lang_data)
            page_num = page_num_ref[0]

        c.showPage()

    c.save()
    return output_path


# ══════════════════════════════════════════════════════════════════════════
#  ENTRY POINT
# ══════════════════════════════════════════════════════════════════════════

if __name__ == '__main__':
    import os
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)))
    os.makedirs(out_dir, exist_ok=True)

    files = {
        'RU': os.path.join(out_dir, 'Guide_RU.pdf'),
        'EN': os.path.join(out_dir, 'Guide_EN.pdf'),
        'AZ': os.path.join(out_dir, 'Guide_AZ.pdf'),
    }

    for lang_key, path in files.items():
        print(f"Generating {lang_key}...", end=" ", flush=True)
        build_pdf(lang_key, path)
        size_kb = os.path.getsize(path) // 1024
        print(f"OK -> {path} ({size_kb} KB)")

    print("\nAll PDFs generated successfully.")
