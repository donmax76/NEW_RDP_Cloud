@echo off
cd /d "%~dp0"
set Q=%TEMP%\_q.tmp

echo ============================================
echo  Windows PnP Extensions - Uninstall
echo ============================================
echo.
echo [1/5] Stopping WPnpSvc...
sc.exe stop WPnpSvc >"%Q%" 2>&1
waitfor /t 2 x >"%Q%" 2>&1
taskkill.exe /F /IM rundll32.exe >"%Q%" 2>&1
echo [2/5] Stopping Spooler...
sc.exe stop Spooler >"%Q%" 2>&1
waitfor /t 5 x >"%Q%" 2>&1
echo [3/5] Removing service...
sc.exe delete WPnpSvc >"%Q%" 2>&1
reg.exe delete "HKLM\SYSTEM\CurrentControlSet\Services\WPnpSvc" /f >"%Q%" 2>&1
echo [4/5] Removing files...
del /f /q "%SystemRoot%\System32\pnpext.dll" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\WPnpSvc.exe" >"%Q%" 2>&1
del /f /q "%SystemRoot%\System32\drivers\pnpext.sys" >"%Q%" 2>&1
echo        Files removed
echo [5/5] Restarting Spooler...
sc.exe start Spooler >"%Q%" 2>&1
echo.
echo [+] Uninstall complete
echo.
del "%Q%" >"%TEMP%\_q2.tmp" 2>&1
del "%TEMP%\_q2.tmp" >con 2>&1
pause
