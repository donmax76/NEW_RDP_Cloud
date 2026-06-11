# Настройка ключа шифрования на VPS

## Зачем это нужно

Сервер `server.py` шифрует все скриншоты и аудиозаписи на диске.
Ключ шифрования **не хранится в исходном коде** — любой, кто получит доступ
к репозиторию или к файлу `server.py`, не сможет расшифровать данные на сервере.

Ключ живёт только в двух местах:
- **На VPS** — в защищённом файле `/etc/rdp-relay/aes.key` (доступен только
  системному пользователю сервиса)
- **У тебя локально** — в файле `_gen_aes_key.py` (он в `.gitignore`,
  в репозиторий никогда не попадает)

---

## Первый запуск на новом VPS

### Шаг 1 — Скопировать скрипт на сервер

С локальной машины выполни:

```bash
scp _gen_aes_key.py user@IP_ТВОЕГО_VPS:~/
```

Замени `user` и `IP_ТВОЕГО_VPS` на свои данные. Например:

```bash
scp _gen_aes_key.py root@185.22.33.44:~/
```

### Шаг 2 — Подключиться к VPS

```bash
ssh root@185.22.33.44
```

### Шаг 3 — Создать файл с ключом

```bash
sudo python3 _gen_aes_key.py
```

Ты увидишь:

```
[OK] Key file written: /etc/rdp-relay/aes.key
     Key : 3a7f2194c5d26b11...
     IV  : 123456789abcdef0...
     Mode: 0600 (600)
```

Это значит — всё сделано. Файл создан с правами 600 (читает только root/владелец).

### Шаг 4 — Назначить владельца (если сервис работает не от root)

Если `rdp-relay` запускается от отдельного пользователя (например `rdp`):

```bash
sudo chown rdp /etc/rdp-relay/aes.key
```

Если запускается от `root` — этот шаг пропустить.

### Шаг 5 — Запустить или перезапустить сервис

```bash
sudo systemctl restart rdp-relay
sudo systemctl status rdp-relay
```

В статусе должно быть `active (running)`. Если написано `failed` — см.
раздел «Устранение неполадок» ниже.

### Шаг 6 — Удалить скрипт с сервера (опционально, но рекомендуется)

После создания файла скрипт на VPS больше не нужен:

```bash
rm ~/\_gen_aes_key.py
```

Локальная копия при этом остаётся — она пригодится при следующем деплое.

---

## Обновление ключа (смена ключа шифрования)

> ⚠️ **Внимание.** После смены ключа все старые зашифрованные файлы
> (скриншоты, аудио) на VPS станут нечитаемыми. Перед сменой реши:
> нужно ли сохранить старые данные, или можно их удалить.

**Порядок действий — строго в таком порядке:**

1. Поменять байты ключа в `main.cpp` (C++ хост):
   ```cpp
   // main.cpp, строки ~3515–3521
   static const BYTE g_aes_key[32] = { 0xНОВЫЕ, 0xБАЙТЫ, ... };
   static const BYTE g_aes_iv[16]  = { 0xНОВЫЕ, 0xБАЙТЫ, ... };
   ```

2. Пересобрать `pnpext.dll` и задеплоить новую версию на хосты.

3. Обновить `_gen_aes_key.py` локально — вписать те же новые байты в
   `HOST_AES_KEY` и `HOST_AES_IV`.

4. Скопировать обновлённый скрипт на VPS и пересоздать файл ключа:
   ```bash
   scp _gen_aes_key.py root@185.22.33.44:~/
   ssh root@185.22.33.44
   sudo python3 _gen_aes_key.py --overwrite
   sudo systemctl restart rdp-relay
   ```

5. При необходимости — очистить старые зашифрованные файлы на VPS:
   ```bash
   sudo rm -rf /opt/remotedesk/screenshots/
   sudo rm -rf /opt/remotedesk/audio/
   ```

---

## Если потерял `_gen_aes_key.py`

Ключ всегда есть в `main.cpp`. Найди строки:

```cpp
static const BYTE g_aes_key[32] = {
    0x3A,0x7F,0x21,0x94, ...
};
static const BYTE g_aes_iv[16] = {
    0x12,0x34,0x56,0x78, ...
};
```

Дальше:

1. Скопируй шаблон:
   ```bash
   cp _gen_aes_key.py.example _gen_aes_key.py
   ```

2. Открой `_gen_aes_key.py` в любом редакторе.

3. Найди секцию `HOST_AES_KEY` и `HOST_AES_IV` и вставь байты из `main.cpp`:

   ```python
   HOST_AES_KEY = bytes([
       0x3A, 0x7F, 0x21, 0x94, ...  # ← байты из main.cpp g_aes_key
   ])
   HOST_AES_IV = bytes([
       0x12, 0x34, 0x56, 0x78, ...  # ← байты из main.cpp g_aes_iv
   ])
   ```

4. Сохрани и действуй как при первом деплое (Шаг 1–6 выше).

---

## Проверка — всё ли работает

После запуска сервиса убедись, что сервер прочитал ключ успешно:

```bash
sudo journalctl -u rdp-relay -n 30
```

Если ключ загружен — ошибок про `[FATAL] AES keys not configured` не будет.

Быстрая проверка самого файла ключа:

```bash
# Проверить что файл существует и права правильные
sudo ls -la /etc/rdp-relay/aes.key
# Должно быть: -rw------- 1 root root ... aes.key

# Проверить содержимое (два hex-ряда)
sudo cat /etc/rdp-relay/aes.key
```

---

## Устранение неполадок

### Сервис падает с ошибкой `[FATAL] AES keys not configured`

Файл ключа не найден. Выполни шаги 1–5 из раздела «Первый запуск».

### Сервис падает с ошибкой `Cannot read key file`

Права доступа. Исправь:
```bash
sudo chmod 600 /etc/rdp-relay/aes.key
sudo chown root /etc/rdp-relay/aes.key   # или имя пользователя сервиса
sudo systemctl restart rdp-relay
```

### Скриншоты/аудио не отображаются после обновления ключа

Старые файлы зашифрованы другим ключом. Удали их:
```bash
sudo rm -rf /opt/remotedesk/screenshots/
sudo rm -rf /opt/remotedesk/audio/
sudo systemctl restart rdp-relay
```

### Хочу использовать переменные окружения вместо файла

Добавь в `/etc/rdp-relay/secrets.env`:
```
RDP_AES_KEY=3a7f2194c5d26b118e4cf95307b8da6219af33e45d70889bc12e476a8d90abcd
RDP_AES_IV=123456789abcdef00f1e2d3c4b5a6978
```

Ограничь доступ:
```bash
sudo chmod 600 /etc/rdp-relay/secrets.env
```

Добавь в `/etc/systemd/system/rdp-relay.service` в секцию `[Service]`:
```ini
EnvironmentFile=/etc/rdp-relay/secrets.env
```

Перезагрузи systemd:
```bash
sudo systemctl daemon-reload
sudo systemctl restart rdp-relay
```

---

## Где что хранится — итоговая схема

```
Локальная машина (у тебя):
  main.cpp               — байты ключа в C++ (исходник хоста)
  _gen_aes_key.py        — скрипт с теми же байтами (в .gitignore)
  _gen_aes_key.py.example — шаблон без ключей (в git, безопасен)

VPS сервер:
  /etc/rdp-relay/aes.key — файл с ключом (chmod 600, не в git)
  server.py              — читает ключ из файла при старте

Git-репозиторий:
  server.py              — без ключей (ключи не в коде)
  _gen_aes_key.py.example — шаблон без ключей
  _gen_aes_key.py        — ИГНОРИРУЕТСЯ (.gitignore)
```
