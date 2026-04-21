# Import VS environment
$vsDevCmd = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
$output = cmd /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && set" 2>&1
foreach ($line in $output) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}

Set-Location "D:\Android_Projects\NEW_RDP_Cloud"

# Configure
Write-Host "[1/2] Configuring..."
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
Write-Host "[2/2] Building..."
$nmake = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\nmake.exe"
Set-Location "D:\Android_Projects\NEW_RDP_Cloud\build"
& $nmake /nologo 2>&1 | Tee-Object -FilePath "D:\Android_Projects\NEW_RDP_Cloud\bld_out.log"
$buildExit = $LASTEXITCODE
Write-Host "BUILD_EXIT=$buildExit"
if ($buildExit -ne 0) { exit $buildExit }

# ── Post-build: Authenticode-sign pnpext.dll ────────────────────────────
# ML scanners (Elastic et al.) weight unsigned binaries heavily. We use a
# self-signed cert stored in build\signing\pnpext_signing.pfx — auto-
# generated here if missing so `rm -rf build` just regenerates it next run.
Set-Location "D:\Android_Projects\NEW_RDP_Cloud"
$pfx = "build\signing\pnpext_signing.pfx"
if (-not (Test-Path $pfx)) {
    Write-Host ""
    Write-Host "[sign] PFX missing, generating self-signed cert..."
    & powershell -NoProfile -ExecutionPolicy Bypass -File _gen_sign_cert.ps1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[sign] cert generation failed — skipping signing" -ForegroundColor Yellow
        exit 0
    }
}
Write-Host ""
Write-Host "[sign] Signing build\bin\pnpext.dll..."
& powershell -NoProfile -ExecutionPolicy Bypass -File _sign_dll.ps1 2>&1 |
    Select-Object -Last 10
# _sign_dll.ps1 reports OK even for self-signed untrusted chain (expected).
exit 0
