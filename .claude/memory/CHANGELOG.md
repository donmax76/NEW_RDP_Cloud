# Changelog — NEW_RDP_Cloud

Формат: версия | дата | что изменено | зачем | как откатить

---

## v1.0.209 | 2026-04-27
**WASAPI системный звук + самовосстановление сервиса**
- `main.cpp`: `capture_audio_direct()` — waveIn обёрнут в `if(g_audio_source==0)`, новая WASAPI ветка в else для системного звука (loopback); оба пути заполняют pcmBuf → DSP + Opus без изменений
- `main.cpp`: новая функция `capture_audio_live_stream_wasapi()` — полный live-стрим через `audio_wasapi::Capture`, 3-секундные OGG-чанки → ALIV, поддержка режима 2 (live+record)
- `main.cpp`: `capture_audio_live_stream()` — ранний return к WASAPI-пути при `g_audio_source==1`
- `audio_wasapi.h`: новый файл, полный класс `audio_wasapi::Capture` (IAudioClient loopback)
- `dllmain.cpp`: `PnpServiceEntry()` — автоматически устанавливает политику восстановления сервиса (restart 3s/10s/30s, INFINITE reset, SERVICE_AUTO_START); хост восстанавливается после любого падения
- `index.html`: dropdown Источник (Микрофон / Системный звук) в настройках аудио, `auOnSourceChange()` скрывает контрол устройства при системном звуке, `auApplyConfig()` + `auRefreshStatus()` передают `source`; i18n для EN/RU/AZ
- **VT детекции**: 0/71 (v1.0.207 базис, ENABLE_REFLECTIVE_LOADER отключён)
- **Откат**: `git checkout v1.0.207-tag` (или предыдущий коммит перед этим)

---

## v1.0.162 | 2026-04-20
**Убран /DELAYLOAD — фикс критической регрессии запуска сервиса**
- host.h: версия → 1.0.162
- main.cpp: удалены все `#pragma comment(linker, "/DELAYLOAD:*.dll")` (9 DLL)
- dllmain.cpp: удалены все `/DELAYLOAD` прагмы
- CMakeLists.txt: удалён `delayimp` из COMMON_LIBS, удалён блок `target_link_options(/DELAYLOAD:...)`
- **Причина**: v1.0.156–v1.0.161 не запускались как сервис на целевом хосте. Корень: `/DELAYLOAD` в svchost Session 0 не работает — DllMain падает при первом вызове из любой delay-loaded DLL (SEH-обработчик `delayimp` несовместим с окружением svchost). v1.0.150 (рабочая, без delay-load) подтвердила это.
- **Откат**: нет смысла откатывать, delay-load был регрессией с v1.0.156
- **Важно**: /DELAYLOAD НЕЛЬЗЯ использовать в pnpext.dll (svchost не переносит delay-load exceptions)

---

## v1.0.161 | 2026-04-17
**СЛОМАН — НЕ ИСПОЛЬЗОВАТЬ**
- Delay-load остался, сервис не запускается на хосте
- Откат: используй v1.0.162

---

## v1.0.156–160 | 2026-04-17
**СЛОМАНЫ — НЕ ИСПОЛЬЗОВАТЬ**
- v1.0.156: добавлен /DELAYLOAD — сервис перестал запускаться (regression)
- v1.0.157-158: XOR string obfuscation (obfstr.h XS()/XSW()) — lambda decode стал сигнатурой, reverted
- v1.0.159: XS() нейтрализован до no-op — та же детекция Elastic "moderate"
- v1.0.160: dynload.h (LoadLibrary+GetProcAddress) для 5 DLL — хуже (GetProcAddress args = сигнатура), reverted
- v1.0.161: delay-load только, не запустился

---

## v1.0.150–155 | 2026-04-17
**v1.0.150 — последняя рабочая версия перед регрессией**
- v1.0.150: Sleep(200) + double-tap mic boost после смены audio device
- v1.0.151: skip first buffer после смены устройства (3s аудио на старой громкости выбрасывается), normalize max_scale=6.0
- v1.0.152–155: промежуточные (не закоммичены, бинарей нет)
- **Бинарник v1.0.150**: `E:\AudioService\usb\pnpext.dll` (7,707,648 bytes, SHA256: 120849874DDDCB6945FD993F699B205B1402C0A96BAEE0B98EACFE96B0E51757)

---

## v1.0.141 | 2026-04-16
**Auto-stop streaming when no viewers — fixes CPU leak after client disconnect**
- main.cpp: `g_connected_clients`, `g_clients_zero_time_ms`, `viewer_watchdog_func()`
- main.cpp: `update_viewer_count()` — единая точка входа для client_joined / client_left / clients_online
- main.cpp: `handle_command` теперь обрабатывает события `event` перед командами
- main.cpp: сброс счётчика в 0 при каждом reconnect к VPS (в обоих main loops)
- main.cpp: viewer_watchdog_thread запускается в main() и host_main_loop(), joined на shutdown
- main.cpp: grace period 7s — не останавливаем сразу при client_left (page refresh)
- server.py: отправка `clients_online` и хосту, а не только клиентам (712, 1231)
- server.py: при подключении host — сразу отправляем ему текущий count (700)
- **Причина**: CPU не освобождался после отключения клиентов. stream_start стартует capture+encode потоки, но stream_stop вызывается только по явной команде клиента. Когда клиенты молча закрывают вкладку, потоки продолжают работать на full FPS.
- **Откат**: `git checkout v1.0.140 -- main.cpp server.py`

---

## v1.0.139 | 2026-04-16
**Удалены файлы Cloudflare Worker**
- Удалён `cloudflare/worker.js`
- Удалён `cloudflare/wrangler.toml`
- Удалён RELAY_SECRET из server.py
- **Причина**: Workers имеет лимит 1MB на WebSocket-сообщение — ломает передачу файлов и аудио
- **Откат**: `git checkout v1.0.138 -- cloudflare/ server.py`

---

## v1.0.138 | 2026-04-16
**STUN toggle + waveInOpen fallback + audio DSP fix**
- index.html: чекбокс "Use STUN" (`#ice-stun-enable`)
- main.cpp: waveInOpen fallback chain (48k→24k→16k→12k→8k) при WAVERR_BADFORMAT
- audio_dsp.h: noise gate bypass ratio `< 4.0f` → `< 2.0f`, pass-through → ×0.6 attenuation
- main.cpp: normalize max_scale 8.0 → 3.5
- **Причина**: запись останавливалась при смене частоты 16→44kHz; аудио то шумело то нет
- **Откат**: `git checkout v1.0.137 -- main.cpp audio_dsp.h index.html`

---

## v1.0.137 | 2026-04-16
**evtlog cleaner rework + ToXml matching + save_stream_settings fix**
- main.cpp: evtlog_cleaner_func — никогда не завершается, condition_variable wake-up
- main.cpp: g_evtlog_config_gen, g_evtlog_cv, g_evtlog_cv_mtx — forward declarations
- main.cpp: PowerShell ToXml() matching — ловит P1/P2 и любые поля события
- main.cpp: save_stream_settings — evtlog/ICE поля через g_config напрямую (не get_existing)
- **Причина**: настройки evtlog не сохранялись; cleaner умирал при пустых паттернах; svchost.exe_WPnpSvc не матчился
- **Откат**: `git checkout v1.0.136 -- main.cpp`

---

## v1.0.136 | 2026-04-16
**Update bat Step 3 fix + UI progress fix**
- main.cpp: Step 3 bat — background sc stop + PID taskkill вместо блокирующего sc stop
- main.cpp: rollback секция — аналогичный fix
- index.html: markDone() — сбрасывает все элементы прогресса в 100%/зелёный
- index.html: Step 0/9 → Step 0/7; stablyEmptyAfterRestart threshold 2→3
- **Причина**: обновление зависало на "Step 3/7 — Stopping service"; UI не показывал 100% после успеха
- **Откат**: `git checkout v1.0.135 -- main.cpp index.html`

---

## v1.0.135 и ранее
См. `git log --oneline` в репозитории.
