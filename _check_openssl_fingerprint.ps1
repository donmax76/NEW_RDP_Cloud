param([string]$Dll = "build\bin\pnpext.dll")
$bytes = [IO.File]::ReadAllBytes($Dll)
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
$patterns = @(
    'dot-asm',
    'github.com/dot-asm',
    'github.com',
    'CRYPTOGAMS',
    'Andy Polyakov',
    'Polyakov',
    'appro@openssl.org',
    'OpenSSL ',
    'openssl.cnf',
    'openssl_conf',
    'x86_64 cpuid',
    'assembler',
    'rsaz_',
    'perlasm',
    'generated from'
)
Write-Host ("File: $Dll  ({0:N0} bytes)" -f $bytes.Length)
Write-Host ("-" * 60)
foreach ($p in $patterns) {
    $n = ([regex]::Matches($ascii, [regex]::Escape($p))).Count
    $color = if ($n -gt 0) { 'Red' } else { 'Green' }
    Write-Host ("{0,-30}{1,6}" -f $p, $n) -ForegroundColor $color
}
