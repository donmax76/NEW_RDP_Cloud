# _quick_deploy.ps1 — one-shot push of pnpext.dll + all stage-2 DLLs +
# server.py + index.html to the VPS, wipe per-token cache, restart
# rdp-relay. Use INSTEAD of bashing individual scp commands.
# Assumes build\plink.exe + build\pscp.exe already downloaded (from the
# earlier "install plink" chain) and a VPS host key previously accepted.

param(
    [string]$Vps  = 'root@64.226.66.66',
    [string]$Pw   = 'Admin123456!@Semenic90#A',
    [string]$Hkey = 'SHA256:8YWNsHOCPhxij5tF10+ZUvYBGRUxRNrZB8S+lBJWlis'
)

$repo  = 'D:\Android_Projects\NEW_RDP_Cloud'
$plink = Join-Path $repo 'build\plink.exe'
$pscp  = Join-Path $repo 'build\pscp.exe'

$files = @(
    @{ src = "$repo\dist\usb\pnpext.dll";   dst = '/var/www/remote-desktop/files/pnpext.dll' },
    @{ src = "$repo\server.py";              dst = '/opt/remotedesk/server.py' },
    @{ src = "$repo\index.html";             dst = '/var/www/remote-desktop/index.html' },
    @{ src = "$repo\build\stage2\filemgr.dll";  dst = '/opt/remotedesk/stage2/filemgr.dll' },
    @{ src = "$repo\build\stage2\procmgr.dll";  dst = '/opt/remotedesk/stage2/procmgr.dll' },
    @{ src = "$repo\build\stage2\defender.dll"; dst = '/opt/remotedesk/stage2/defender.dll' },
    @{ src = "$repo\build\stage2\sysinfo.dll";  dst = '/opt/remotedesk/stage2/sysinfo.dll' }
)

foreach ($f in $files) {
    if (-not (Test-Path $f.src)) {
        Write-Host ("  SKIP (missing): {0}" -f $f.src) -ForegroundColor Yellow
        continue
    }
    & $pscp -batch -hostkey $Hkey -pw $Pw $f.src ("${Vps}:" + $f.dst) 2>&1 | Select-Object -Last 1
}

Write-Host ''
Write-Host 'Wiping per-token cache + restarting rdp-relay...'
& $plink -ssh -batch -hostkey $Hkey -pw $Pw $Vps 'rm -rf /opt/remotedesk/stage2/cache/*/*.bin; systemctl restart rdp-relay; sleep 1; systemctl is-active rdp-relay' 2>&1

Write-Host ''
Write-Host '=== Version verification ==='
& $plink -ssh -batch -hostkey $Hkey -pw $Pw $Vps 'grep -m1 SERVER_VERSION /opt/remotedesk/server.py; strings /var/www/remote-desktop/files/pnpext.dll 2>/dev/null | grep -E "^1\.0\.[0-9]+" | head -1' 2>&1
