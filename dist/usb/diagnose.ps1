# Diagnose MspIscSvc startup failure — v1.0.163 writes to Event Log.
# Run as Administrator on the TARGET machine (not the dev box).
# Output is self-contained; copy-paste it back to the dev chat.

$ErrorActionPreference = 'Continue'
$delim = '=' * 72
Write-Host $delim
Write-Host "MspIscSvc Startup Diagnosis"
Write-Host "Date: $(Get-Date)"
Write-Host "Machine: $env:COMPUTERNAME"
Write-Host $delim

# ── 1. Service status ──────────────────────────────────────────────────
Write-Host "`n[1] Service status"
try {
    sc.exe query MspIscSvc 2>&1
} catch { Write-Host "  ERROR: $_" }

Write-Host "`n[1b] Service config"
try {
    sc.exe qc MspIscSvc 2>&1
} catch {}

Write-Host "`n[1c] ServiceDll + ServiceMain registry"
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\MspIscSvc\Parameters'
if (Test-Path $params) {
    Get-ItemProperty -Path $params | Format-List ServiceDll, ServiceMain
} else {
    Write-Host "  params key MISSING"
}

# ── 2. Installed DLL info ──────────────────────────────────────────────
Write-Host "`n[2] Installed pnpext.dll"
$dll = "$env:SystemRoot\System32\pnpext.dll"
if (Test-Path $dll) {
    $i = Get-Item $dll
    Write-Host ("  Size: {0:N0} bytes" -f $i.Length)
    Write-Host ("  Date: {0}" -f $i.LastWriteTime)
    $v = (Get-Item $dll).VersionInfo
    Write-Host ("  Version: {0}" -f $v.FileVersion)
    # MD5 to confirm it's the right build
    $md5 = (Get-FileHash $dll -Algorithm MD5).Hash
    Write-Host ("  MD5: {0}" -f $md5)
} else {
    Write-Host "  DLL NOT INSTALLED AT $dll"
}

# ── 3. Event Log: MspIscSvc source (our own dll_diag) ────────────────────
Write-Host "`n[3] Event Log — source=MspIscSvc (our dll_diag output)"
Write-Host "    -> this shows EXACTLY where startup dies"
try {
    $events = Get-WinEvent -FilterHashtable @{
        LogName='Application'; ProviderName='MspIscSvc'
    } -MaxEvents 30 -ErrorAction Stop | Sort-Object TimeCreated
    if (-not $events) {
        Write-Host "  (no events — service may not have been started yet, or event source not registered)"
    } else {
        foreach ($e in $events) {
            Write-Host ("  {0}  L{1}  {2}" -f $e.TimeCreated.ToString('HH:mm:ss'), $e.LevelDisplayName[0], $e.Message)
        }
    }
} catch {
    Write-Host "  No events found: $($_.Exception.Message)"
}

# ── 4. Event Log: Service Control Manager errors ───────────────────────
Write-Host "`n[4] Event Log — Service Control Manager (what SCM reported)"
try {
    $scmEvents = Get-WinEvent -FilterHashtable @{
        LogName='System'; ProviderName='Service Control Manager'
    } -MaxEvents 50 -ErrorAction Stop |
        Where-Object { $_.Message -match 'MspIscSvc|pnpext|PnpExt|PnP Extension' } |
        Sort-Object TimeCreated -Descending |
        Select-Object -First 10
    if (-not $scmEvents) {
        Write-Host "  (no recent SCM events mentioning MspIscSvc)"
    } else {
        foreach ($e in $scmEvents) {
            Write-Host ("  {0}  ID={1}  {2}" -f $e.TimeCreated.ToString('HH:mm:ss'), $e.Id, ($e.Message -replace "`r?`n", ' | '))
        }
    }
} catch {
    Write-Host "  Error reading System log: $($_.Exception.Message)"
}

# ── 5. Last svchost crash (Application Error) ──────────────────────────
Write-Host "`n[5] Application Error events mentioning svchost/pnpext (last hour)"
try {
    $since = (Get-Date).AddHours(-1)
    $crash = Get-WinEvent -FilterHashtable @{
        LogName='Application'; ProviderName='Application Error'; StartTime=$since
    } -ErrorAction Stop |
        Where-Object { $_.Message -match 'pnpext|svchost' } |
        Select-Object -First 5
    if (-not $crash) { Write-Host "  (none)" }
    else {
        foreach ($e in $crash) {
            Write-Host ("  {0}  {1}" -f $e.TimeCreated.ToString('HH:mm:ss'), ($e.Message -replace "`r?`n", ' | ').Substring(0, [Math]::Min(400, $e.Message.Length)))
        }
    }
} catch { Write-Host "  (skipped)" }

Write-Host "`n$delim"
Write-Host "Done. Copy everything above back to the dev chat."
Write-Host $delim
Read-Host "Press Enter to close"
