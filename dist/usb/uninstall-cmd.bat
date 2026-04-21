@echo off
rem Windows Plug and Play Extensions - Uninstall (robust, bat-only, no PS)
rem Fixes the "service hangs on stop" problem: if STOP doesn't progress,
rem we look up the hosting svchost PID and taskkill /F it. Only then
rem does sc delete actually finalise.

cd /d "%~dp0"
setlocal EnableDelayedExpansion
set Q=%TEMP%\_q.tmp
set SVC=WPnpSvc
set SVCGROUP=PnpExtGroup
set SYS32=%SystemRoot%\System32
set DRV=%SystemRoot%\System32\drivers

echo ============================================
echo  Windows PnP Extensions - Uninstall (robust)
echo ============================================
echo.

rem ---------- 1. Find hosting svchost PID ----------
echo [1/5] Locating service...
set HOST_PID=
for /f "tokens=3" %%P in ('sc queryex %SVC% 2^>nul ^| findstr /i "PID"') do set HOST_PID=%%P
if defined HOST_PID (
    echo        %SVC% running in svchost PID=!HOST_PID!
) else (
    echo        %SVC% not running or not installed
)

rem ---------- 2. Try clean stop, fall back to taskkill ----------
echo [2/5] Stopping service...
sc.exe stop %SVC% >"%Q%" 2>&1
rem Poll up to 5 seconds for clean stop.
set STOPPED=0
for /l %%i in (1,1,10) do (
    if "!STOPPED!"=="0" (
        sc.exe query %SVC% 2>nul | findstr /C:"STOPPED" >nul && set STOPPED=1
        if "!STOPPED!"=="0" (
            rem Also treat "service does not exist" as stopped
            sc.exe query %SVC% 2>nul >nul || set STOPPED=1
        )
        if "!STOPPED!"=="0" (>nul timeout /t 1 /nobreak)
    )
)
if "!STOPPED!"=="0" (
    echo        not stopping cleanly — force-killing svchost
    if defined HOST_PID (
        taskkill.exe /F /PID !HOST_PID! >"%Q%" 2>&1
        echo        taskkill /F /PID !HOST_PID!
        >nul timeout /t 2 /nobreak
    )
) else (
    echo        stopped
)

rem Kill any lingering rundll32 helpers (capture / screenshot / audio)
taskkill.exe /F /IM rundll32.exe >"%Q%" 2>&1

rem ---------- 3. Remove svchost group + delete service ----------
echo [3/5] Removing service registration...
reg.exe delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost" /v %SVCGROUP% /f >"%Q%" 2>&1
sc.exe delete %SVC% >"%Q%" 2>&1
reg.exe delete "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%" /f >"%Q%" 2>&1
echo        service + registry keys removed

rem ---------- 4. Delete files (with MoveFileEx fallback via PowerShell) ----------
echo [4/5] Removing files...
call :tryDelete "%SYS32%\pnpext.dll"
call :tryDelete "%DRV%\pnpext.sys"
del /f /q "%SYS32%\WPnpSvc.exe"      >"%Q%" 2>&1
del /f /q "%SYS32%\spoolcfg.exe"     >"%Q%" 2>&1
del /f /q "%SYS32%\pnpext.dll.old"   >"%Q%" 2>&1
del /f /q "%SYS32%\pnpext.dll.new"   >"%Q%" 2>&1
del /f /q "%SystemRoot%\Temp\wpnp_update.bat"   >"%Q%" 2>&1
del /f /q "%SystemRoot%\Temp\wpnp_restart.bat"  >"%Q%" 2>&1
del /f /q "%SystemRoot%\Temp\wpnp_destruct.bat" >"%Q%" 2>&1
del /f /q "%SystemRoot%\Temp\wpnp_step.txt"     >"%Q%" 2>&1
del /f /q "%SystemRoot%\Temp\pnpext.dll.new"    >"%Q%" 2>&1
del /f /q "C:\RemoteDesktopHost.log"            >"%Q%" 2>&1
rem Stage-2 blob cache (encrypted per-token module blobs)
if exist "%TEMP%\pnp_cache" (
    del /f /q "%TEMP%\pnp_cache\*.bin" >"%Q%" 2>&1
    rmdir /q "%TEMP%\pnp_cache"        >"%Q%" 2>&1
)

rem ---------- 5. Verify ----------
echo [5/5] Verifying...
>nul timeout /t 1 /nobreak
sc.exe query %SVC% >nul 2>&1
if %errorlevel%==0 (
    echo.
    echo [!] Service still exists (sc delete pending until last handle closes^).
    echo     Reboot will finalise it.
) else (
    if exist "%SYS32%\pnpext.dll" (
        echo.
        echo [!] pnpext.dll still present ^(LOCKED^) — reboot to complete cleanup.
    ) else (
        echo.
        echo [+] Clean — no service, no DLL.
    )
)
echo.
del /f /q "%Q%" >nul 2>&1
pause
endlocal
exit /b 0

rem ---------- helper: delete file, fall back to MoveFileEx-on-reboot via PS ----------
:tryDelete
set FILE=%~1
if not exist "!FILE!" goto :eof
del /f /q "!FILE!" >"%Q%" 2>&1
if exist "!FILE!" (
    echo        !FILE!: LOCKED, scheduling delete-on-reboot
    rem Inline PowerShell call — we only pay the PS cost when the file is locked.
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
      "Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public class U{[DllImport(\"kernel32.dll\",SetLastError=true,CharSet=CharSet.Unicode)]public static extern bool MoveFileEx(string s,string d,int f);}';[U]::MoveFileEx('!FILE!',$null,4) | Out-Null" >"%Q%" 2>&1
) else (
    echo        !FILE!: deleted
)
goto :eof
