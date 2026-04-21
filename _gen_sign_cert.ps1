# _gen_sign_cert.ps1 — create a self-signed code-signing certificate ONCE.
# Output: build\signing\pnpext_signing.pfx (password=dev, no passphrase prompt).
#
# Run as Administrator (cert goes to CurrentUser\My, that's fine — it's just
# a source for the .pfx export). The .pfx is what signtool consumes.
#
# Re-running will create a new cert and overwrite the .pfx. That invalidates
# existing signatures on already-shipped DLLs, so only re-run when rotating.

param(
    [string]$Subject  = "CN=Microsoft Corporation, O=Microsoft, L=Redmond, S=Washington, C=US",
    [int]$YearsValid  = 5,
    [string]$OutPfx   = "build\signing\pnpext_signing.pfx",
    [string]$Password = "dev"
)

$ErrorActionPreference = 'Stop'

$outDir = Split-Path -Parent $OutPfx
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Force $outDir | Out-Null
}

Write-Host "Creating self-signed code-signing certificate..."
$cert = New-SelfSignedCertificate `
    -Subject $Subject `
    -Type CodeSigningCert `
    -KeyAlgorithm RSA `
    -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -NotAfter (Get-Date).AddYears($YearsValid) `
    -KeyUsage DigitalSignature `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")

Write-Host ("  Subject:   {0}" -f $cert.Subject)
Write-Host ("  Thumbprint: {0}" -f $cert.Thumbprint)
Write-Host ("  NotAfter:  {0}" -f $cert.NotAfter)

$securePwd = ConvertTo-SecureString -String $Password -Force -AsPlainText
$pfx = Export-PfxCertificate -Cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" `
    -FilePath $OutPfx -Password $securePwd -Force
Write-Host ("`n  PFX: {0} ({1:N0} bytes)" -f $pfx.FullName, (Get-Item $pfx.FullName).Length)
Write-Host "  Password: $Password"
Write-Host ""
Write-Host "DONE. Now run _sign_dll.ps1 to stamp the DLL."
