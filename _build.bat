@echo off
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d D:\Android_Projects\NEW_RDP_Cloud
if "%1"=="configure" (
    cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
) else (
    cmake --build build -- /nologo
    if errorlevel 1 exit /b 1
    REM Mirror DLL to dist/usb so installer bundle always has the latest build
    copy /y "D:\Android_Projects\NEW_RDP_Cloud\build\bin\pnpext.dll" "D:\Android_Projects\NEW_RDP_Cloud\dist\usb\pnpext.dll" >nul
    echo [post] pnpext.dll copied to dist\usb
)
