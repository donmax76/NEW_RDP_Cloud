# Project Requirements — NEW_RDP_Cloud

## Архитектура
- **Host**: Windows DLL (WPnpSvc / pnpext.dll) — захват экрана, аудио, управление
- **Relay**: VPS (server.py, Python WebSocket) — порт 443, WSS
- **Client**: браузер (index.html) — WebSocket, Web Audio API
- **Схема**: Host ↔ VPS ↔ Browser (не P2P, нет WebRTC)

## Сборка
- `nmake` из Visual Studio Developer Command Prompt
- После каждого изменения host-кода: **bump HOST_VERSION в host.h**
- После сборки: проверить mtime DLL vs исходники (nmake может no-op на header-only)
- Post-build: скопировать `build/bin/pnpext.dll` → `dist/usb/pnpext.dll`
- `pnpext.sys` (зашифрованный конфиг): генерировать через `_gen_pnpext_sys.py`, копировать в `build/bin/` и `dist/usb/`

## Стрим / Аудио
- Видео: скриншоты через GDI/DXGI, JPEG, через WebSocket
- Аудио: waveIn → PCM → Opus → WebSocket
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

## Обновление хоста
- 7 шагов, прогресс-бар в UI
- Step 3 (Stop service): background sc stop + PID-based taskkill (не блокирует)
- После завершения: markDone() сбрасывает прогресс в 100% и зелёный цвет

## STUN / ICE
- STUN бесполезен в relay-архитектуре (нет P2P)
- Чекбокс "Use STUN" в настройках — если выключен, stun_server = ""
- turn_server, stun_server сохраняются напрямую из g_config (не через get_existing)

## Безопасность / Маскировка
- Вся связь через порт 443 WSS
- Cloudflare Tunnel: вариант для скрытия реального IP (на будущее, пока VPS)

## Запреты (критично)
- **НЕ** CreateToolhelp32Snapshot / RegDeleteTreeW в аудио-цикле (только ≥10s gate)
- **НЕ** TerminateProcess на ShellExperienceHost из таймера (только event-driven)
- **НЕ** sc stop без PID-kill fallback в update bat

## Сервер (server.py)
- Python, WebSocket, порт 443, SSL
- Без RELAY_SECRET (убран вместе с Cloudflare Worker)
- Текущая версия: 1.0.139
