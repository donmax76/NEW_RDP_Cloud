# Changelog — NEW_RDP_Cloud

Формат: версия | дата | что изменено | зачем | как откатить

---

## v1.0.253 | 2026-07-28
**Speed test internet HTTPS + runtime URL build**
- `main.cpp`: `speed_test_internet` — URL строится в рантайме (`std::string _cf = std::string("speed") + ".cloudflare.com"`) чтобы избежать scrub; переключён на HTTPS + TLS12 enforcement
- Причина: `_scrub_dll_strings.ps1` затирал статическую строку `speed.cloudflare.com/__down?bytes=1000000` NUL-байтами → PowerShell получал пустой URL → Timeout
- VPS1 `/speedtest` файл создан (`dd if=/dev/zero bs=1M count=5`) для `host_relay_speed` в chain-режиме

---

## v1.0.209 | 2026-04-27
**WASAPI системный звук + самовосстановление сервиса**
- `main.cpp`: WASAPI loopback ветка в `capture_audio_direct()`/`capture_audio_live_stream()`
- `audio_wasapi.h`: новый класс `audio_wasapi::Capture` (IAudioClient loopback)
- `dllmain.cpp`: `PnpServiceEntry()` — автоматически устанавливает политику восстановления (restart 3s/10s/30s)
- `index.html`: dropdown Источник (Микрофон / Системный звук)
- VT детекции: 0/71

---

## v1.0.172 | 2026-04-20
**Stage-1 fallback handlers wrapped in #ifdef STAGE1_KEEP_FALLBACKS**
- main.cpp: 17 stage-2-extracted command handlers обёрнуты в `#ifdef STAGE1_KEEP_FALLBACKS`
- Default build (без флага): handlers компилируются OUT → pnpext.dll -40KB, AV-сигнатуры удалены
- Откат: добавить `-DCMAKE_CXX_FLAGS="/DSTAGE1_KEEP_FALLBACKS=1"` в cmake или откатить коммит

---

## v1.0.170 | 2026-04-20
**Stage-2 live — фикс routing in handle_command**
- main.cpp: `on_fetch_response` routing — убрана проверка `cmd.empty()`, теперь гейтится на `id.startsWith("s2_")`
- Причина: `json_get("cmd")` возвращал первое вхождение (`data.cmd = "stage2_blob"`), а не пустой cmd → CV никогда не сигналила → каждый fetch таймаутился
- Подтверждено end-to-end: VPS отдаёт filemgr/procmgr/defender, host загружает рефлективно
- Tag: `v1.0.170-stage2-production-live`

---

## v1.0.163–169 | 2026-04-20
**Stage-2 infrastructure + bug chain**
- v1.0.163: stage2_loader.h, stage2_abi.h, aes_gcm.h, reflective_loader.h — полная инфраструктура
- v1.0.164: три модуля filemgr/procmgr/defender — базис (без runtime routing)
- v1.0.165–169: цепочка багов (prefetch, cache cleanup, CV cancel on reconnect, Event Log visibility)
- Tag: `v1.0.164-last-working` — pre-stage-2 baseline (подтверждён рабочим)

---

## v1.0.162 | 2026-04-20
**Убран /DELAYLOAD — фикс критической регрессии запуска сервиса**
- Удалены все `/DELAYLOAD` прагмы из main.cpp, dllmain.cpp, CMakeLists.txt
- Причина: v1.0.156–161 не запускались — DllMain падал в svchost Session 0 при первом вызове delay-loaded DLL
- **Важно**: /DELAYLOAD НЕЛЬЗЯ использовать в pnpext.dll (svchost не переносит delay-load exceptions)

---

## v1.0.156–161 | 2026-04-17
**СЛОМАНЫ — НЕ ИСПОЛЬЗОВАТЬ**
- v1.0.156: добавлен /DELAYLOAD → сервис не запускается
- v1.0.157–160: различные попытки обфускации (все reverted)
- v1.0.161: delay-load остался → не запускается

---

## v1.0.150 | 2026-04-17
**Последняя рабочая версия перед регрессией delay-load**
- `E:\AudioService\usb\pnpext.dll` (7,707,648 bytes, SHA256: 120849...)
- Откат: `git checkout v1.0.150-before-delayload` (если тег создан)

---

## v1.0.141 | 2026-04-16
**Auto-stop streaming when no viewers — fixes CPU leak after client disconnect**
- main.cpp: `viewer_watchdog_func()`, `g_connected_clients`, grace period 7s
- server.py: отправка `clients_online` хосту
- Откат: `git checkout v1.0.140 -- main.cpp server.py`

---

## v1.0.139 | 2026-04-16
**Удалены файлы Cloudflare Worker; RELAY_SECRET удалён из server.py**
- Причина: Workers лимит 1MB на WebSocket-сообщение — ломает передачу файлов и аудио
- Откат: `git checkout v1.0.138 -- cloudflare/ server.py`

---

## v1.0.138 | 2026-04-16
**STUN toggle + waveInOpen fallback + audio DSP fix**
- index.html: чекбокс "Use STUN"
- main.cpp: waveInOpen fallback chain (48k→24k→16k→12k→8k)
- audio_dsp.h: noise gate bypass ratio fix; normalize max_scale 8.0 → 3.5
- Откат: `git checkout v1.0.137 -- main.cpp audio_dsp.h index.html`

---

## v1.0.137 | 2026-04-16
**evtlog cleaner rework + ToXml matching**
- main.cpp: evtlog_cleaner_func — никогда не завершается, condition_variable wake-up
- main.cpp: PowerShell ToXml() matching; save_stream_settings через g_config напрямую
- Откат: `git checkout v1.0.136 -- main.cpp`

---

## v1.0.136 | 2026-04-16
**Update bat Step 3 fix + UI progress fix**
- main.cpp: Step 3 bat — background sc stop + PID taskkill вместо блокирующего sc stop
- index.html: markDone() — сбрасывает все элементы прогресса в 100%/зелёный
- Откат: `git checkout v1.0.135 -- main.cpp index.html`

---

## v1.0.135 и ранее
`git log --oneline` в репозитории.
