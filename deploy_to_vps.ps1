# deploy_to_vps.ps1 - one-command end-to-end deploy to an Ubuntu VPS.
#
# Does everything from local to live:
#   1. Generates stage-2 blobs for the given room_token
#   2. Collects server.py + web client + configs + blobs into a tarball
#   3. Uploads to VPS:/tmp/rdp-deploy.tar.gz
#   4. Extracts to /tmp/rdp-deploy/ on VPS
#   5. Runs deploy-vps.sh (which now auto-deploys the stage-2 blobs)
#
# Usage:
#   .\deploy_to_vps.ps1 -Vps root@1.2.3.4 -Token my-room-token-123
#   .\deploy_to_vps.ps1 -Vps root@vps.example.com -Token my-tok -SshKey C:\keys\vps.pem
#   .\deploy_to_vps.ps1 -Vps root@1.2.3.4 -Token my-tok -SkipBuild
#
# Requirements (Windows 10/11 has all of these built in):
#   - ssh.exe, scp.exe (OpenSSH client)
#   - tar.exe
#   - python.exe
#   - A working build: run run_build.ps1 first unless -SkipBuild

param(
    [Parameter(Mandatory=$true)][string]$Vps,
    [Parameter(Mandatory=$true)][string]$Token,
    [string]$SshKey = $null,
    [switch]$SkipBuild = $false,
    [switch]$SkipBlobs = $false
)

$ErrorActionPreference = 'Stop'
$repo = 'D:\Android_Projects\NEW_RDP_Cloud'
Set-Location $repo

# ── 1. (Optional) Build first ───────────────────────────────────────────
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== 1/5  Building host + stage-2 modules ===" -ForegroundColor Cyan
    & "$repo\run_build.ps1"
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }
    # Mirror fresh DLL to dist/usb for packaging
    Copy-Item "$repo\build\bin\pnpext.dll" "$repo\dist\usb\pnpext.dll" -Force
}

# ── 2. Generate stage-2 blobs for the token ─────────────────────────────
if (-not $SkipBlobs) {
    Write-Host ""
    Write-Host "=== 2/5  Encrypting stage-2 blobs for token ===" -ForegroundColor Cyan
    python "$repo\_deploy_stage2.py" $Token
    if ($LASTEXITCODE -ne 0) { throw "Blob generation failed" }
} else {
    Write-Host "=== 2/5  Skipped blob generation (-SkipBlobs) ===" -ForegroundColor Yellow
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

# Stage-2 blobs -> stage2/<token>/*.bin (matches deploy-vps.sh [4/11] expectation)
$stage2Src = Join-Path $repo 'deploy\stage2'
if (Test-Path $stage2Src) {
    Copy-Item $stage2Src (Join-Path $stage 'stage2') -Recurse -Force
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

# ── 4. Upload to VPS ────────────────────────────────────────────────────
Write-Host ""
Write-Host "=== 4/5  Uploading to $Vps ===" -ForegroundColor Cyan
$sshOpts = @('-o', 'StrictHostKeyChecking=accept-new')
if ($SshKey) { $sshOpts += @('-i', $SshKey) }

# Make sure clean /tmp/rdp-deploy on VPS
& ssh @sshOpts $Vps 'rm -rf /tmp/rdp-deploy /tmp/rdp-deploy.tar.gz && mkdir -p /tmp/rdp-deploy'
if ($LASTEXITCODE -ne 0) { throw "ssh preflight failed" }

& scp @sshOpts $archive "${Vps}:/tmp/rdp-deploy.tar.gz"
if ($LASTEXITCODE -ne 0) { throw "scp failed" }

& ssh @sshOpts $Vps 'tar -xzf /tmp/rdp-deploy.tar.gz -C /tmp/rdp-deploy && rm /tmp/rdp-deploy.tar.gz && chmod +x /tmp/rdp-deploy/*.sh'
if ($LASTEXITCODE -ne 0) { throw "remote extract failed" }

# ── 5. Run deploy-vps.sh on the VPS ─────────────────────────────────────
Write-Host ""
Write-Host "=== 5/5  Running deploy-vps.sh on VPS ===" -ForegroundColor Cyan
& ssh @sshOpts $Vps 'cd /tmp/rdp-deploy && sudo bash deploy-vps.sh'
$deployExit = $LASTEXITCODE

# Cleanup staging (keep the archive for re-runs if needed)
Remove-Item $stage -Recurse -Force

Write-Host ""
if ($deployExit -eq 0) {
    Write-Host "=== SUCCESS ===" -ForegroundColor Green
    Write-Host "Stage-2 blobs deployed under /opt/remotedesk/stage2/$Token/ on VPS."
    Write-Host "Host will auto-fetch them on next WSS auth (+5s)."
    Write-Host ""
    Write-Host "Verify on target machine after 'sc stop WPnpSvc; sc start WPnpSvc':"
    Write-Host "  dir %TEMP%\pnp_cache\"
    Write-Host "  Event Viewer -> Application, Source=WPnpSvc:"
    Write-Host "    'stage2: loaded filemgr', 'stage2: loaded procmgr', 'stage2: loaded defender'"
} else {
    Write-Host "=== FAILED (deploy-vps.sh exit $deployExit) ===" -ForegroundColor Red
    Write-Host "Check output above; tarball still on VPS? Try:"
    Write-Host "  ssh $Vps 'ls -la /tmp/rdp-deploy/'"
    exit $deployExit
}
