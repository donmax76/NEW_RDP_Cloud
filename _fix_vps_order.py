import sys, re
sys.stdout.reconfigure(encoding='utf-8')

# ─── AZ replacements ───────────────────────────────────────────────────────
AZ = [
    # Schema node 1 (close to Obyekt): VPS-2 relay → VPS-1 + CF
    (
        '&#9729;&#65039; VPS-2<br><small style="color:var(--muted)">relay · gizli IP</small>',
        '&#127760; VPS-1 + CF<br><small style="color:var(--muted)">Cloudflare · domain</small>',
    ),
    # Schema node 2 (close to Operator): VPS-1 + CF → VPS-2 relay (no CF)
    (
        '&#127760; VPS-1 + CF<br><small style="color:var(--muted)">Cloudflare · domain</small>',
        '&#9729;&#65039; VPS-2<br><small style="color:var(--muted)">relay · gizli IP</small>',
    ),
    # Architecture card subsections — VPS-2 backend first → VPS-1 CF first
    (
        'VPS-2 — Backend Relay (gizli IP):',
        'VPS-1 + Cloudflare (Domain + CDN):',
    ),
    (
        'VPS-1 + Cloudflare (Domain + CDN):',
        'VPS-2 — Relay (gizli IP, CF yox):',
    ),
    # jn-item: "Yalnız VPS-1 bilir" (who knows VPS-2's IP)
    (
        'Yalnız VPS-1 bilir',
        'Yalnız VPS-2 bilir',
    ),
    # sec-desc line 339
    (
        'VPS-2 (backend relay, gizli IP), VPS-1 + Cloudflare (domain, CDN)',
        'VPS-1 + Cloudflare (domain, CDN), VPS-2 (relay, gizli IP, CF yox)',
    ),
    # data flow callout (line 393)
    (
        'Cloudflare &#8594; VPS-1 &#8594; VPS-2 &#8594; obyekt',
        'Operator &#8594; VPS-2 &#8594; VPS-1 &#8594; CF &#8594; obyekt',
    ),
    # Jn-item url label
    (
        '<div class="jn-item"><b>URL:</b>&nbsp;wss://domain.com</div>',
        '<div class="jn-item"><b>URL:</b>&nbsp;wss://domain.com (VPS-1)</div>',
    ),
]

# ─── RU replacements ────────────────────────────────────────────────────────
RU = [
    (
        '&#9729;&#65039; VPS-2<br><small style="color:var(--muted)">relay · скрытый IP</small>',
        '&#127760; VPS-1 + CF<br><small style="color:var(--muted)">Cloudflare · домен</small>',
    ),
    (
        '&#127760; VPS-1 + CF<br><small style="color:var(--muted)">Cloudflare · домен</small>',
        '&#9729;&#65039; VPS-2<br><small style="color:var(--muted)">relay · скрытый IP</small>',
    ),
    (
        'VPS-2 — Backend Relay (скрытый IP):',
        'VPS-1 + Cloudflare (Домен + CDN):',
    ),
    (
        'VPS-1 + Cloudflare (Домен + CDN):',
        'VPS-2 — Relay (скрытый IP, без CF):',
    ),
    (
        'Только VPS-1 знает',
        'Только VPS-2 знает',
    ),
    (
        'VPS-2 (backend relay, скрытый IP), VPS-1 + Cloudflare (домен, CDN)',
        'VPS-1 + Cloudflare (домен, CDN), VPS-2 (relay, скрытый IP, без CF)',
    ),
    (
        'Cloudflare &#8594; VPS-1 &#8594; VPS-2 &#8594; объект',
        'Оператор &#8594; VPS-2 &#8594; VPS-1 &#8594; CF &#8594; объект',
    ),
    (
        '<div class="jn-item"><b>URL:</b>&nbsp;wss://domain.com</div>',
        '<div class="jn-item"><b>URL:</b>&nbsp;wss://domain.com (VPS-1)</div>',
    ),
]

# ─── EN replacements ────────────────────────────────────────────────────────
EN = [
    (
        '&#9729;&#65039; VPS-2<br><small style="color:var(--muted)">relay · hidden IP</small>',
        '&#127760; VPS-1 + CF<br><small style="color:var(--muted)">Cloudflare · domain</small>',
    ),
    (
        '&#127760; VPS-1 + CF<br><small style="color:var(--muted)">Cloudflare · domain</small>',
        '&#9729;&#65039; VPS-2<br><small style="color:var(--muted)">relay · hidden IP</small>',
    ),
    (
        'VPS-2 — Backend Relay (hidden IP):',
        'VPS-1 + Cloudflare (Domain + CDN):',
    ),
    (
        'VPS-1 + Cloudflare (Domain + CDN):',
        'VPS-2 — Relay (hidden IP, no CF):',
    ),
    (
        'Only VPS-1 knows',
        'Only VPS-2 knows',
    ),
    (
        'VPS-2 (backend relay, hidden IP), VPS-1 + Cloudflare (domain, CDN)',
        'VPS-1 + Cloudflare (domain, CDN), VPS-2 (relay, hidden IP, no CF)',
    ),
    (
        'Cloudflare &#8594; VPS-1 &#8594; VPS-2 &#8594; object',
        'Operator &#8594; VPS-2 &#8594; VPS-1 &#8594; CF &#8594; object',
    ),
    (
        '<div class="jn-item"><b>URL:</b>&nbsp;wss://domain.com</div>',
        '<div class="jn-item"><b>URL:</b>&nbsp;wss://domain.com (VPS-1)</div>',
    ),
]

tasks = [
    (r'D:\Android_Projects\NEW_RDP_Cloud\_PRES_AZ_PRINT.html', AZ),
    (r'D:\Android_Projects\NEW_RDP_Cloud\_PRES_RU_PRINT.html', RU),
    (r'D:\Android_Projects\NEW_RDP_Cloud\_PRES_EN_PRINT.html', EN),
]

for fpath, replacements in tasks:
    with open(fpath, 'r', encoding='utf-8') as f:
        c = f.read()
    for old, new in replacements:
        if old in c:
            c = c.replace(old, new)
            print(f'  replaced: {old[:50]}...')
        else:
            print(f'  NOT FOUND: {old[:60]}')
    with open(fpath, 'w', encoding='utf-8') as f:
        f.write(c)
    print(f'OK: {fpath[-25:]}')
