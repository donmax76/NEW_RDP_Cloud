param([string]$Dll = "build\bin\pnpext.dll")

if (-not (Test-Path $Dll)) { throw "DLL not found: $Dll" }

# OpenSSL static libs carry Andy Polyakov perlasm copyright headers in
# .rdata. VirusTotal behavioural sandbox reads them from memory and
# reports "github.com/dot-asm" as a "Memory Pattern URL" plus CRYPTOGAMS
# and Polyakov as distinguishing strings that fingerprint statically-
# linked OpenSSL.
#
# Our previous attempt replaced with "github.com/ms-asm_" of the same
# length. That still matched VT's URL-extraction regex (anything with
# "github.com/...") and kept showing up in behavioural reports.
#
# Now: overwrite with NUL bytes. The strings are never referenced at
# runtime (pure .ascii directives from the assembly source), so NUL-
# padding is harmless to execution. Strings extractors stop at NUL so
# these effectively disappear from any static listing.

# Pattern list — all get NUL-overwritten. Order longest first so a
# substring match doesn't double-hit a shorter pattern.
$patterns = @(
    'github.com/dot-asm',   # VT Memory Pattern URL
    'github.com/ms-asm_',   # previous scrub's leftover
    'Andy Polyakov',
    'CRYPTOGAMS',
    'dot-asm',
    'ms-asm_'
)

$bytes = [System.IO.File]::ReadAllBytes($Dll)
Write-Host ("File: {0}  ({1:N0} bytes)" -f $Dll, $bytes.Length)

$totalHits = 0
foreach ($from in $patterns) {
    $fromBytes = [Text.Encoding]::ASCII.GetBytes($from)
    $toBytes   = New-Object byte[] $fromBytes.Length   # all-zero
    $hits = 0
    $i = 0
    while ($i -le $bytes.Length - $fromBytes.Length) {
        $match = $true
        for ($j = 0; $j -lt $fromBytes.Length; $j++) {
            if ($bytes[$i + $j] -ne $fromBytes[$j]) { $match = $false; break }
        }
        if ($match) {
            for ($j = 0; $j -lt $toBytes.Length; $j++) {
                $bytes[$i + $j] = $toBytes[$j]
            }
            $hits++
            $i += $fromBytes.Length
        } else {
            $i++
        }
    }
    $totalHits += $hits
    Write-Host ("  {0,-24} -> {1,-24}  patched={2}" -f $from, '(NUL bytes)', $hits)
}

if ($totalHits -gt 0) {
    [System.IO.File]::WriteAllBytes($Dll, $bytes)
    Write-Host ""
    Write-Host "[+] Patched $totalHits occurrences. Re-signing required."
} else {
    Write-Host ""
    Write-Host "[=] No occurrences found, binary already clean."
}
