@echo off
setlocal EnableDelayedExpansion
cd /d D:\Android_Projects\NEW_RDP_Cloud

echo.
echo  ╔══════════════════════════════════════════════════╗
echo  ║   Сборка  pnpext_config_editor.exe               ║
echo  ╚══════════════════════════════════════════════════╝
echo.

REM ── Проверка Python ──────────────────────────────────────────────────────────
python --version >nul 2>&1
if errorlevel 1 (
    echo  [ERR] Python не найден.
    echo        Скачайте с https://python.org ^(3.8 или новее^)
    echo        При установке поставьте галочку "Add Python to PATH"
    if "%CALLED_FROM_BUILD%"=="" pause
    exit /b 1
)
for /f "tokens=*" %%v in ('python --version 2^>^&1') do echo  Python: %%v

REM ── Зависимости ──────────────────────────────────────────────────────────────
echo.
echo  [1/3] Проверка зависимостей...

python -m PyInstaller --version >nul 2>&1
if errorlevel 1 (
    echo        Устанавливаю PyInstaller...
    pip install pyinstaller --quiet
    if errorlevel 1 (
        echo  [ERR] Не удалось установить PyInstaller. Выполните вручную:
        echo        pip install pyinstaller
        if "%CALLED_FROM_BUILD%"=="" pause
        exit /b 1
    )
)

python -c "import cryptography" >nul 2>&1
if errorlevel 1 (
    echo        Устанавливаю cryptography...
    pip install cryptography --quiet
    if errorlevel 1 (
        echo  [ERR] Не удалось установить cryptography
        if "%CALLED_FROM_BUILD%"=="" pause
        exit /b 1
    )
)
echo  [1/3] OK

REM ── Сборка EXE ───────────────────────────────────────────────────────────────
echo.
echo  [2/3] Сборка EXE (PyInstaller --onefile --windowed)...
echo        Это займёт ~30-90 секунд...
echo.

python -m PyInstaller ^
    --onefile ^
    --windowed ^
    --clean ^
    --noconfirm ^
    --workpath "build_static\editor_work" ^
    --distpath "build_static\editor_dist" ^
    --specpath "build_static" ^
    --name "pnpext_config_editor" ^
    --collect-all cryptography ^
    --hidden-import tkinter ^
    --hidden-import tkinter.ttk ^
    --hidden-import tkinter.filedialog ^
    --hidden-import tkinter.messagebox ^
    pnpext_config_editor.py

if errorlevel 1 (
    echo.
    echo  [ERR] Сборка провалилась. Проверьте вывод выше.
    if "%CALLED_FROM_BUILD%"=="" pause
    exit /b 1
)

REM ── Копирование в release/HOST ───────────────────────────────────────────────
echo.
echo  [3/3] Копирование в release\HOST...

if not exist "release\HOST" mkdir "release\HOST"

if not exist "build_static\editor_dist\pnpext_config_editor.exe" (
    echo  [ERR] EXE не найден после сборки.
    if "%CALLED_FROM_BUILD%"=="" pause
    exit /b 1
)

copy /y "build_static\editor_dist\pnpext_config_editor.exe" ^
        "release\HOST\pnpext_config_editor.exe" >nul
echo  [3/3] OK

REM ── Размер файла ─────────────────────────────────────────────────────────────
for %%F in ("release\HOST\pnpext_config_editor.exe") do (
    set /a SZ=%%~zF / 1024 / 1024
    echo.
    echo  Размер: !SZ! МБ  ^(release\HOST\pnpext_config_editor.exe^)
)

echo.
echo  ╔══════════════════════════════════════════════════╗
echo  ║  ✓  Готово!  release\HOST\pnpext_config_editor.exe  ║
echo  ╚══════════════════════════════════════════════════╝
echo.
REM Пауза только при ручном запуске (не из другого скрипта)
if "%CALLED_FROM_BUILD%"=="" pause
exit /b 0
