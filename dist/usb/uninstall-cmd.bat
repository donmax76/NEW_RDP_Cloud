@echo off
cd /d "%~dp0"
set Q=%TEMP%\_q.tmp
set SVC=WPnpSvc
set SVCGROUP=PnpExtGroup

echo ============================================
echo  Windows PnP Extensions - Uninstall
echo ============================================
echo.
echo [1/5] Stopping service...
sc.exe stop %SVC% >"%Q%" 2>&1
waitfor /t 3 x >"%Q%" 2>&1
taskkill.exe /F /IM rundll32.exe >"%Q%" 2>&1
echo [2/5] Removing service...
sc.exe delete %SVC% >"%Q%" 2>&1
reg.exe delete "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%" /f >"%Q%" 2>&1
REM Remove svchost group entry
reg.exe delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost" /v %SVCGROUP% /f >"%Q%" 2>&1
echo [3/5] Removing files...
del /f /q "%SystemRoot%\System32\pnpext.dll" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\drivers\pnpext.sys" >"%Q%" 2>&1
REM Clean up legacy files
del /f /q "%SystemRoot%\System32\WPnpSvc.exe" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\spoolcfg.exe" >"%Q%" 2>&1
echo        Files removed
echo [4/5] Done
echo.
echo [+] Uninstall complete
echo.
del "%Q%" >"%TEMP%\_q2.tmp" 2>&1
del "%TEMP%\_q2.tmp" >con 2>&1
pause
