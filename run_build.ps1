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
exit 0
