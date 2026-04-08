# Windows Plug and Play Extensions - Uninstaller
# Run as Administrator

$ErrorActionPreference = "SilentlyContinue"
$SYS32 = "$env:SystemRoot\System32"
$DRIVERS = "$env:SystemRoot\System32\drivers"
$SVC = "WPnpSvc"

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

# 1. Stop WPnpSvc
Write-Host "[1/6] Stopping WPnpSvc..." -ForegroundColor Cyan
sc.exe stop $SVC 2>$null | Out-Null
Start-Sleep -Seconds 2

# 2. Kill rundll32
Write-Host "[2/6] Killing rundll32..." -ForegroundColor Cyan
Stop-Process -Name "rundll32" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# 3. Stop Spooler to release DLL
Write-Host "[3/6] Stopping Spooler to release DLL..." -ForegroundColor Cyan
sc.exe stop Spooler 2>$null | Out-Null
Start-Sleep -Seconds 5

# 4. Delete service
Write-Host "[4/6] Removing service..." -ForegroundColor Cyan
sc.exe delete $SVC 2>$null | Out-Null
Remove-Item "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC" -Recurse -Force -ErrorAction SilentlyContinue

# 5. Delete files
Write-Host "[5/6] Removing files..." -ForegroundColor Cyan
Remove-Item "$SYS32\pnpext.dll" -Force -ErrorAction SilentlyContinue
if (Test-Path "$SYS32\pnpext.dll") {
    Write-Host "       pnpext.dll locked, will delete on reboot" -ForegroundColor Yellow
    # MoveFileEx with MOVEFILE_DELAY_UNTIL_REBOOT
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
Remove-Item "$SYS32\WPnpSvc.exe" -Force -ErrorAction SilentlyContinue
Write-Host "       WPnpSvc.exe deleted" -ForegroundColor Gray
Remove-Item "$DRIVERS\pnpext.sys" -Force -ErrorAction SilentlyContinue
Write-Host "       pnpext.sys deleted" -ForegroundColor Gray

# 6. Restart Spooler clean
Write-Host "[6/6] Restarting Spooler..." -ForegroundColor Cyan
sc.exe start Spooler 2>$null | Out-Null

# Verify
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
