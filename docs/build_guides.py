#!/usr/bin/env python3
"""Generate Prometey PDF guides in EN/RU/AZ with embedded screenshots."""
import os
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm, cm
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.colors import HexColor, white, black
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.platypus import (SimpleDocTemplate, Paragraph, Spacer, Image,
                                 PageBreak, Table, TableStyle, KeepTogether,
                                 HRFlowable)
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfbase.pdfmetrics import registerFontFamily
from PIL import Image as PILImage

pdfmetrics.registerFont(TTFont("Arial", "C:/Windows/Fonts/arial.ttf"))
pdfmetrics.registerFont(TTFont("Arial-Bold", "C:/Windows/Fonts/arialbd.ttf"))
pdfmetrics.registerFont(TTFont("Arial-Italic", "C:/Windows/Fonts/ariali.ttf"))
pdfmetrics.registerFont(TTFont("Arial-BoldItalic", "C:/Windows/Fonts/arialbi.ttf"))
registerFontFamily("Arial", normal="Arial", bold="Arial-Bold",
                   italic="Arial-Italic", boldItalic="Arial-BoldItalic")

IMG_DIR = os.path.join(os.path.dirname(__file__), "img")
OUT_DIR = os.path.dirname(__file__)
W, H = A4
MARGIN = 1.8 * cm
CONTENT_W = W - 2 * MARGIN

DARK = HexColor("#2E4057")
ACCENT = HexColor("#00A8CC")
MUTED = HexColor("#888888")
LINE_COLOR = HexColor("#CCCCCC")

# ── All translations ──
LANGS = {}

_screenshots_en = [
    ("01_login.png", "4.1 Login",
     "Open https://VPS_IP/ in Chrome or Edge. Enter the <b>Token</b> (room identifier) and <b>Password</b> in the fields. Click <b>Connect</b>. If the object is online, you'll see the Dashboard immediately. If offline, the client will poll and auto-connect when the object appears. The server address and port are auto-filled from the URL. Check <b>SSL</b> if using HTTPS (recommended)."),
    ("02_ssl_warning.png", "4.2 SSL Warning",
     "When using a self-signed certificate, the browser shows a security warning. Click <b>Advanced</b>, then <b>Proceed to site</b> (or 'Accept the Risk' in Firefox). This is expected for self-signed certs. To avoid this warning, install a Let's Encrypt certificate on the VPS."),
    ("03_connected.png", "4.3 Connected — Topbar",
     "After successful connection the topbar shows: <b>green dot</b> = connected to relay, <b>Host: ONLINE</b> = object is active. Version info on the right: Client / VPS / Host versions. The <b>orange badge</b> appears if other operators are connected to the same object. Buttons: <b>CONNECT/DISCONNECT</b>, <b>▶ STREAM</b> to start video, <b>⏺ REC</b> to record stream, language selector <b>EN/RU/AZ</b>."),
    ("04_dashboard.png", "4.4 Dashboard",
     "The main overview panel. <b>Metric cards</b>: RAM usage (%), CPU load (%), GPU usage (%), running processes count, active services count, stream FPS, system uptime, hostname, OS version, host version. Cards update every 3 seconds. Below: <b>Activity Log</b> — real-time event feed (connections, errors, actions). <b>Quick Actions</b> — buttons to jump to Screen, Files, Processes, Services, or open Task Manager/Explorer on the object."),
    ("05_speed_test.png", "4.5 Speed Test",
     "Click <b>▶ Run All Tests</b> to measure connection quality. Four cards: <b>Browser ↔ Relay</b> (your link to VPS: ping, download, upload), <b>Browser ↔ Host</b> (full round-trip through relay: RTT, DL, UL), <b>Host ↔ Relay</b> (object's link: ping, DL from VPS), <b>Host → Internet</b> (object's download/upload from Cloudflare). Color coding: <b>green</b> = good, <b>yellow</b> = acceptable, <b>orange</b> = slow, <b>red</b> = poor. Last run time shown."),
    ("06_screen.png", "4.6 Screen — Remote Desktop",
     "Click <b>▶ STREAM</b> in the topbar to start live video. Two transport modes: <b>H.264 via WebRTC</b> (UDP, lowest latency, requires STUN/TURN) or <b>JPEG via WebSocket</b> (TCP, works through any firewall, higher latency). Configure in Settings → WebRTC ICE Servers. Controls: <b>quality/FPS/scale/bitrate</b> sliders in the stream bar, <b>Fit/Fill</b> toggle, <b>fullscreen</b> button. Click <b>⏺ REC</b> to record the stream locally."),
    ("07_files.png", "4.7 Files — File Manager",
     "Left panel: <b>drive tree</b> with quick links (C:\\, D:\\, Users, Programs, Desktop). Click to navigate. Right panel: <b>file list</b> with Name, Size, Modified, Attributes columns — click column headers to sort. <b>Actions</b>: select files/folders with checkboxes → <b>Download</b> (multi-file download as individual files), <b>Upload</b> (drag-and-drop or button), <b>New Folder</b>, <b>Rename</b>, <b>Delete</b>. Transfer progress shows speed, ETA, and percentage. Maximum transfer speed: ~2 MB/s via TLS."),
    ("08_processes.png", "4.8 Processes",
     "Table columns: <b>PID, Name, CPU%, Memory, Threads, Actions</b>. Click any column header to sort (arrow shows direction). CPU% is color-coded: green <5%, yellow <20%, orange <50%, red ≥50%. <b>Filter</b>: type in the search box to filter by process name. <b>Kill</b>: click Kill button or select multiple with checkboxes → Kill Selected. <b>Launch</b>: enter executable path and arguments, choose elevation (Normal / Admin / System), click Launch. First Refresh shows 0% CPU (no baseline), second Refresh shows real values."),
    ("09_terminal.png", "4.9 Terminal — Remote CMD",
     "A full remote command prompt. Type any command and press Enter. Output displays in real-time. The working directory persists between commands. Examples: <b>ipconfig</b> (network info), <b>systeminfo</b> (OS details), <b>dir C:\\</b> (list files), <b>tasklist</b> (processes), <b>netstat -ano</b> (connections), <b>whoami</b> (current user). Use <b>Clear</b> button to reset output. Note: interactive commands (like 'pause' or editors) are not supported — use non-interactive alternatives."),
    ("10_audio.png", "4.10 Audio Recording",
     "Three modes: <b>Record only</b> (captures to file, sends to VPS), <b>Live listen</b> (real-time streaming to your browser), <b>Both</b> (simultaneous). Controls: <b>Gain</b> slider (50–2000%), <b>Denoise</b> (high-pass filter + noise gate), <b>Normalize</b> (peak normalization for consistent volume), <b>Hum filter</b> (50Hz/60Hz power line notch). <b>Client-side</b> section: HP filter and Hum filter applied in the browser during playback (for already recorded files). <b>Waveform player</b>: click to seek, scroll to zoom horizontally, Shift+scroll for vertical zoom. Zoom state saved per track. <b>EQ</b>: 7-band equalizer with Voice/Bass/Treble presets."),
    ("11_screenshots.png", "4.11 Screenshots",
     "Configure auto-capture: <b>Interval</b> (seconds between captures), <b>Quality</b> (JPEG 1–100), <b>Scale</b> (10–100%). <b>Always</b> checkbox: capture regardless of active app. When off, only captures when the foreground window title matches <b>Apps</b> filter (comma-separated keywords). View modes: <b>Grid</b> (thumbnails) or <b>List</b> (detailed). Click a thumbnail to preview full-size. Select screenshots with checkboxes → <b>Download</b> or <b>Delete</b>. Files stored encrypted on VPS with per-room quota."),
    ("12_eventlog.png", "4.12 Event Log",
     "Browse Windows Event Logs: <b>System, Application, Security, Setup</b>. Filter by <b>level</b> (Error/Warning/Information), <b>text search</b>, <b>date range</b> (quick buttons: 1h, 24h, 7d). Click an entry to expand details (message, source, event ID). <b>Auto-clean</b> toolbar: <b>Once</b> (clean at object startup), <b>Periodic</b> (every N seconds), <b>Off</b>. <b>Patterns</b> field: comma-separated keywords to match and remove (e.g., 'pnpext,WPnpSvc'). <b>Delete Selected</b> / <b>Clear All</b> for manual cleanup."),
    ("13_services.png", "4.13 Services",
     "Full list of Windows services with <b>Name, Display Name, Status, Start Type</b>. Filter by name in the search box. Actions per service: <b>Start, Stop, Restart</b>. Status shown as color badges: green = Running, red = Stopped. Useful for managing remote services without RDP access."),
    ("14_programs.png", "4.14 Installed Programs",
     "Lists all installed software from Windows registry (both 64-bit and 32-bit). Columns: <b>Name, Version, Publisher, Install Date, Size</b>. Click column headers to sort. Search box filters by name or publisher. <b>Export</b> button saves the full list as CSV file for reporting."),
    ("15_registry.png", "4.15 Registry Editor",
     "Browse the Windows Registry remotely. Root keys: <b>HKLM, HKCU, HKCR, HKU, HKCC</b>. Left panel: key tree — click to expand. Right panel: values with Name, Type, Data. <b>Supported types</b>: REG_SZ (string), REG_DWORD (32-bit number), REG_QWORD (64-bit), REG_BINARY, REG_EXPAND_SZ, REG_MULTI_SZ. Actions: <b>+ Key</b> (create subkey), <b>+ Value</b> (create value), <b>Edit</b> (double-click value), <b>Delete</b>."),
    ("16_settings_update.png", "4.16 Settings — Remote Update",
     "Update the object's DLL remotely without physical access. Enter the <b>DLL URL</b> on VPS (e.g., https://VPS_IP/files/pnpext.dll). Click <b>Update Host</b>. Progress bar shows 7 steps: Download → Disable Defender → Stop service → Replace DLL → Start service → Enable Defender → Done. The object stays online during download; disconnects only ~5 seconds during replacement. <b>↻ Restart</b> button: quick service restart without DLL replacement. Current version and build date shown above."),
    ("17_settings_deploy.png", "4.17 Settings — VPS Deploy",
     "Upload updated files to VPS directly from the browser. Three file slots: <b>index.html</b> (web client → /var/www/remote-desktop/), <b>server.py</b> (relay → /opt/remotedesk/), <b>pnpext.dll</b> (DLL → /files/ for remote update). Individual <b>Upload</b> buttons or green <b>⬆ Upload All 3</b> for one-click deploy. After upload: <b>Restart server.py</b> button restarts the relay service; page reloads automatically to pick up new client code."),
    ("18_settings_config.png", "4.18 Settings — Config Editor",
     "Edit the object's configuration without reinstallation. <b>Load from host</b>: reads current pnpext.sys (decrypted JSON). <b>Save to host</b>: encrypts and writes back, applies immediately (hot-reload). <b>Format JSON</b>: prettifies the text. <b>Local operations</b>: <b>📄 New</b> creates a blank template with all fields, <b>📂 Open .json</b> loads a local file, <b>💾 Save .json</b> downloads to your computer. Use local operations to prepare configs for new objects offline."),
    ("19_settings_threat.png", "4.19 Settings — Threat Monitor",
     "Monitors the object for security/analysis tools. Detected applications: Task Manager, Resource Monitor, Process Explorer/Hacker, Wireshark, Fiddler, TCPView, x64dbg, OllyDbg, IDA, WinDbg, Windows Settings. <b>Scan processes</b> checkbox: enable/disable scanning (every 5 seconds). <b>Auto-pause on detection</b>: when a threat is detected, the object pauses video/audio streaming until the threat clears. Status shown in the Settings panel and as an orange warning banner in the topbar."),
    ("20_lang_switch.png", "4.20 Language Selector",
     "Dropdown in the top-right corner of the topbar. Three languages: <b>English (EN), Russian (RU), Azerbaijani (AZ)</b>. Switching is instant — all panel titles, buttons, labels, tooltips, log messages update immediately without page reload. Selection saved in browser localStorage — persists across sessions."),
    ("21_terminate.png", "4.21 Settings — Self-Destruct",
     "Emergency cleanup: irreversibly removes all traces from the object. <b>💣 Self-Destruct Host</b> button (requires confirmation). 8-step process: Stop streaming → Collect file paths → Delete config (pnpext.sys) → Delete logs → Clear Windows Event Logs (Application/System/Setup) → Generate cleanup script → Disconnect → Done. After completion, the object deletes its own DLL and service registration. Cannot be undone."),
]

_screenshots_ru = [
    ("01_login.png", "4.1 Вход",
     "Откройте https://VPS_IP/ в Chrome или Edge. Введите <b>Токен</b> (идентификатор комнаты) и <b>Пароль</b>. Нажмите <b>Подключить</b>. Если объект онлайн — откроется Dashboard. Если офлайн — клиент будет ждать и подключится автоматически при появлении объекта. Поставьте галочку <b>SSL</b> при использовании HTTPS."),
    ("02_ssl_warning.png", "4.2 Предупреждение SSL",
     "При самоподписанном сертификате браузер показывает предупреждение. Нажмите <b>Дополнительно</b> → <b>Перейти на сайт</b>. Это нормально для самоподписанных сертификатов. Для устранения предупреждения — установите Let's Encrypt на VPS."),
    ("03_connected.png", "4.3 Подключено — Топбар",
     "После подключения: <b>зелёная точка</b> = связь с relay, <b>Host: ONLINE</b> = объект активен. Справа — версии: Client / VPS / Host. <b>Оранжевый бейдж</b> появляется если другие операторы подключены к этому же объекту. Кнопки: ПОДКЛЮЧИТЬ/ОТКЛЮЧИТЬ, ▶ ПОТОК, ⏺ ЗАПИСЬ, селектор языка EN/RU/AZ."),
    ("04_dashboard.png", "4.4 Панель управления",
     "Обзорная панель. <b>Карточки метрик</b>: RAM (%), CPU (%), GPU (%), процессы, службы, FPS потока, uptime, hostname, версия ОС и хоста. Обновление каждые 3 сек. <b>Журнал активности</b> — лента событий в реальном времени. <b>Быстрые действия</b> — кнопки для перехода к Экрану, Файлам, Процессам, Службам, открытие Task Manager/Explorer на объекте."),
    ("05_speed_test.png", "4.5 Тест скорости",
     "Нажмите <b>▶ Запустить все тесты</b>. Четыре карточки: <b>Браузер ↔ Релей</b> (ваш канал до VPS: пинг, скачивание, загрузка), <b>Браузер ↔ Объект</b> (полный RTT через relay), <b>Объект ↔ Релей</b> (канал объекта до VPS), <b>Объект → Интернет</b> (скорость интернета объекта через Cloudflare). Цвета: <b>зелёный</b> = хорошо, <b>жёлтый</b> = нормально, <b>оранжевый</b> = медленно, <b>красный</b> = плохо."),
    ("06_screen.png", "4.6 Экран — Удалённый рабочий стол",
     "Нажмите <b>▶ ПОТОК</b> в топбаре. Два режима транспорта: <b>H.264 через WebRTC</b> (UDP, минимальная задержка, требует STUN/TURN) или <b>JPEG через WebSocket</b> (TCP, работает через любой firewall, выше задержка). Настройка в Настройки → WebRTC ICE Серверы. Управление: слайдеры качество/FPS/масштаб/битрейт, переключатель Вместить/Заполнить, полноэкранный режим."),
    ("07_files.png", "4.7 Файлы — Файловый менеджер",
     "Левая панель: <b>дерево дисков</b> с быстрыми ссылками (C:\\, D:\\, Пользователи, Программы). Правая: <b>список файлов</b> с колонками Имя, Размер, Изменён — клик по заголовку для сортировки. <b>Действия</b>: выберите чекбоксами → Скачать, Загрузить (drag-and-drop или кнопка), Новая папка, Переименовать, Удалить. Прогресс-бар показывает скорость, ETA, процент. Скорость: ~2 МБ/с через TLS."),
    ("08_processes.png", "4.8 Процессы",
     "Колонки: <b>PID, Имя, CPU%, Память, Потоки, Действия</b>. Клик по заголовку — сортировка (стрелка показывает направление). CPU% с цветом: зелёный <5%, жёлтый <20%, оранжевый <50%, красный ≥50%. <b>Фильтр</b>: поиск по имени. <b>Завершить</b>: кнопка Kill или выбрать несколько → Kill Selected. <b>Запуск</b>: путь + аргументы, уровень (Обычный / Админ / Система). При первом Refresh CPU% = 0 (нет базы), при втором — реальные значения."),
    ("09_terminal.png", "4.9 Терминал — Удалённый CMD",
     "Полноценная командная строка. Введите команду, нажмите Enter — результат в реальном времени. Рабочая директория сохраняется. Примеры: <b>ipconfig</b> (сеть), <b>systeminfo</b> (система), <b>dir C:\\</b> (файлы), <b>tasklist</b> (процессы), <b>netstat -ano</b> (соединения), <b>whoami</b> (пользователь). Кнопка <b>Очистить</b> для сброса. Интерактивные команды (pause, edit) не поддерживаются."),
    ("10_audio.png", "4.10 Аудио — Запись и прослушка",
     "Три режима: <b>Только запись</b> (файл на VPS), <b>Прослушка</b> (стрим в браузер), <b>Оба</b> (одновременно). Настройки: <b>Усиление</b> (50–2000%), <b>Шумоподавление</b> (high-pass + noise gate), <b>Нормализация</b> (выравнивание громкости), <b>Фильтр гула</b> (50/60 Гц для наводки от питания). Секция <b>КЛИЕНТ</b>: фильтры в браузере при воспроизведении записей. <b>Плеер</b>: клик для перемотки, колёсико для zoom, Shift+колёсико для вертикального zoom. 7-полосный <b>эквалайзер</b> с пресетами Voice/Bass/Treble."),
    ("11_screenshots.png", "4.11 Скриншоты",
     "Настройка авто-захвата: <b>Интервал</b> (секунды), <b>Качество</b> (JPEG 1–100), <b>Масштаб</b> (10–100%). <b>Всегда</b>: захват независимо от приложения. Когда выключен — только если заголовок окна содержит ключевые слова из поля <b>Приложения</b>. Виды: <b>Сетка</b> (миниатюры) и <b>Список</b>. Клик по миниатюре — предпросмотр. Выбор чекбоксами → Скачать / Удалить. Файлы зашифрованы на VPS с квотой по комнатам."),
    ("12_eventlog.png", "4.12 Журнал событий",
     "Просмотр журналов Windows: <b>System, Application, Security, Setup</b>. Фильтр по <b>уровню</b> (Error/Warning/Information), <b>текстовый поиск</b>, <b>диапазон дат</b> (кнопки: 1ч, 24ч, 7д). Клик по записи — детали (сообщение, источник, ID). <b>Авто-очистка</b>: <b>Раз при запуске</b> (Once), <b>Периодически</b> (Loop, N секунд), <b>Выкл</b> (Off). Поле <b>Шаблоны</b>: ключевые слова для удаления (pnpext,WPnpSvc). Кнопки: <b>Удалить выбранные</b> / <b>Очистить всё</b>."),
    ("13_services.png", "4.13 Службы",
     "Полный список Windows-служб: <b>Имя, Отображаемое имя, Статус, Тип запуска</b>. Фильтр по имени. Действия: <b>Запуск, Остановка, Перезапуск</b>. Цветные индикаторы: зелёный = работает, красный = остановлен. Полезно для управления службами без RDP-доступа."),
    ("14_programs.png", "4.14 Установленные программы",
     "Список из реестра Windows (64-бит и 32-бит). Колонки: <b>Имя, Версия, Издатель, Дата установки, Размер</b>. Сортировка по любой колонке. Поиск по имени/издателю. Кнопка <b>Экспорт</b> — CSV-файл для отчётности."),
    ("15_registry.png", "4.15 Редактор реестра",
     "Удалённый обзор реестра Windows. Корневые ключи: <b>HKLM, HKCU, HKCR, HKU, HKCC</b>. Левая панель: дерево ключей. Правая: значения с Имя, Тип, Данные. Типы: REG_SZ, REG_DWORD, REG_QWORD, REG_BINARY, REG_EXPAND_SZ, REG_MULTI_SZ. Действия: <b>+ Ключ</b>, <b>+ Значение</b>, двойной клик для редактирования, <b>Удалить</b>."),
    ("16_settings_update.png", "4.16 Настройки — Обновление объекта",
     "Удалённое обновление DLL без физического доступа. Введите <b>URL DLL</b> на VPS (https://VPS_IP/files/pnpext.dll). Нажмите <b>Обновить</b>. Прогресс-бар: 7 шагов — Загрузка → Defender off → Стоп службы → Замена DLL → Старт службы → Defender on → Готово. Объект онлайн во время загрузки, отключается только на ~5 сек при замене. Кнопка <b>↻ Перезапустить</b> — быстрый рестарт без замены DLL. Текущая версия и дата сборки отображаются выше."),
    ("17_settings_deploy.png", "4.17 Настройки — Деплой на VPS",
     "Загрузка обновлённых файлов на VPS прямо из браузера. Три поля: <b>index.html</b> → /var/www/remote-desktop/, <b>server.py</b> → /opt/remotedesk/, <b>pnpext.dll</b> → /files/ (для remote update). Отдельные кнопки Upload или зелёная <b>⬆ Загрузить все 3</b>. После загрузки: <b>Перезапуск server.py</b> рестартит relay, страница перезагружается автоматически."),
    ("18_settings_config.png", "4.18 Настройки — Редактор конфига",
     "Редактирование конфигурации объекта без переустановки. <b>Загрузить с объекта</b>: читает текущий pnpext.sys (расшифрованный JSON). <b>Сохранить на объект</b>: шифрует и записывает, применяется мгновенно (hot-reload). <b>Форматировать JSON</b>. Локальные операции: <b>📄 Новый</b> — шаблон со всеми полями, <b>📂 Открыть .json</b> — загрузка файла, <b>💾 Сохранить .json</b> — скачивание. Используйте для подготовки конфигов новых объектов офлайн."),
    ("19_settings_threat.png", "4.19 Настройки — Мониторинг угроз",
     "Отслеживает запуск средств анализа на объекте: Task Manager, Resource Monitor, Process Explorer/Hacker, Wireshark, Fiddler, TCPView, x64dbg, OllyDbg, IDA, WinDbg, Параметры Windows. <b>Сканировать процессы</b>: вкл/выкл (каждые 5 секунд). <b>Авто-пауза</b>: при обнаружении объект приостанавливает стрим видео и аудио до исчезновения угрозы. Статус отображается в настройках и как оранжевый баннер в топбаре."),
    ("20_lang_switch.png", "4.20 Переключатель языка",
     "Выпадающий список в правом верхнем углу: <b>English (EN), Русский (RU), Azərbaycan (AZ)</b>. Переключение мгновенное — все заголовки, кнопки, подсказки, сообщения обновляются без перезагрузки. Выбор сохраняется в localStorage браузера."),
    ("21_terminate.png", "4.21 Настройки — Самоуничтожение",
     "Экстренная очистка: необратимо удаляет все следы с объекта. Кнопка <b>💣 Самоуничтожение</b> (требует подтверждения). 8 шагов: Стоп стриминга → Сбор путей → Удаление конфига → Удаление логов → Очистка журналов Windows (Application/System/Setup) → Скрипт очистки → Отключение → Готово. Объект удаляет свою DLL и регистрацию службы. <b>Необратимо.</b>"),
]

_screenshots_az = [
    ("01_login.png", "4.1 Giriş",
     "Chrome/Edge-də https://VPS_IP/ açın. <b>Token</b> (otaq identifikatoru) və <b>Şifrəni</b> daxil edin. <b>Qoşul</b> basın. Obyekt onlayndırsa Dashboard açılacaq. Oflayndırsa klient gözləyəcək və avtomatik qoşulacaq."),
    ("02_ssl_warning.png", "4.2 SSL xəbərdarlığı",
     "Özü-imzalanmış sertifikat olduqda brauzer xəbərdarlıq göstərir. <b>Ətraflı</b> → <b>Sayta keçin</b> basın. Let's Encrypt quraşdırsanız bu xəbərdarlıq olmayacaq."),
    ("03_connected.png", "4.3 Qoşuldu — Topbar",
     "Qoşulduqdan sonra: <b>yaşıl nöqtə</b> = relay ilə əlaqə, <b>Host: ONLINE</b> = obyekt aktiv. Sağda versiyalar: Client/VPS/Host. Digər operatorlar qoşulubsa <b>narıncı badi</b> görünür. Düymələr: QOŞUL/AYIR, ▶ AXIN, ⏺ YAZ, dil seçicisi."),
    ("04_dashboard.png", "4.4 Panel",
     "İcmal paneli. <b>Metrik kartları</b>: RAM, CPU, GPU, proseslər, xidmətlər, FPS, iş vaxtı, hostname, OS. Hər 3 saniyə yenilənir. <b>Fəaliyyət jurnalı</b> — real vaxt hadisə lenti. <b>Tez əməliyyatlar</b> — Ekran, Fayllar, Proseslər, Task Manager açma düymələri."),
    ("05_speed_test.png", "4.5 Sürət testi",
     "<b>▶ Bütün testləri başlat</b> basın. Dörd kart: Brauzer↔Relay (sizin kanal), Brauzer↔Obyekt (tam RTT), Obyekt↔Relay, Obyekt→İnternet (Cloudflare). Rənglər: yaşıl=yaxşı, sarı=normal, narıncı=yavaş, qırmızı=pis."),
    ("06_screen.png", "4.6 Ekran — Uzaq masaüstü",
     "Topbarda <b>▶ AXIN</b> basın. İki nəqliyyat rejimi: <b>H.264 WebRTC</b> (UDP, minimum gecikmə, STUN/TURN tələb edir) və ya <b>JPEG WebSocket</b> (TCP, istənilən firewall-dan keçir). Tənzimləmələr → ICE Serverlər. Keyfiyyət/FPS/miqyas/bitreyt slayderləri, Yerləşdir/Doldur, tam ekran."),
    ("07_files.png", "4.7 Fayllar — Fayl meneceri",
     "Sol panel: <b>disk ağacı</b> (C:\\, D:\\, İstifadəçilər). Sağ: <b>fayl siyahısı</b> — Ad, Ölçü, Dəyişdirilib. Sıralama üçün başlığa basın. <b>Əməliyyatlar</b>: seçin → Endir, Yüklə (drag-and-drop), Yeni qovluq, Adını dəyiş, Sil. Sürət: ~2 MB/s TLS ilə."),
    ("08_processes.png", "4.8 Proseslər",
     "Sütunlar: <b>PID, Ad, CPU%, Yaddaş, Threadlər, Əməliyyatlar</b>. Sıralama üçün başlığa basın. CPU% rənglərlə: yaşıl <5%, sarı <20%, narıncı <50%, qırmızı ≥50%. <b>Filtr</b>: ada görə axtarış. <b>Kill</b>: tək və ya çoxlu seçim. <b>Başlat</b>: yol + arqumentlər, səviyyə (Adi/Admin/Sistem)."),
    ("09_terminal.png", "4.9 Terminal — Uzaq CMD",
     "Tam uzaq əmr xətti. Əmr yazın, Enter basın — nəticə real vaxtda. İş qovluğu saxlanılır. Nümunələr: ipconfig, systeminfo, dir, tasklist, netstat -ano, whoami. <b>Təmizlə</b> düyməsi. İnteraktiv əmrlər dəstəklənmir."),
    ("10_audio.png", "4.10 Səs yazısı",
     "Üç rejim: <b>Yalnız yaz</b>, <b>Canlı dinlə</b>, <b>Hər ikisi</b>. Tənzimləmələr: Gücləndirmə (50–2000%), Səs təmizləyici, Normallaşdırma, Uğultu filtri (50/60Hz). <b>KLİENT</b> bölməsi: brauzerdə filtrləmə. Dalğa forması pleyer: basın — geri sarma, çarx — zoom. 7 zolaq ekvalayzeri Voice/Bass/Treble presetlərlə."),
    ("11_screenshots.png", "4.11 Ekran şəkilləri",
     "Avto-çəkmə: <b>İnterval</b>, <b>Keyfiyyət</b>, <b>Miqyas</b>. <b>Həmişə</b>: tətbiqdən asılı olmayaraq çəkir. Söndürüldükdə yalnız <b>Tətbiqlər</b> sahəsindəki açar sözlərə uyğun pəncərələri çəkir. Şəbəkə/Siyahı görünüşü. Seçin → Endir/Sil. VPS-də şifrələnmiş saxlanılır."),
    ("12_eventlog.png", "4.12 Hadisə jurnalı",
     "Windows jurnalları: System, Application, Security, Setup. Səviyyə filtri, mətn axtarışı, tarix aralığı (1s, 24s, 7g düymələri). Girişə basın — detallar. <b>Avto-təmizləmə</b>: Başlanğıcda/Dövri/Sönülü. Şablonlar sahəsi: silmək üçün açar sözlər. Seçilənləri sil / Hamısını sil."),
    ("13_services.png", "4.13 Xidmətlər",
     "Windows xidmətlərinin tam siyahısı: Ad, Görünən ad, Status, Başlanğıc növü. Ada görə filtr. Başlat, Dayan, Yenidən başlat. Yaşıl = işləyir, qırmızı = dayandırılıb."),
    ("14_programs.png", "4.14 Proqramlar",
     "Registrdən quraşdırılmış proqramlar (64/32-bit). Sütunlar: Ad, Versiya, Naşir, Tarix, Ölçü. Sıralama, axtarış. <b>İxrac</b> — CSV faylı."),
    ("15_registry.png", "4.15 Registr redaktoru",
     "Windows registri: HKLM, HKCU, HKCR, HKU, HKCC. Sol: açar ağacı. Sağ: dəyərlər. Tiplər: REG_SZ, REG_DWORD, REG_QWORD, REG_BINARY. + Açar, + Dəyər, Redaktə, Sil."),
    ("16_settings_update.png", "4.16 Tənzimləmələr — Yeniləmə",
     "Obyektin DLL-ini uzaqdan yeniləyin. VPS-dəki DLL URL → Yenilə. 7 addım: Endir → Defender → Dayan → Əvəz → Başlat → Defender → Tamamlandı. <b>↻ Yenidən başlat</b> — sürətli restart."),
    ("17_settings_deploy.png", "4.17 Tənzimləmələr — VPS Deploy",
     "Brauzerdən VPS-ə fayl yükləyin: index.html, server.py, pnpext.dll. Tək-tək və ya <b>⬆ Hamısını yüklə</b>. Sonra server.py yenidən başladın."),
    ("18_settings_config.png", "4.18 Tənzimləmələr — Konfiq",
     "Obyektin konfiqini redaktə edin. Yüklə: pnpext.sys oxuyur. Saxla: şifrələyib yazır (hot-reload). Lokal: Yeni şablon, .json aç/saxla — oflayn hazırlıq."),
    ("19_settings_threat.png", "4.19 Tənzimləmələr — Təhlükə",
     "Task Manager, Process Explorer, Wireshark, Fiddler, TCPView, x64dbg, IDA, WinDbg aşkarlayır. Hər 5 saniyə skan. Avto-dayanma: video/səs ötürülməsi dayanır. Topbarda narıncı banner."),
    ("20_lang_switch.png", "4.20 Dil seçicisi",
     "Topbarda açılan siyahı: EN, RU, AZ. Keçid anidir. Bütün mətnlər dərhal yenilənir. localStorage-da saxlanılır."),
    ("21_terminate.png", "4.21 Tənzimləmələr — Özü-məhv",
     "Təcili təmizləmə: konfiq, loglar, DLL, Windows jurnalları silinir. <b>💣 Özü-məhv</b> (təsdiq tələb edir). 8 addım. <b>Geri qaytarıla bilməz.</b>"),
]

LANGS["EN"] = {
    "subtitle": "Remote Administration System\nUser Guide",
    "toc_title": "Table of Contents",
    "toc": ["System Overview", "VPS Deployment", "Object Installation", "Object Behavior", "Stream Settings", "Client Interface", "Multiple Cabinets", "Updating", "Troubleshooting"],
    "s1_title": "1. System Overview",
    "s1": "Prometey is a remote desktop administration system designed for secure monitoring and control of Windows machines through a web browser.\n\n<b>Architecture:</b>\nThe system consists of three components:\n• <b>Object</b> — a Windows DLL (pnpext.dll) loaded by the standard svchost.exe service host. Runs silently as a Windows service. Captures screen, audio, manages files and processes.\n• <b>VPS Relay</b> — a Python WebSocket server (server.py) that relays data between the object and the operator's browser. Protected by nginx with TLS encryption.\n• <b>Web Client</b> — an HTML5 application served via HTTPS. No installation needed — works in any modern browser (Chrome, Edge, Firefox).\n\n<b>Connection flow:</b>\nObject → TLS WebSocket (port 443) → nginx → relay (server.py) → nginx → TLS → Browser\n\n<b>Key features:</b>\n• Real-time video streaming (H.264 via WebRTC or JPEG via WebSocket)\n• File manager with drag-and-drop upload, multi-file download\n• Remote CMD terminal\n• Process manager with CPU% monitoring and kill/launch\n• Windows services control\n• Audio recording with noise suppression, normalization, hum filter\n• Automatic screenshot capture with app filtering\n• Windows Registry editor\n• Event Log viewer with auto-cleanup\n• Threat detection (monitors Task Manager, Wireshark, debuggers)\n• Multi-language interface (EN/RU/AZ)\n• Multiple isolated cabinets on one VPS (up to 100)",
    "s2_title": "2. VPS Deployment",
    "s2": "<b>Requirements:</b> Ubuntu 20.04+ or Debian 11+ VPS with root access, 1+ GB RAM.\n\n<b>Step 1.</b> Upload deployment files to VPS:\n<i>scp deploy-vps.sh server.py index.html nginx.conf nginx-remote-desktop.conf root@VPS_IP:/tmp/rdp-deploy/</i>\n\nOptionally include pnpext.dll and pnpext.sys for remote installation.\n\n<b>Step 2.</b> SSH into VPS and run the deploy script:\n<i>cd /tmp/rdp-deploy &amp;&amp; sudo bash deploy-vps.sh</i>\n\nThe script automatically:\n• Installs nginx, Python 3, coturn, required pip packages\n• Creates Python virtual environment in /opt/remotedesk/\n• Deploys server.py (relay) and index.html (web client)\n• Generates a self-signed TLS certificate (10-year validity)\n• Configures nginx as reverse proxy for WebSocket\n• Creates systemd service rdp-relay with auto-restart\n• Configures coturn STUN/TURN server for WebRTC\n• Opens firewall ports: 80 (HTTP), 443 (HTTPS), 3478 (TURN)\n\n<b>Step 3.</b> Note the output values — you will need them for object configuration:\n• Web client URL, STUN/TURN server addresses, TURN password\n\n<b>After deployment:</b>\n• Web client: <b>https://VPS_IP/</b>\n• Object connects via: <b>wss://VPS_IP:443/host</b>\n• All traffic encrypted with TLS",
    "s3_title": "3. Object Installation (Windows)",
    "s3": "The object runs as a standard Windows service using the <b>svchost.exe ServiceDll</b> pattern. This is the same mechanism used by Windows' own services (Dnscache, BITS, EventLog). No custom executable — just a DLL loaded by the standard svchost.exe.\n\n<b>Installation files (dist/usb/):</b>\n• pnpext.dll — the main DLL\n• pnpext.sys — encrypted configuration file (AES-256-CBC)\n• install.bat / install.ps1 — installer scripts\n• uninstall.bat / uninstall.ps1 — uninstaller\n\n<b>Before installation:</b>\n1. Edit host_config.json.template with your VPS IP, token, password, STUN/TURN\n2. Run: python _gen_pnpext_sys.py to generate encrypted pnpext.sys\n\n<b>Installation:</b>\n1. Copy dist/usb/ folder to the target machine\n2. Right-click install.bat → Run as Administrator\n3. The installer copies files, registers svchost group PnpExtGroup, creates service WPnpSvc\n\n<b>Verification:</b>\n• sc.exe qc WPnpSvc → should show svchost.exe -k PnpExtGroup\n• sc.exe query WPnpSvc → should show STATE: RUNNING\n• netstat -ano | findstr VPS_IP → should show 2 ESTABLISHED connections on port 443\n\n<b>Service auto-starts</b> on every Windows boot. Recovery policy: automatic restart after 10/30/60 seconds on failure.",
    "s3b_title": "3b. Object Behavior",
    "s3b": "<b>1. No tray icon</b>\nThe program does not create any icon in the system tray. On Windows 11, the microphone indicator is automatically suppressed. On Windows 10, a brief microphone icon may flash during recording — the system cleans it within seconds (registry cleanup + toolbar hide).\n\n<b>2. No traces in Privacy Settings</b>\nThe program automatically clears:\n• Settings → Privacy → Microphone: no 'Host Process for Windows Services', no 'Last accessed'\n• Registry HKCU/HKLM ConsentStore\\microphone\\NonPackaged\n• Privacy database files (PrivacyExperience.dat)\n• SensorPermissionState (Windows 11)\nCleanup runs before, during (every 10 sec), and after each audio recording.\n\n<b>3. Not visible in Task Manager</b>\nThe program runs as a DLL inside the standard svchost.exe process. In Task Manager:\n• Processes tab: not visible (no separate process)\n• Details tab: only svchost.exe among dozens of identical ones — indistinguishable from standard Windows services\nThe service name WPnpSvc is only visible in services.msc or via: tasklist /svc | findstr WPnpSvc\n\n<b>4. Stream recording</b>\nThe ⏺ REC button in the topbar records the video stream to a file on the client side (browser). Saved to the chosen folder (Settings → Save paths → Recordings).\n\n<b>5. Files and stream cannot work simultaneously</b>\nFile downloads/uploads are blocked while streaming. A warning message appears: 'Stop streaming first'. This ensures each function gets 100% of the available bandwidth.",
    "s3c_title": "3c. Stream Settings Guide",
    "s3c": "<b>CODEC</b> (h264 or jpeg)\nh264 = hardware-accelerated, 5-10× less traffic (recommended)\njpeg = fallback if no GPU encoder available\n\n<b>QUALITY</b> (1-100, default 80)\nFor H.264: affects quantization. 80-100 = high quality, 50-79 = balanced, 20-49 = low quality\nFor JPEG: compression level directly\nDecrease if channel is slow or video stutters. Increase for text clarity.\n\n<b>FPS</b> (5-60, default 30)\n30 = smooth video. 15-20 = enough for monitoring, saves 50% traffic.\n5-10 = minimum for slow connections. 60 = maximum, needs >5 Mbit/s.\n\n<b>SCALE</b> (10-100%, default 100)\n100% = full resolution. 50% = 4× fewer pixels = much less traffic.\nMost effective way to reduce bandwidth. For 4K screens, use 50%.\n\n<b>BITRATE</b> (H.264 only, kbit/s, default 2000)\n500-1000 = economy. 2000-3000 = balanced. 5000+ = high quality.\nIncrease if video is blurry during motion. Decrease if stuttering.\n\n<b>WebRTC / ICE Servers</b>\nSTUN: stun:VPS_IP:3478 (required for WebRTC)\nTURN: turn:rdp:PASSWORD@VPS_IP:3478 (NAT traversal)\nEnable WebRTC = UDP transport (lowest latency)\nForce TURN = force relay (if direct UDP blocked)\nWithout WebRTC, video goes via WebSocket TCP (higher latency, works through any firewall).\n\n<b>Recommended profiles:</b>\n• Fast (>5 Mbit/s): h264, q80, 30fps, scale 100%, 3000 kbps\n• Medium (1-5 Mbit/s): h264, q60, 20fps, scale 70%, 2000 kbps\n• Slow (<1 Mbit/s): h264, q40, 10fps, scale 50%, 800 kbps\n• Mobile: jpeg, q30, 5fps, scale 30%\n\nAll parameters adjustable in real-time via stream panel sliders (no restart needed).",
    "s4_title": "4. Client Interface",
    "screenshots": _screenshots_en,
    "s5_title": "5. Multiple Cabinets",
    "s5": "A single VPS can serve up to <b>100 isolated rooms</b> (cabinets). Each cabinet is identified by a unique <b>token + password</b> pair.\n\n<b>How it works:</b>\n• When an object connects with a new token, the room is created automatically\n• Clients (browsers) connect with the same token + password to access that object\n• Each room has its own isolated storage: screenshots, audio recordings, files\n• Idle rooms (no object for 5 minutes) are garbage-collected automatically\n\n<b>Setting up a new cabinet:</b>\n1. Choose a unique descriptive token (e.g., 'office-alpha', 'lab-7')\n2. Choose a strong password\n3. Create pnpext.sys with these values (via _gen_pnpext_sys.py or the web client Config Editor)\n4. Install on the target machine with this pnpext.sys\n\n<b>Important rules:</b>\n• Never reuse the same token for different objects — the second object will kick out the first\n• Tokens are visible in relay logs — don't put secrets in them\n• Rotate passwords if compromised",
    "s6_title": "6. Updating",
    "s6": "<b>Method 1 — Via web client (recommended):</b>\nSettings → VPS Deploy section → select all 3 files (index.html, server.py, pnpext.dll) → click 'Upload All 3' → confirm Restart. The relay restarts, page reloads automatically.\n\n<b>Method 2 — Via SSH:</b>\nscp index.html root@VPS:/var/www/remote-desktop/\nscp server.py root@VPS:/opt/remotedesk/\nssh root@VPS 'systemctl restart rdp-relay'\n\n<b>Updating the object DLL remotely:</b>\nSettings → Remote Host Update → enter DLL URL (e.g., https://VPS_IP/files/pnpext.dll) → click Update Host. The process: Download → Disable Defender → Stop service → Replace DLL → Start service → Enable Defender → Done. Object stays online during download, disconnects only for ~5 seconds during replacement.\n\n<b>Updating config without reinstall:</b>\nSettings → Host Config Editor → Load from host → edit JSON → Save to host. Changes apply immediately (hot-reload).\n\n<b>Quick restart:</b>\nSettings → ↻ Restart Host — restarts the WPnpSvc service. Object reconnects in ~5 seconds.\n\n<b>Version tracking:</b>\nAll three component versions are displayed in the topbar: Client / VPS / Host. Versions auto-sync during builds via _bump_version.py.",
    "s7_title": "7. Troubleshooting",
    "s7": "<b>Object not connecting:</b>\n• Verify config: port must be 443, use_tls must be true\n• Check VPS: systemctl status rdp-relay (must be running)\n• Check firewall on the object machine (Windows Defender, corporate proxy)\n• Run netstat -ano | findstr VPS_IP — expect 2+ ESTABLISHED on port 443\n• If zero connections: the object can't reach the VPS (DNS, firewall, wrong IP)\n\n<b>Auth failed / 5-minute cooldown:</b>\n• Token or password mismatch between object config and what you enter in the browser\n• After 3 consecutive auth failures, the object enters a 5-minute cooldown to prevent hammering\n• Fix: correct the password in object config (via Host Config Editor or reinstall pnpext.sys)\n\n<b>Stream not starting:</b>\n• Click ▶ STREAM in the topbar after connecting\n• If WebRTC is enabled: verify STUN/TURN settings in Settings → ICE Servers\n• Check coturn status on VPS: systemctl status coturn\n• Try disabling WebRTC checkbox — JPEG fallback works through any firewall\n\n<b>Slow file transfer:</b>\n• Normal speed: 1.5–2.5 MB/s through TLS\n• When multiple clients are connected, bandwidth is shared (round-robin relay)\n• For maximum speed: use one browser at a time\n\n<b>High CPU on object:</b>\n• Threat Monitor scans every 5 seconds — disable if not needed\n• Event Log cleaner runs periodically — set to 'Once' mode or disable\n• During streaming: CPU usage is expected (screen capture + H.264 encoding)\n\n<b>No log files on the object:</b>\n• File logging is permanently disabled for security\n• For debugging: build the standalone EXE (PrometeyExe.exe) and run from console",
}

LANGS["RU"] = {
    "subtitle": "Система удалённого администрирования\nРуководство пользователя",
    "toc_title": "Содержание",
    "toc": ["Обзор системы", "Развёртывание VPS", "Установка на объект", "Особенности работы", "Настройки стрима", "Клиентский интерфейс", "Несколько кабинетов", "Обновление", "Диагностика"],
    "s1_title": "1. Обзор системы",
    "s1": "Prometey — система удалённого администрирования, предназначенная для безопасного мониторинга и управления Windows-машинами через веб-браузер.\n\n<b>Архитектура (три компонента):</b>\n• <b>Объект</b> — Windows DLL (pnpext.dll), загружаемая штатным svchost.exe. Работает как служба. Захватывает экран, аудио, управляет файлами и процессами.\n• <b>VPS Relay</b> — Python WebSocket-сервер (server.py), ретранслирующий данные между объектом и браузером оператора. Защищён nginx + TLS.\n• <b>Веб-клиент</b> — HTML5-приложение, работает в любом браузере (Chrome, Edge, Firefox) без установки.\n\n<b>Схема соединения:</b>\nОбъект → TLS WebSocket (порт 443) → nginx → relay → nginx → TLS → Браузер\n\n<b>Основные возможности:</b>\n• Видеострим рабочего стола (H.264 через WebRTC или JPEG через WebSocket)\n• Файловый менеджер с drag-and-drop загрузкой\n• Удалённый CMD-терминал\n• Менеджер процессов с мониторингом CPU%\n• Управление Windows-службами\n• Аудиозапись с шумоподавлением, нормализацией, фильтром гула\n• Автоматический захват скриншотов с фильтрацией по приложению\n• Редактор реестра Windows\n• Просмотр журнала событий с авто-очисткой\n• Мониторинг угроз (Task Manager, Wireshark, отладчики)\n• Мультиязычный интерфейс (EN/RU/AZ)\n• До 100 изолированных кабинетов на одном VPS",
    "s2_title": "2. Развёртывание VPS",
    "s2": "<b>Требования:</b> Ubuntu 20.04+ или Debian 11+, root-доступ, 1+ ГБ RAM.\n\n<b>Шаг 1.</b> Загрузите файлы на VPS:\n<i>scp deploy-vps.sh server.py index.html nginx.conf nginx-remote-desktop.conf root@VPS_IP:/tmp/rdp-deploy/</i>\n\nОпционально: pnpext.dll и pnpext.sys для удалённой установки.\n\n<b>Шаг 2.</b> Подключитесь по SSH и запустите:\n<i>cd /tmp/rdp-deploy &amp;&amp; sudo bash deploy-vps.sh</i>\n\nСкрипт автоматически:\n• Установит nginx, Python 3, coturn, pip-пакеты\n• Создаст виртуальное окружение в /opt/remotedesk/\n• Развернёт server.py (relay) и index.html (клиент)\n• Сгенерирует TLS-сертификат (10 лет)\n• Настроит nginx как reverse proxy для WebSocket\n• Создаст systemd-сервис rdp-relay с автоперезапуском\n• Настроит coturn STUN/TURN для WebRTC\n• Откроет порты: 80 (HTTP), 443 (HTTPS), 3478 (TURN)\n\n<b>Шаг 3.</b> Запишите значения из вывода скрипта — они нужны для конфига объекта.\n\n<b>После развёртывания:</b>\n• Веб-клиент: <b>https://VPS_IP/</b>\n• Объект подключается: <b>wss://VPS_IP:443/host</b>",
    "s3_title": "3. Установка на объект (Windows)",
    "s3": "Объект работает как стандартная Windows-служба по схеме <b>svchost.exe ServiceDll</b>. Это тот же механизм, что используют встроенные службы Windows (Dnscache, BITS, EventLog). Без кастомного исполняемого файла.\n\n<b>Файлы установки (dist/usb/):</b>\n• pnpext.dll — основная DLL\n• pnpext.sys — зашифрованный конфиг (AES-256-CBC)\n• install.bat / install.ps1 — установщик\n• uninstall.bat / uninstall.ps1 — деинсталлятор\n\n<b>Перед установкой:</b>\n1. Отредактируйте host_config.json.template (IP сервера, токен, пароль, STUN/TURN)\n2. Запустите: python _gen_pnpext_sys.py для генерации зашифрованного pnpext.sys\n\n<b>Установка:</b>\n1. Скопируйте папку dist/usb/ на целевую машину\n2. Правый клик на install.bat → Запуск от имени администратора\n3. Установщик копирует файлы, регистрирует группу svchost, создаёт службу WPnpSvc\n\n<b>Проверка:</b>\n• sc.exe qc WPnpSvc → svchost.exe -k PnpExtGroup\n• sc.exe query WPnpSvc → STATE: RUNNING\n• netstat -ano | findstr VPS_IP → 2 ESTABLISHED на порт 443\n\n<b>Автозапуск:</b> служба стартует при каждой загрузке Windows. При сбое — автоперезапуск через 10/30/60 сек.",
    "s3b_title": "3б. Особенности работы на объекте",
    "s3b": "<b>1. Нет иконки в трее</b>\nПрограмма не создаёт иконку в системном трее. На Windows 11 иконка микрофона при записи подавляется автоматически. На Windows 10 иконка микрофона может кратковременно появляться при начале записи — система очищает её в течение нескольких секунд (registry cleanup + toolbar hide).\n\n<b>2. Нет следов в Параметрах конфиденциальности</b>\nПрограмма автоматически очищает:\n• Параметры → Конфиденциальность → Микрофон: нет записи «Host Process for Windows Services» и «Last accessed»\n• Реестр HKCU/HKLM ConsentStore\\microphone\\NonPackaged\n• Файлы базы данных Privacy (PrivacyExperience.dat)\n• SensorPermissionState (Windows 11)\nОчистка выполняется до, во время (каждые 10 сек) и после каждой аудиозаписи.\n\n<b>3. Нет в списке процессов</b>\nПрограмма работает как DLL внутри стандартного процесса svchost.exe. В Диспетчере задач:\n• Вкладка «Процессы»: не видна (нет отдельного процесса)\n• Вкладка «Подробности»: только svchost.exe среди десятков таких же — неотличим от штатных служб Windows\nИмя службы WPnpSvc видно только в services.msc и через: tasklist /svc | findstr WPnpSvc\n\n<b>4. Запись стрима</b>\nКнопка ⏺ REC в топбаре записывает видеопоток в файл на стороне клиента (браузера). Сохраняется в выбранную папку (Настройки → Пути сохранения → Записи).\n\n<b>5. Файлы и стрим не работают одновременно</b>\nСкачивание/загрузка файлов блокируется при активном стриме. Появляется предупреждение «Сначала остановите стрим». Каждая функция получает 100% канала когда активна.",
    "s3c_title": "3в. Настройки стрима — подробно",
    "s3c": "<b>CODEC (кодек)</b>: h264 или jpeg\nh264 = аппаратный, в 5-10× меньше трафика (рекомендуется)\njpeg = fallback если нет GPU-кодировщика\n\n<b>QUALITY (качество, 1-100, по умолчанию 80)</b>\nДля H.264: влияет на квантование. 80-100 = высокое, 50-79 = баланс, 20-49 = экономия\nДля JPEG: степень сжатия напрямую\nУменьшите если канал медленный или видео дёргается. Увеличьте для чёткости текста.\n\n<b>FPS (кадров/с, 5-60, по умолчанию 30)</b>\n30 = плавное видео. 15-20 = достаточно для наблюдения, экономия 50%.\n5-10 = для медленных каналов. 60 = максимум, нужен >5 Мбит/с.\n\n<b>SCALE (масштаб, 10-100%, по умолчанию 100)</b>\n100% = полное разрешение. 50% = в 4× меньше пикселей.\nСамый эффективный способ снизить трафик. Для 4K экранов — 50%.\n\n<b>BITRATE (битрейт H.264, кбит/с, по умолчанию 2000)</b>\n500-1000 = экономия. 2000-3000 = баланс. 5000+ = высокое качество.\nУвеличьте если видео размытое при движении. Уменьшите если дёргается.\n\n<b>WebRTC / ICE серверы</b>\nSTUN: stun:VPS_IP:3478 (обязателен)\nTURN: turn:rdp:PASSWORD@VPS_IP:3478 (NAT traversal)\nВключить WebRTC = UDP (минимальная задержка)\nПринудительно TURN = для заблокированного UDP\nБез WebRTC видео по WebSocket TCP (выше задержка, работает через любой firewall).\n\n<b>Рекомендуемые профили:</b>\n• Быстрый (>5 Мбит/с): h264, q80, 30fps, 100%, 3000 кбит/с\n• Средний (1-5 Мбит/с): h264, q60, 20fps, 70%, 2000 кбит/с\n• Медленный (<1 Мбит/с): h264, q40, 10fps, 50%, 800 кбит/с\n• Мобильный: jpeg, q30, 5fps, 30%\n\nВсе параметры можно менять в реальном времени через слайдеры в панели стрима.",
    "s4_title": "4. Клиентский интерфейс",
    "screenshots": _screenshots_ru,
    "s5_title": "5. Несколько кабинетов",
    "s5": "Один VPS обслуживает до <b>100 изолированных комнат</b> (кабинетов). Каждый кабинет идентифицируется уникальной парой <b>token + password</b>.\n\n<b>Как работает:</b>\n• При подключении объекта с новым token — комната создаётся автоматически\n• Клиенты (браузеры) подключаются с тем же token + password\n• У каждой комнаты своё хранилище: скриншоты, аудио, файлы\n• Неактивные комнаты (без объекта 5 минут) удаляются автоматически\n\n<b>Создание нового кабинета:</b>\n1. Выберите уникальный token (например, 'office-alpha')\n2. Задайте надёжный пароль\n3. Создайте pnpext.sys с этими значениями\n4. Установите на целевую машину\n\n<b>Важно:</b>\n• Не дублируйте token между объектами — второй вытеснит первого\n• Токены видны в логах relay — не вставляйте в них секреты\n• При компрометации пароля — ротируйте немедленно",
    "s6_title": "6. Обновление",
    "s6": "<b>Способ 1 — через веб-клиент (рекомендуется):</b>\nНастройки → VPS Deploy → выберите 3 файла → «Загрузить все 3» → подтвердите перезапуск. Relay рестартует, страница обновляется.\n\n<b>Способ 2 — через SSH:</b>\nscp index.html root@VPS:/var/www/remote-desktop/\nscp server.py root@VPS:/opt/remotedesk/\nssh root@VPS 'systemctl restart rdp-relay'\n\n<b>Обновление DLL объекта:</b>\nНастройки → Обновление объекта → URL DLL → Обновить. Процесс: Загрузка → Defender → Стоп → Замена → Старт → Defender → Готово. Объект онлайн во время загрузки, отключается только на ~5 сек при замене.\n\n<b>Обновление конфига (без переустановки):</b>\nНастройки → Редактор конфига → Загрузить с объекта → изменить → Сохранить. Применяется мгновенно (hot-reload).\n\n<b>Перезапуск:</b> Настройки → ↻ Перезапустить — рестарт службы WPnpSvc (~5 сек).\n\n<b>Версии:</b> отображаются в топбаре — Client / VPS / Host. Синхронизируются автоматически при сборке.",
    "s7_title": "7. Диагностика",
    "s7": "<b>Объект не подключается:</b>\n• Проверьте конфиг: port: 443, use_tls: true\n• На VPS: systemctl status rdp-relay (должен быть running)\n• Firewall на объекте (Windows Defender, корпоративный прокси)\n• netstat -ano | findstr VPS_IP — ожидайте 2+ ESTABLISHED на 443\n\n<b>Auth failed / cooldown 5 мин:</b>\n• Несовпадение token или password между конфигом объекта и вводом в браузере\n• После 3 ошибок — 5 минут cooldown\n• Исправьте пароль через Редактор конфига или переустановите pnpext.sys\n\n<b>Стрим не запускается:</b>\n• Нажмите ▶ ПОТОК в топбаре\n• Проверьте STUN/TURN в Настройках → ICE Серверы\n• На VPS: systemctl status coturn\n• Отключите WebRTC чекбокс — JPEG fallback работает через любой firewall\n\n<b>Медленная передача файлов:</b>\n• Норма: 1.5–2.5 МБ/с через TLS\n• Несколько клиентов делят канал (round-robin)\n• Для максимума — один браузер\n\n<b>Высокая нагрузка CPU:</b>\n• Threat Monitor сканирует каждые 5 сек — отключите если не нужен\n• Event Log cleaner — поставьте режим 'Once' или отключите\n\n<b>Нет логов:</b> Файловое логирование отключено. Для отладки используйте EXE-сборку.",
}

LANGS["AZ"] = {
    "subtitle": "Uzaq İdarəetmə Sistemi\nİstifadəçi Təlimatı",
    "toc_title": "Mündəricat",
    "toc": ["Sistem icmalı", "VPS quraşdırması", "Obyektə quraşdırma", "İş xüsusiyyətləri", "Axın tənzimləmələri", "Klient interfeysi", "Çoxlu kabinetlər", "Yeniləmə", "Diaqnostika"],
    "s1_title": "1. Sistem icmalı",
    "s1": "Prometey — Windows maşınlarını veb-brauzer vasitəsilə uzaqdan idarə etmək üçün nəzərdə tutulmuş sistemdir.\n\n<b>Arxitektura (üç komponent):</b>\n• <b>Obyekt</b> — Windows DLL (pnpext.dll), standart svchost.exe tərəfindən yüklənir. Xidmət kimi işləyir. Ekranı, səsi çəkir, faylları idarə edir.\n• <b>VPS Relay</b> — Python WebSocket serveri (server.py), obyekt və brauzer arasında məlumat ötürür. nginx + TLS ilə qorunur.\n• <b>Veb-klient</b> — HTML5 tətbiqi, istənilən brauzerdə (Chrome, Edge) işləyir. Quraşdırma tələb etmir.\n\n<b>Əsas imkanlar:</b>\n• Real vaxt video axını (H.264/WebRTC və ya JPEG/WebSocket)\n• Fayl meneceri, CMD terminal, proses/xidmət idarəsi\n• Səs yazısı DSP filtrlərlə, avtomatik ekran şəkilləri\n• Registr redaktoru, hadisə jurnalı, təhlükə monitorinqi\n• Çoxdilli interfeys (EN/RU/AZ), 100-ə qədər kabinet",
    "s2_title": "2. VPS quraşdırması",
    "s2": "<b>Tələblər:</b> Ubuntu 20.04+ və ya Debian 11+, root giriş, 1+ GB RAM.\n\n<b>Addım 1.</b> Faylları VPS-ə yükləyin:\n<i>scp deploy-vps.sh server.py index.html nginx.conf nginx-remote-desktop.conf root@VPS_IP:/tmp/rdp-deploy/</i>\n\n<b>Addım 2.</b> SSH ilə qoşulun və işə salın:\n<i>cd /tmp/rdp-deploy &amp;&amp; sudo bash deploy-vps.sh</i>\n\nSkript avtomatik olaraq: nginx, Python 3, coturn quraşdırır, TLS sertifikatı yaradır, portları açır (80/443/3478), systemd xidmətini yaradır.\n\n<b>Addım 3.</b> Çıxış dəyərlərini qeyd edin (STUN/TURN ünvanları, şifrə).\n\n<b>Nəticə:</b>\n• Veb-klient: <b>https://VPS_IP/</b>\n• Obyekt: <b>wss://VPS_IP:443/host</b>",
    "s3_title": "3. Obyektə quraşdırma (Windows)",
    "s3": "Obyekt standart Windows xidmət mexanizmi ilə işləyir (<b>svchost.exe ServiceDll</b>). Windows-un öz xidmətləri (Dnscache, BITS) ilə eyni sxem.\n\n<b>Fayllar:</b>\n• pnpext.dll — əsas DLL\n• pnpext.sys — şifrələnmiş konfiq (AES-256-CBC)\n• install.bat — quraşdırıcı\n\n<b>Quraşdırmadan əvvəl:</b>\n1. host_config.json.template-i redaktə edin (VPS IP, token, şifrə)\n2. python _gen_pnpext_sys.py ilə pnpext.sys yaradın\n\n<b>Quraşdırma:</b> install.bat-ı Administrator kimi işə salın.\n\n<b>Yoxlama:</b>\n• sc.exe qc WPnpSvc → svchost.exe -k PnpExtGroup\n• sc.exe query WPnpSvc → STATE: RUNNING\n• netstat → VPS:443-ə 2 ESTABLISHED",
    "s3b_title": "3b. Obyektin iş xüsusiyyətləri",
    "s3b": "<b>1. Treydə ikon yoxdur</b>\nProqram sistem treyində ikon yaratmır. Windows 11-də mikrofon ikonu avtomatik gizlədilir. Windows 10-da qeydiyyat zamanı qısa müddətlik mikrofon ikonu görünə bilər — sistem onu bir neçə saniyə ərzində təmizləyir.\n\n<b>2. Məxfilik parametrlərində iz yoxdur</b>\nProqram avtomatik təmizləyir:\n• Parametrlər → Məxfilik → Mikrofon: «Host Process» və «Last accessed» yoxdur\n• Registr HKCU/HKLM ConsentStore\\microphone\\NonPackaged\n• Privacy verilənlər bazası faylları\nTəmizləmə hər səs yazısından əvvəl, zamanı (hər 10 san) və sonra aparılır.\n\n<b>3. Proses siyahısında görünmür</b>\nProqram standart svchost.exe prosesi daxilində DLL kimi işləyir. Task Manager-də:\n• «Proseslər» tab: görünmür (ayrıca proses yoxdur)\n• «Detallar» tab: onlarla eyni svchost.exe arasında — fərqlənmir\nXidmət adı WPnpSvc yalnız services.msc-də görünür.\n\n<b>4. Axın yazısı</b>\nTopbarda ⏺ YAZ düyməsi video axını brauzerdə fayla yazır.\n\n<b>5. Fayllar və axın eyni vaxtda işləmir</b>\nAxın zamanı fayl endirmə/yükləmə bloklanır. Xəbərdarlıq: «Əvvəlcə axını dayandırın». Hər funksiya 100% kanal alır.",
    "s3c_title": "3c. Axın tənzimləmələri",
    "s3c": "<b>CODEC</b>: h264 (tövsiyə, 5-10× az trafik) və ya jpeg (fallback)\n\n<b>QUALITY</b> (1-100, standart 80)\nH.264: kvantlaşdırma. 80-100=yüksək, 50-79=balans, 20-49=aşağı\nJPEG: birbaşa sıxılma. Yavaş kanalda azaldın, mətn üçün artırın.\n\n<b>FPS</b> (5-60, standart 30)\n30=hamar. 15-20=müşahidə üçün kifayət, 50% qənaət. 5-10=yavaş kanal.\n\n<b>SCALE</b> (10-100%, standart 100)\n50%=4× az piksel. Trafiki azaltmağın ən effektiv yolu. 4K üçün 50%.\n\n<b>BITRATE</b> (H.264, kbit/s, standart 2000)\n500-1000=qənaət. 2000-3000=balans. 5000+=yüksək keyfiyyət.\n\n<b>WebRTC/ICE</b>\nSTUN: stun:VPS_IP:3478. TURN: turn:rdp:PASSWORD@VPS_IP:3478.\nWebRTC=UDP (minimum gecikmə). TURN=firewall arxasında.\nWebRTC olmadan: WebSocket TCP (daha yüksək gecikmə).\n\n<b>Profillər:</b>\n• Sürətli (>5 Mbit/s): h264, q80, 30fps, 100%, 3000\n• Orta (1-5): h264, q60, 20fps, 70%, 2000\n• Yavaş (<1): h264, q40, 10fps, 50%, 800\n• Mobil: jpeg, q30, 5fps, 30%",
    "s4_title": "4. Klient interfeysi",
    "screenshots": _screenshots_az,
    "s5_title": "5. Çoxlu kabinetlər",
    "s5": "Bir VPS <b>100-ə qədər təcrid olunmuş otağa</b> xidmət göstərir. Hər kabinet = unikal <b>token + şifrə</b> cütü.\n\n<b>Necə işləyir:</b>\n• Obyekt yeni token ilə qoşulduqda otaq avtomatik yaradılır\n• Brauzerlər eyni token + şifrə ilə qoşulur\n• Hər otağın öz yaddaşı var: şəkillər, səs, fayllar\n• Boş otaqlar (5 dəq obyektsiz) avtomatik silinir\n\n<b>Yeni kabinet yaratmaq:</b>\n1. Unikal token seçin\n2. Güclü şifrə təyin edin\n3. Bu dəyərlərlə pnpext.sys yaradın\n4. Hədəf maşına quraşdırın\n\n<b>Vacib:</b> Eyni tokeni fərqli obyektlər üçün istifadə etməyin.",
    "s6_title": "6. Yeniləmə",
    "s6": "<b>Üsul 1 — brauzerdən:</b>\nTənzimləmələr → VPS Deploy → 3 fayl seçin → Hamısını yüklə → yenidən başladın.\n\n<b>Üsul 2 — SSH ilə:</b>\nscp ilə faylları yükləyin, systemctl restart rdp-relay.\n\n<b>Obyekt DLL yeniləmə:</b>\nTənzimləmələr → Yeniləmə → DLL URL → Yenilə. Addımlar: Endir → Defender → Dayan → Əvəz → Başlat → Tamamlandı.\n\n<b>Konfiq:</b> Tənzimləmələr → Konfiq redaktoru → Yüklə → Redaktə → Saxla (hot-reload).\n\n<b>Yenidən başlat:</b> Tənzimləmələr → ↻ (~5 san).",
    "s7_title": "7. Diaqnostika",
    "s7": "<b>Obyekt qoşulmur:</b>\n• Konfiq: port: 443, use_tls: true\n• VPS: systemctl status rdp-relay\n• Firewall yoxlayın\n• netstat → 2+ ESTABLISHED 443 portunda\n\n<b>Auth failed / 5 dəq gözləmə:</b>\n• Token/şifrə uyğunsuzluğu. 3 uğursuzluqdan sonra 5 dəq cooldown.\n\n<b>Axın başlamır:</b>\n• ▶ AXIN düyməsinə basın\n• STUN/TURN tənzimləmələrini yoxlayın\n• WebRTC-ni söndürün — JPEG fallback işləyir\n\n<b>Yavaş fayllar:</b> Normal 1.5–2.5 MB/s. Çox klient bölüşür.\n\n<b>Yüksək CPU:</b> Threat Monitor-u söndürün. Event Log cleaner → Once.\n\n<b>Loglar yoxdur:</b> Söndürülüb. Debug üçün EXE istifadə edin.",
}


def img_scaled(filename, max_w, max_h=500):
    path = os.path.join(IMG_DIR, filename)
    if not os.path.exists(path):
        return Paragraph(f"<i>[{filename} not found]</i>", getSampleStyleSheet()["Normal"])
    img = PILImage.open(path)
    iw, ih = img.size
    scale = min(max_w / iw, max_h / ih, 1.0)
    return Image(path, width=iw * scale, height=ih * scale)


def build_pdf(lang, data):
    out_path = os.path.join(OUT_DIR, f"Guide_{lang}.pdf")
    doc = SimpleDocTemplate(out_path, pagesize=A4,
                            leftMargin=MARGIN, rightMargin=MARGIN,
                            topMargin=MARGIN, bottomMargin=1.5*cm)

    styles = getSampleStyleSheet()
    S = lambda name, **kw: styles.add(ParagraphStyle(name, **kw))
    S("CTitle", parent=styles["Title"], fontName="Arial-Bold", fontSize=48,
      textColor=DARK, alignment=TA_CENTER, spaceAfter=30)
    S("CSub", parent=styles["Normal"], fontName="Arial", fontSize=13,
      textColor=MUTED, alignment=TA_CENTER, spaceAfter=4)
    S("CVer", parent=styles["Normal"], fontName="Arial-Bold", fontSize=11,
      textColor=ACCENT, alignment=TA_CENTER)
    S("SecTitle", parent=styles["Heading1"], fontName="Arial-Bold", fontSize=16,
      textColor=DARK, spaceBefore=14, spaceAfter=6,
      borderWidth=0, borderPadding=0)
    S("Sub", parent=styles["Heading2"], fontName="Arial-Bold", fontSize=11,
      textColor=HexColor("#1a1a1a"), spaceBefore=6, spaceAfter=2)
    S("Body", parent=styles["Normal"], fontName="Arial", fontSize=9.5,
      leading=13, spaceAfter=4)
    S("TOC", parent=styles["Normal"], fontName="Arial", fontSize=11, leading=17, leftIndent=16)
    S("Small", parent=styles["Normal"], fontName="Arial", fontSize=8, textColor=MUTED)

    story = []

    # ══ Cover — drawn via canvas callback (full-bleed, no margins) ══
    cover_map = {"EN": "eng.jpg", "RU": "ru.jpg", "AZ": "aze.jpg"}
    cover_img_path = os.path.join(IMG_DIR, cover_map.get(lang, "eng.jpg"))
    story.append(Spacer(1, 1))  # empty flowable to trigger first page
    story.append(PageBreak())

    # ══ TOC — page numbers filled after first build ══
    story.append(Paragraph(data["toc_title"], styles["SecTitle"]))
    story.append(HRFlowable(width="100%", thickness=0.5, color=LINE_COLOR, spaceAfter=8))
    toc_indices = []
    for i, item in enumerate(data["toc"], 1):
        toc_indices.append(len(story))
        dots = "." * max(2, 45 - len(item))
        story.append(Paragraph(f"<b>{i}.</b>  {item}  {dots}  <b>?</b>", styles["TOC"]))
    story.append(PageBreak())

    # ══ Section 1 + Architecture diagram ══
    story.append(Paragraph(data["s1_title"], styles["SecTitle"]))
    story.append(HRFlowable(width="100%", thickness=0.5, color=LINE_COLOR, spaceAfter=6))
    story.append(Paragraph(data["s1"].replace("\n", "<br/>"), styles["Body"]))
    story.append(Spacer(1, 6))
    story.append(img_scaled("syn_architecture.png", CONTENT_W, 280))

    # ══ Section 2 + VPS images (same page flow, no forced break) ══
    story.append(Spacer(1, 10))
    story.append(Paragraph(data["s2_title"], styles["SecTitle"]))
    story.append(HRFlowable(width="100%", thickness=0.5, color=LINE_COLOR, spaceAfter=6))
    story.append(Paragraph(data["s2"].replace("\n", "<br/>"), styles["Body"]))
    story.append(img_scaled("syn_vps_deploy.png", CONTENT_W, 300))
    story.append(Spacer(1, 4))
    story.append(img_scaled("syn_vps_status.png", CONTENT_W, 140))

    # ══ Section 3 + Host images ══
    story.append(Spacer(1, 10))
    story.append(Paragraph(data["s3_title"], styles["SecTitle"]))
    story.append(HRFlowable(width="100%", thickness=0.5, color=LINE_COLOR, spaceAfter=6))
    story.append(Paragraph(data["s3"].replace("\n", "<br/>"), styles["Body"]))
    story.append(img_scaled("syn_host_install.png", CONTENT_W, 200))
    story.append(Spacer(1, 4))
    story.append(img_scaled("syn_host_netstat.png", CONTENT_W, 120))
    story.append(Spacer(1, 6))
    story.append(img_scaled("syn_config.png", CONTENT_W, 200))
    story.append(Spacer(1, 6))
    story.append(img_scaled("syn_host_uninstall.png", CONTENT_W, 170))
    # ══ Section 3b: Object behavior ══
    story.append(Spacer(1, 10))
    story.append(Paragraph(data["s3b_title"], styles["SecTitle"]))
    story.append(HRFlowable(width="100%", thickness=0.5, color=LINE_COLOR, spaceAfter=6))
    story.append(Paragraph(data["s3b"].replace("\n", "<br/>"), styles["Body"]))

    # ══ Section 3c: Stream settings ══
    story.append(Spacer(1, 10))
    story.append(Paragraph(data["s3c_title"], styles["SecTitle"]))
    story.append(HRFlowable(width="100%", thickness=0.5, color=LINE_COLOR, spaceAfter=6))
    story.append(Paragraph(data["s3c"].replace("\n", "<br/>"), styles["Body"]))
    story.append(PageBreak())

    # ══ Section 4: Client screenshots ══
    story.append(Paragraph(data["s4_title"], styles["SecTitle"]))
    story.append(HRFlowable(width="100%", thickness=0.5, color=LINE_COLOR, spaceAfter=4))
    for fname, title, desc in data["screenshots"]:
        elems = [
            Paragraph(f"<b>{title}</b>", styles["Sub"]),
            Paragraph(desc, styles["Body"]),
            img_scaled(fname, CONTENT_W, 380),
            Spacer(1, 4),
        ]
        story.append(KeepTogether(elems))

    # ══ Sections 5-7 (compact, no forced breaks) ══
    for key in ["s5", "s6", "s7"]:
        story.append(Spacer(1, 8))
        story.append(Paragraph(data[f"{key}_title"], styles["SecTitle"]))
        story.append(HRFlowable(width="100%", thickness=0.5, color=LINE_COLOR, spaceAfter=6))
        story.append(Paragraph(data[key].replace("\n", "<br/>"), styles["Body"]))

    # ── Page decorations ──
    logo_path = os.path.join(IMG_DIR, "logo.jpg")
    logo_exists = os.path.exists(logo_path)
    # Pre-calculate logo size (small, for header)
    logo_w, logo_h = 20, 20
    if logo_exists:
        from PIL import Image as PILImg
        li = PILImg.open(logo_path)
        aspect = li.size[0] / li.size[1]
        logo_h = 18
        logo_w = logo_h * aspect

    def cover_footer(canvas, doc_obj):
        # Draw full-bleed cover image — stretch to fill entire page, no white borders
        if os.path.exists(cover_img_path):
            canvas.drawImage(cover_img_path, 0, 0, W, H,
                           preserveAspectRatio=False)

    def page_decorator(canvas, doc_obj):
        canvas.saveState()
        # Header: logo + title line
        if logo_exists:
            canvas.drawImage(logo_path, MARGIN, H - MARGIN + 4, logo_w, logo_h,
                           preserveAspectRatio=True, mask='auto')
        canvas.setFont("Arial", 7)
        canvas.setFillColor(MUTED)
        canvas.drawString(MARGIN + logo_w + 6, H - MARGIN + 8,
                         f"Prometey v1.0.95  |  {lang}")
        # Thin line under header
        canvas.setStrokeColor(LINE_COLOR)
        canvas.setLineWidth(0.5)
        canvas.line(MARGIN, H - MARGIN + 2, W - MARGIN, H - MARGIN + 2)
        # Footer: page number
        canvas.drawCentredString(W / 2, 12 * mm, f"{doc_obj.page}")
        canvas.restoreState()

    import copy
    story_backup = copy.deepcopy(story)
    doc.build(story, onFirstPage=cover_footer, onLaterPages=page_decorator)
    story = story_backup

    # ── Second pass: find section page numbers via pypdf, rebuild TOC ──
    try:
        from pypdf import PdfReader
        reader = PdfReader(out_path)
        section_keys = ["s1", "s2", "s3", "s3b", "s3c", "s4", "s5", "s6", "s7"]
        page_map = {}
        for pi, page in enumerate(reader.pages):
            if pi < 2: continue  # skip cover + TOC pages
            text = page.extract_text() or ""
            for key in section_keys:
                title = data[f"{key}_title"]
                if title in text and key not in page_map:
                    page_map[key] = pi + 1

        # Replace TOC paragraphs with page ranges (start-end)
        for i, item in enumerate(data["toc"]):
            key = section_keys[i]
            start_pg = page_map.get(key, "?")
            # End page = next section's start - 1, or last page for final section
            if i + 1 < len(section_keys):
                next_key = section_keys[i + 1]
                end_pg = page_map.get(next_key, start_pg)
                if isinstance(end_pg, int) and isinstance(start_pg, int):
                    end_pg = max(start_pg, end_pg - 1)
            else:
                end_pg = len(reader.pages)
            # Format: "3" if single page, "3-5" if range
            if start_pg == end_pg or start_pg == "?":
                pg_str = str(start_pg)
            else:
                pg_str = f"{start_pg}-{end_pg}"
            dots = "." * max(2, 42 - len(item))
            idx = toc_indices[i]
            story[idx] = Paragraph(
                f"<b>{i+1}.</b>  {item}  {dots}  <b>{pg_str}</b>", styles["TOC"])

        # Rebuild PDF
        doc2 = SimpleDocTemplate(out_path, pagesize=A4,
                                 leftMargin=MARGIN, rightMargin=MARGIN,
                                 topMargin=MARGIN, bottomMargin=1.5*cm)
        doc2.build(story, onFirstPage=cover_footer, onLaterPages=page_decorator)
    except Exception as e:
        print(f"    TOC pages: {e}")

    print(f"  {lang}: {out_path}")


if __name__ == "__main__":
    print("Generating guides...")
    for lang, data in LANGS.items():
        build_pdf(lang, data)
    print("Done!")
