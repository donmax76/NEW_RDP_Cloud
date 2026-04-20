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
exit $buildExit
