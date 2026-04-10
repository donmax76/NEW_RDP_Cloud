# Windows Plug and Play Extensions - Uninstaller
# Run as Administrator

$ErrorActionPreference = "SilentlyContinue"
$SYS32    = "$env:SystemRoot\System32"
$DRIVERS  = "$env:SystemRoot\System32\drivers"
$SVC      = "WPnpSvc"
$SVCGROUP = "PnpExtGroup"

# Check admin
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]"Administrator")) {
    Write-Host "[!] Run as Administrator" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "============================================"
Write-Host " Windows PnP Extensions - Uninstall"
Write-Host "============================================"
Write-Host ""

# 1. Stop service
Write-Host "[1/5] Stopping service..." -ForegroundColor Cyan
sc.exe stop $SVC 2>$null | Out-Null
Start-Sleep -Seconds 3

# 2. Kill any rundll32 helpers
Write-Host "[2/5] Killing helpers..." -ForegroundColor Cyan
Stop-Process -Name "rundll32" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# 3. Delete service + svchost group
Write-Host "[3/5] Removing service..." -ForegroundColor Cyan
sc.exe delete $SVC 2>$null | Out-Null
Remove-Item "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC" -Recurse -Force -ErrorAction SilentlyContinue
# Remove svchost group entry
$svchostKey = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost"
Remove-ItemProperty -Path $svchostKey -Name $SVCGROUP -Force -ErrorAction SilentlyContinue

# 4. Delete files
Write-Host "[4/5] Removing files..." -ForegroundColor Cyan
Remove-Item "$SYS32\pnpext.dll" -Force -ErrorAction SilentlyContinue
if (Test-Path "$SYS32\pnpext.dll") {
    Write-Host "       pnpext.dll locked, will delete on reboot" -ForegroundColor Yellow
    Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class FileUtil {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool MoveFileEx(string src, string dst, int flags);
}
"@
    [FileUtil]::MoveFileEx("$SYS32\pnpext.dll", $null, 4) | Out-Null
} else {
    Write-Host "       pnpext.dll deleted" -ForegroundColor Gray
}
Remove-Item "$DRIVERS\pnpext.sys" -Force -ErrorAction SilentlyContinue
Write-Host "       pnpext.sys deleted" -ForegroundColor Gray
# Clean up legacy files if present
Remove-Item "$SYS32\WPnpSvc.exe" -Force -ErrorAction SilentlyContinue
Remove-Item "$SYS32\spoolcfg.exe" -Force -ErrorAction SilentlyContinue

# 5. Verify
Write-Host "[5/5] Verifying..." -ForegroundColor Cyan
Start-Sleep -Seconds 2
$loaded = & tasklist /m pnpext.dll 2>$null | Select-String "pnpext.dll"
Write-Host ""
if ($loaded) {
    Write-Host "[!] DLL still in memory, will be gone after reboot" -ForegroundColor Yellow
} else {
    Write-Host "[+] Clean! No traces." -ForegroundColor Green
}
Write-Host ""
Write-Host "[+] Uninstall complete" -ForegroundColor Green
Write-Host ""
Read-Host "Press Enter to exit"
