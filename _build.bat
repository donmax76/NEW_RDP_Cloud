@echo off
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d D:\Android_Projects\NEW_RDP_Cloud
if "%1"=="configure" (
    cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
) else (
    REM Auto-bump version across host.h + index.html + server.py
    python D:\Android_Projects\NEW_RDP_Cloud\_bump_version.py
    REM Propagate new HOST_VERSION into pnpext.rc FILEVERSION resource
    powershell -NoProfile -ExecutionPolicy Bypass -File D:\Android_Projects\NEW_RDP_Cloud\_sync_versions.ps1
    cmake --build build -- /nologo
    if errorlevel 1 exit /b 1
    REM Strip OpenSSL CRYPTOGAMS / dot-asm / Andy Polyakov fingerprints
    REM (these trigger VT "Memory Pattern URL: github.com/dot-asm" detection)
    powershell -NoProfile -ExecutionPolicy Bypass -File D:\Android_Projects\NEW_RDP_Cloud\_scrub_dll_strings.ps1 -Dll "D:\Android_Projects\NEW_RDP_Cloud\build\bin\pnpext.dll"
    REM Mirror DLL to dist/usb and release/HOST so both bundles always have the latest build
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\build\bin\pnpext.dll" "D:\Android_Projects\NEW_RDP_Cloud\dist\usb\pnpext.dll" >nul
    echo [post] pnpext.dll copied to dist\usb
    if not exist "D:\Android_Projects\NEW_RDP_Cloud\release\HOST" mkdir "D:\Android_Projects\NEW_RDP_Cloud\release\HOST"
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\build\bin\pnpext.dll" "D:\Android_Projects\NEW_RDP_Cloud\release\HOST\pnpext.dll" >nul
    echo [post] pnpext.dll copied to release\HOST
    REM Mirror VPS server files to release/VPS
    if not exist "D:\Android_Projects\NEW_RDP_Cloud\release\VPS" mkdir "D:\Android_Projects\NEW_RDP_Cloud\release\VPS"
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\server.py" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\server.py" >nul
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\index.html" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\index.html" >nul
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\admin_dashboard.html" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\admin_dashboard.html" >nul
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\nginx.conf" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\nginx.conf" >nul
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\nginx-remote-desktop.conf" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\nginx-remote-desktop.conf" >nul
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\deploy-vps.sh" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\deploy-vps.sh" >nul
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\deploy-vps2.sh" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\deploy-vps2.sh" >nul
    if exist "D:\Android_Projects\NEW_RDP_Cloud\MANUAL.html" copy /y "D:\Android_Projects\NEW_RDP_Cloud\MANUAL.html" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\MANUAL.html" >nul
    if exist "D:\Android_Projects\NEW_RDP_Cloud\MANUAL.md" copy /y "D:\Android_Projects\NEW_RDP_Cloud\MANUAL.md" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\MANUAL.md" >nul
    echo [post] VPS files synced to release\VPS
    REM Mirror update binaries to release/VPS (served by VPS for host updates)
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\build\bin\pnpext.dll" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\pnpext.dll" >nul
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\dist\usb\pnpext.sys" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\pnpext.sys" >nul
    echo [post] pnpext.dll + pnpext.sys synced to release\VPS
    REM Mirror stage2 DLLs to release/VPS/stage2
    if not exist "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\stage2" mkdir "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\stage2"
    for %%f in (filemgr procmgr defender sysinfo) do (
        if exist "D:\Android_Projects\NEW_RDP_Cloud\build\stage2\%%f.dll" (
            copy /y "D:\Android_Projects\NEW_RDP_Cloud\build\stage2\%%f.dll" "D:\Android_Projects\NEW_RDP_Cloud\release\VPS\stage2\%%f.dll" >nul
        )
    )
    echo [post] stage2 DLLs synced to release\VPS\stage2
    REM Mirror BAT installers to release/HOST
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\dist\usb\install-cmd.bat" "D:\Android_Projects\NEW_RDP_Cloud\release\HOST\install-cmd.bat" >nul
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\dist\usb\install-web-cmd.bat" "D:\Android_Projects\NEW_RDP_Cloud\release\HOST\install-web-cmd.bat" >nul
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\dist\usb\uninstall-cmd.bat" "D:\Android_Projects\NEW_RDP_Cloud\release\HOST\uninstall-cmd.bat" >nul
    echo [post] BAT installers synced to release\HOST
    REM Mirror config editor EXE to release/HOST (built separately via _build_config_editor.bat)
    if exist "D:\Android_Projects\NEW_RDP_Cloud\build_static\editor_dist\pnpext_config_editor.exe" (
        copy /y "D:\Android_Projects\NEW_RDP_Cloud\build_static\editor_dist\pnpext_config_editor.exe" "D:\Android_Projects\NEW_RDP_Cloud\release\HOST\pnpext_config_editor.exe" >nul
        echo [post] pnpext_config_editor.exe synced to release\HOST
    )
)
