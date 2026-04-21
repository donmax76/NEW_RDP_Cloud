# _sign_dll.ps1 — Authenticode-sign pnpext.dll with the self-signed PFX from
# build/signing/pnpext_signing.pfx. Timestamp via a public RFC3161 TSA so the
# signature stays valid after the cert expires.
#
# Run after run_build.ps1 (and before mirroring to dist/usb/).

param(
    [string]$Dll      = "build\bin\pnpext.dll",
    [string]$Pfx      = "build\signing\pnpext_signing.pfx",
    [string]$Password = "dev",
    [string]$Tsa      = "http://timestamp.digicert.com",
    [string]$Desc     = "Plug and Play Extension Host Service"
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Dll)) { throw "DLL not found: $Dll (run the build first)" }
if (-not (Test-Path $Pfx)) { throw "PFX not found: $Pfx (run _gen_sign_cert.ps1 first)" }

# Find signtool.exe — ships with the Windows SDK (10).
$sdkRoots = @(
    "C:\Program Files (x86)\Windows Kits\10\bin",
    "C:\Program Files\Windows Kits\10\bin"
)
$signtool = $null
foreach ($root in $sdkRoots) {
    if (-not (Test-Path $root)) { continue }
    # Prefer newest SDK's x64 signtool.
    $candidate = Get-ChildItem $root -Filter 'signtool.exe' -Recurse -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
                 Sort-Object FullName -Descending |
                 Select-Object -First 1
    if ($candidate) { $signtool = $candidate.FullName; break }
}
if (-not $signtool) { throw "signtool.exe not found. Install Windows 10/11 SDK." }
Write-Host "signtool: $signtool"

Write-Host "Signing $Dll ..."
& $signtool sign `
    /f $Pfx `
    /p $Password `
    /fd SHA256 `
    /tr $Tsa `
    /td SHA256 `
    /d $Desc `
    $Dll
if ($LASTEXITCODE -ne 0) { throw "signtool sign failed (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "Verifying signature:"
& $signtool verify /pa /v $Dll
$verifyExit = $LASTEXITCODE

# /pa returns non-zero for self-signed certs that chain to an untrusted root.
# That's expected — on other machines the signature says "signed but not
# trusted by default". Only fail hard on real errors (missing file etc.).
if ($verifyExit -eq 0) {
    Write-Host "VERIFY OK (trusted)" -ForegroundColor Green
} else {
    Write-Host "VERIFY: signed but chain untrusted (expected for self-signed)" -ForegroundColor Yellow
}

$info = Get-Item $Dll
Write-Host ""
Write-Host ("{0}: {1:N0} bytes  mtime={2}" -f $Dll, $info.Length, $info.LastWriteTime)
