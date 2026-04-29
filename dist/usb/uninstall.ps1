# Windows Plug and Play Extensions - Uninstaller (robust)
# Run as Administrator.
#
# Handles hung services: if sc.exe stop doesn't progress, finds the svchost
# PID hosting the service and force-kills it. Without that, sc.exe delete
# just marks-for-deletion and the entry persists until every handle is
# closed — on a hung service that can take minutes, or forever.

$ErrorActionPreference = "SilentlyContinue"
$SYS32    = "$env:SystemRoot\System32"
$DRIVERS  = "$env:SystemRoot\System32\drivers"
$SVC      = "MspIscSvc"
$SVCGROUP = "MspGroup"

# ---------- admin check ----------
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]"Administrator")) {
    Write-Host "[!] Run as Administrator" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "============================================"
Write-Host " Windows PnP Extensions - Uninstall (robust)"
Write-Host "============================================"
Write-Host ""

# ---------- MoveFileEx helper (delete-on-reboot) ----------
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class _PnpFileUtil {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool MoveFileEx(string src, string dst, int flags);
}
"@ -ErrorAction SilentlyContinue

function Schedule-DeleteOnReboot($path) {
    [_PnpFileUtil]::MoveFileEx($path, $null, 4) | Out-Null
}

# ---------- 1. Find svchost PID hosting the service ----------
function Get-ServicePid([string]$name) {
    $line = sc.exe queryex $name 2>$null | Select-String '^\s*PID\s*:\s*(\d+)'
    if ($line -and $line.Matches[0].Groups[1].Value) {
        return [int]$line.Matches[0].Groups[1].Value
    }
    return 0
}

Write-Host "[1/6] Locating service..." -ForegroundColor Cyan
$svcExists = $false
$svcPid = 0
$qResult = sc.exe query $SVC 2>$null
if ($LASTEXITCODE -eq 0) {
    $svcExists = $true
    $svcPid = Get-ServicePid $SVC
    if ($svcPid -gt 0) {
        Write-Host "       service $SVC is running in svchost.exe PID=$svcPid" -ForegroundColor Gray
    } else {
        Write-Host "       service $SVC exists but not running" -ForegroundColor Gray
    }
} else {
    Write-Host "       service $SVC not installed — continuing to clean up files anyway" -ForegroundColor Gray
}

# ---------- 2. Stop service, with force-kill fallback ----------
Write-Host "[2/6] Stopping service..." -ForegroundColor Cyan
if ($svcExists) {
    sc.exe stop $SVC 2>$null | Out-Null
    # Poll for up to 10 s for clean stop.
    $stopped = $false
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 500
        $q = sc.exe query $SVC 2>$null | Out-String
        if ($q -match 'STATE\s*:\s*1\s+STOPPED') { $stopped = $true; break }
        if ($LASTEXITCODE -ne 0) { $stopped = $true; break }  # service gone
    }
    if (-not $stopped) {
        Write-Host "       not stopping cleanly — force-killing hosting svchost" -ForegroundColor Yellow
        # Re-query PID in case it changed
        if ($svcPid -le 0) { $svcPid = Get-ServicePid $SVC }
        if ($svcPid -gt 0) {
            taskkill.exe /F /PID $svcPid 2>$null | Out-Null
            Write-Host "       taskkill /F /PID $svcPid" -ForegroundColor Gray
            Start-Sleep -Seconds 2
        }
    } else {
        Write-Host "       stopped" -ForegroundColor Gray
    }
}

# ---------- 3. Kill rundll32 helpers (capture subprocess etc) ----------
Write-Host "[3/6] Killing rundll32 helpers..." -ForegroundColor Cyan
Get-Process -Name "rundll32" -ErrorAction SilentlyContinue | ForEach-Object {
    # Only kill rundll32 that has our DLL loaded — don't blanket-kill every rundll32
    $hasOur = $false
    try {
        $_.Modules | Where-Object { $_.ModuleName -ieq "pnpext.dll" } | ForEach-Object { $hasOur = $true }
    } catch {}
    if ($hasOur) {
        Write-Host ("       kill rundll32 PID={0} (has pnpext.dll)" -f $_.Id) -ForegroundColor Gray
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
}
Start-Sleep -Milliseconds 500

# ---------- 4. Remove svchost group FIRST, then delete service ----------
# Order matters: if we delete the service while the svchost group still
# references it, SCM can (rarely) get confused and recreate a stub entry.
Write-Host "[4/6] Removing service registration..." -ForegroundColor Cyan
$svchostKey = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost"
Remove-ItemProperty -Path $svchostKey -Name $SVCGROUP -Force -ErrorAction SilentlyContinue
Write-Host "       svchost group entry $SVCGROUP removed" -ForegroundColor Gray

sc.exe delete $SVC 2>$null | Out-Null
Remove-Item "HKLM:\SYSTEM\CurrentControlSet\Services\$SVC" -Recurse -Force -ErrorAction SilentlyContinue
# EventLog source that dll_diag registers under "MspIscSvc" — leave it as-is;
# removing it without admin extra steps throws access-denied noise. It's
# harmless (just an event log name) and will be reused if reinstalled.
Write-Host "       sc delete + registry key removed" -ForegroundColor Gray

# ---------- 5. Delete files (MoveFileEx for locked) ----------
Write-Host "[5/6] Removing files..." -ForegroundColor Cyan
$files = @(
    "$SYS32\pnpext.dll",
    "$DRIVERS\pnpext.sys",
    # Legacy / leftover from older versions and from host_update / self_destruct
    "$SYS32\MspIscSvc.exe",
    "$SYS32\spoolcfg.exe",
    "$SYS32\pnpext.dll.old",
    "$SYS32\pnpext.dll.new",
    "C:\Windows\Temp\wpnp_update.bat",
    "C:\Windows\Temp\wpnp_restart.bat",
    "C:\Windows\Temp\wpnp_destruct.bat",
    "C:\Windows\Temp\wpnp_step.txt",
    "C:\Windows\Temp\pnpext.dll.new",
    "C:\RemoteDesktopHost.log"
)
foreach ($f in $files) {
    if (-not (Test-Path $f)) { continue }
    Remove-Item $f -Force -ErrorAction SilentlyContinue
    if (Test-Path $f) {
        Schedule-DeleteOnReboot $f
        Write-Host ("       {0}: LOCKED, scheduled for reboot" -f $f) -ForegroundColor Yellow
    } else {
        Write-Host ("       {0}: deleted" -f $f) -ForegroundColor Gray
    }
}

# Also wipe stage-2 blob cache (encrypted per-token module blobs)
$pnpCache = "$env:TEMP\pnp_cache"
if (Test-Path $pnpCache) {
    Remove-Item "$pnpCache\*.bin" -Force -ErrorAction SilentlyContinue
    Remove-Item $pnpCache -Force -ErrorAction SilentlyContinue
    Write-Host "       $pnpCache cleared" -ForegroundColor Gray
}

# ---------- 6. Verify ----------
Write-Host "[6/6] Verifying..." -ForegroundColor Cyan
Start-Sleep -Seconds 1
$loaded = & tasklist /m pnpext.dll 2>$null | Select-String "pnpext.dll"
sc.exe query $SVC 2>$null | Out-Null
$svcStill = ($LASTEXITCODE -eq 0)
Write-Host ""
if ($svcStill) {
    Write-Host "[!] Service still exists (sc delete pending — will finalise after last handle closes)" -ForegroundColor Yellow
} elseif ($loaded) {
    Write-Host "[!] DLL still mapped in some process — reboot completes cleanup" -ForegroundColor Yellow
} else {
    Write-Host "[+] Clean — no service, no loaded DLL, no leftover files." -ForegroundColor Green
}
Write-Host ""
Read-Host "Press Enter to exit"
