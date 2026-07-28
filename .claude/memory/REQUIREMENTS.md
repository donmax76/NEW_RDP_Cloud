# Project Requirements — NEW_RDP_Cloud

## Архитектура
- **Host**: Windows DLL (pnpext.dll) — захват экрана, аудио, управление, запускается как svchost.exe ServiceDll
- **Relay**: VPS (server.py, Python WebSocket) — порт 443, WSS, nginx reverse proxy
- **Client**: браузер (index.html) — WebSocket, Web Audio API
- **Схема**: Host ↔ VPS ↔ Browser (relay, не P2P)
- **Chain (dual-VPS)**: Host → VPS1 (chain relay) → VPS2 (main, web panel) ← Client

## Сборка
- Visual Studio 2022 Enterprise, MSVC, CMake + NMake
- Команда: `cmd /c "D:\Android_Projects\NEW_RDP_Cloud\_build.bat"` — авто-бамп версии, сборка, scrub OpenSSL, подпись, зеркалирование
- После каждого изменения host-кода: **bump HOST_VERSION в host.h** (или запустить _build.bat — он сделает это через _bump_version.py)
- После сборки: проверить mtime DLL vs исходники (nmake может no-op на header-only изменениях)
- Post-build (автоматически через _build.bat):
  - `build/bin/pnpext.dll` → `dist/usb/pnpext.dll`
  - `build/bin/pnpext.dll` → `release/HOST/pnpext.dll`
  - `build/bin/pnpext.dll` → `release/VPS/pnpext.dll`
  - `index.html`, `server.py` и др. → `release/VPS/`
- `pnpext.sys` (зашифрованный конфиг): генерировать через `_gen_pnpext_sys.py`, копировать в `build/bin/` и `dist/usb/`

## ЗАПРЕТЫ (критично)
- **НЕ** `/DELAYLOAD` в pnpext.dll — svchost Session 0 не переносит delay-load exceptions (регрессия v1.0.156–v1.0.161)
- **НЕ** CreateToolhelp32Snapshot / RegDeleteTreeW в аудио-цикле (только ≥10s gate)
- **НЕ** TerminateProcess на ShellExperienceHost из таймера (только event-driven)
- **НЕ** sc stop без PID-kill fallback в update bat
- **НЕ** WinHTTP запросы к api.github.com или любому внешнему dead-drop (Elastic ML детектирует как C2 T1071)
- **НЕ** `DISABLE_WEBRTC_STREAM=ON` — удаление libdatachannel увеличивает AV-детекцию (см. feedback_keep_webrtc_linked.md)

## Стрим / Аудио
- Видео: скриншоты через GDI/DXGI, JPEG или H264, через WebSocket
- Аудио: waveIn (mic) или WASAPI (system audio loopback) → PCM → Opus → WebSocket
- Opus валидные частоты: 8000, 12000, 16000, 24000, 48000 (44100 недопустим → fallback на 48000)
- waveInOpen: fallback chain по частотам при WAVERR_BADFORMAT
- DSP host-цепочка: HighPass → HumFilter → NoiseGate → Normalize (max_scale=3.5)
- DSP client: Web Audio API (realtime, отдельная цепочка)
- Нормализация: target 0.90, max_scale 3.5 (не 8.0 — избегаем blowup тишины)

## Журнал событий (evtlog)
- Режимы: разово при запуске / периодически
- Паттерны: regex, матчинг по ToXml() (ловит P1/P2/провайдер/task/сообщение)
- Cleaner thread: никогда не завершается, condition_variable для wake-up при смене конфига
- g_evtlog_config_gen: атомарный счётчик, инкрементируется при evtlog_set_config

## Обновление хоста (host_update)
- 7 шагов, прогресс-бар в UI
- Step 2: отключить Defender (может не сработать при Tamper Protection — продолжает без ошибки)
- Step 3: остановить сервис background sc stop + PID-based taskkill (не блокирует)
- Step 6: снова включить Defender
- После завершения: markDone() сбрасывает прогресс в 100% и зелёный цвет
- URL для скачивания DLL: `https://VPS_IP/files/pnpext.dll`

## Тест скорости (Speed Test)
- **Host ↔ Relay**: host скачивает `/speedtest` (5MB blob) с VPS через HTTPS
  - В chain-режиме: `/speedtest` должен быть на VPS1 (не только на VPS2)
  - Создать: `dd if=/dev/zero bs=1M count=5 > /var/www/remote-desktop/speedtest`
- **Host → Internet**: Cloudflare speed test через PowerShell (HTTPS port 443)
  - URL строится в рантайме из частей (не хардкодирован) — иначе `_scrub_dll_strings.ps1` затрёт
- **STAGE1_KEEP_FALLBACKS**: оба теста внутри этого `#ifdef`; без него возвращают "sysinfo module loading"

## STUN / ICE
- STUN бесполезен в relay-архитектуре (нет P2P)
- Чекбокс "Use STUN" в настройках — если выключен, stun_server = ""
- turn_server, stun_server сохраняются напрямую из g_config (не через get_existing)

## Сервисный ключ реестра
- Установщик ДОЛЖЕН записывать `ServiceMain = "PnpServiceEntry"` (не "ServiceMain")
- Старые инсталляторы (до v1.0.156) записывали неверное значение → тихий отказ запуска

## AV / Безопасность
- `_scrub_dll_strings.ps1` затирает NUL-байтами OpenSSL fingerprints из .rdata
- Cloudflare URLs (для speed test) строятся конкатенацией в рантайме — scrub их не трогает
- stage-2 encrypted blobs в `%TEMP%\pnp_cache\` — удаляются при остановке сервиса
- WebRTC (libdatachannel) оставить слинкованным — он снижает AV-детекцию через "library ballast"

## Сервер (server.py)
- Python, asyncio + websockets, порт 443, SSL (self-signed)
- RELAY_SECRET удалён (v1.0.139 — Cloudflare Worker убран)
- stage2_fetch handler: отдаёт encrypted blobs из `/opt/remotedesk/stage2/`

## VPS deploy (после сборки)
Через панель Settings → VPS Deploy:
- `server.py` → target: **relay** (попадает в `/opt/remotedesk/`)
- `index.html` → target: **web** (попадает в `/var/www/remote-desktop/`)
- `pnpext.dll` → target: **files** (попадает в `/var/www/remote-desktop/files/`)
- После загрузки: нажать **Restart Relay**

Вручную (SSH):
```bash
scp release/VPS/* root@VPS_IP:/root/data/
# затем перезапустить systemctl restart rdp-relay
```
