# _sync_memory.ps1 - sync local Claude agent memory to .claude/memory/ in repo
# Usage:
#   .\_sync_memory.ps1          - sync and commit
#   .\_sync_memory.ps1 -Push    - sync, commit and push to origin/master
#   .\_sync_memory.ps1 -DryRun  - show what would change, no writes

param(
    [switch]$Push,
    [switch]$DryRun
)

$RepoRoot  = Split-Path $MyInvocation.MyCommand.Path -Resolve
$RepoDest  = Join-Path $RepoRoot ".claude\memory"

# Compute Claude Code project key:
#   D:\Android_Projects\NEW_RDP_Cloud  ->  D--Android-Projects-NEW-RDP-Cloud
$ProjectKey = $RepoRoot -replace ':\\', '--' -replace '\\', '-' -replace '_', '-'
$LocalSrc   = Join-Path $env:USERPROFILE ".claude\projects\$ProjectKey\memory"

Write-Host "Source : $LocalSrc"
Write-Host "Dest   : $RepoDest"
Write-Host ""

if (-not (Test-Path $LocalSrc)) {
    Write-Host "ERROR: local memory not found: $LocalSrc" -ForegroundColor Red
    exit 1
}

if ((-not (Test-Path $RepoDest)) -and (-not $DryRun)) {
    New-Item -ItemType Directory -Path $RepoDest -Force | Out-Null
    Write-Host "Created: $RepoDest"
}

# Sync: compare by MD5, copy only changed files
$copied  = [System.Collections.Generic.List[string]]::new()
$skipped = 0

Get-ChildItem $LocalSrc -Filter "*.md" | ForEach-Object {
    $srcFile = $_.FullName
    $dstFile = Join-Path $RepoDest $_.Name

    $needsCopy = $true
    if (Test-Path $dstFile) {
        $srcHash = (Get-FileHash $srcFile -Algorithm MD5).Hash
        $dstHash = (Get-FileHash $dstFile -Algorithm MD5).Hash
        if ($srcHash -eq $dstHash) { $needsCopy = $false }
    }

    if ($needsCopy) {
        if (-not $DryRun) { Copy-Item $srcFile $dstFile -Force }
        Write-Host "  [updated] $($_.Name)" -ForegroundColor Green
        $copied.Add($_.Name)
    } else {
        $skipped++
    }
}

Write-Host ""
Write-Host "$($copied.Count) files updated, $skipped unchanged"

if ($DryRun) {
    Write-Host "[DryRun] No files were written." -ForegroundColor Yellow
    exit 0
}

if ($copied.Count -eq 0) {
    Write-Host "Nothing to commit." -ForegroundColor Yellow
    exit 0
}

# Git: stage + commit
Set-Location $RepoRoot
git add -f ".claude\memory\"

$n       = $copied.Count
$ts      = Get-Date -Format "yyyy-MM-dd"
$coauth  = 'Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>'
$msg     = "chore: sync agent memory ($n files updated $ts)`n`n$coauth"

git commit -m $msg

if ($LASTEXITCODE -ne 0) {
    Write-Host "Commit failed (nothing staged?)." -ForegroundColor Yellow
    exit 1
}

Write-Host "Committed." -ForegroundColor Cyan

if ($Push) {
    git push origin master
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Pushed to origin/master." -ForegroundColor Green
    } else {
        Write-Host "Push failed." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "To push: git push origin master"
    Write-Host "Or:      .\_sync_memory.ps1 -Push"
}
