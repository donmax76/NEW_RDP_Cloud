param([string]$Dll = "build\bin\pnpext.dll")

if (-not (Test-Path $Dll)) {
    Write-Host "ERROR: $Dll not found. Build first." -ForegroundColor Red
    exit 1
}

$bytes   = [System.IO.File]::ReadAllBytes($Dll)
$ascii   = [System.Text.Encoding]::ASCII.GetString($bytes)
$unicode = [System.Text.Encoding]::Unicode.GetString($bytes)

$patterns = @(
    'MpPreference',
    'Set-MpPreference',
    'DisableRealtimeMonitoring',
    'DisableAntiSpyware',
    'DisableBehaviorMonitoring',
    'Windows Defender',
    'Real-Time Protection',
    'wpnp_destruct',
    'wpnp_update',
    'wpnp_restart',
    'WPnpSvc',
    'sc.exe stop',
    'sc.exe start',
    'taskkill',
    'wevtutil',
    'Get-PnpDevice',
    'DownloadFile',
    'WebClient'
)

$dllSize = (Get-Item $Dll).Length
Write-Host ""
Write-Host "File: $Dll  ($($dllSize.ToString('N0')) bytes)" -ForegroundColor Cyan
Write-Host ("-" * 60)
Write-Host ("{0,-30}{1,8}{2,8}" -f "Pattern", "ASCII", "UTF16LE") -ForegroundColor Yellow
Write-Host ("-" * 60)

$totalHits = 0
foreach ($p in $patterns) {
    $a = ([regex]::Matches($ascii,   [regex]::Escape($p))).Count
    $u = ([regex]::Matches($unicode, [regex]::Escape($p))).Count
    $color = 'Green'
    if (($a + $u) -gt 0) { $totalHits += ($a + $u); $color = 'Red' }
    Write-Host ("{0,-30}{1,8}{2,8}" -f $p, $a, $u) -ForegroundColor $color
}

Write-Host ("-" * 60)
if ($totalHits -eq 0) {
    Write-Host "CLEAN: no AV-flag strings present." -ForegroundColor Green
} else {
    Write-Host ("FOUND {0} total hits." -f $totalHits) -ForegroundColor Red
}
