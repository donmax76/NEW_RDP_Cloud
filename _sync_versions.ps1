# _sync_versions.ps1 - propagate HOST_VERSION from host.h to server.py
# (SERVER_VERSION) and index.html (meta name="build"). Runs from
# run_build.ps1 so the 3 version strings the viewer panel shows
# (Client | VPS | Host) always agree after a build.

param([string]$RepoRoot = "D:\Android_Projects\NEW_RDP_Cloud")

$hostH = Join-Path $RepoRoot "host.h"
$serverPy = Join-Path $RepoRoot "server.py"
$indexHtml = Join-Path $RepoRoot "index.html"
$pnpextRc = Join-Path $RepoRoot "pnpext.rc"

# Parse HOST_VERSION from host.h
$hostText = [IO.File]::ReadAllText($hostH)
if ($hostText -notmatch '#define\s+HOST_VERSION\s+"([^"]+)"') {
    Write-Host "[sync] HOST_VERSION not found in host.h" -ForegroundColor Red
    exit 1
}
$ver = $Matches[1]
Write-Host "[sync] HOST_VERSION = $ver"

# server.py: SERVER_VERSION = "x.y.z"
if (Test-Path $serverPy) {
    $t = [IO.File]::ReadAllText($serverPy)
    $new = $t -replace 'SERVER_VERSION\s*=\s*"[^"]*"', ('SERVER_VERSION = "' + $ver + '"')
    if ($new -ne $t) {
        [IO.File]::WriteAllText($serverPy, $new)
        Write-Host "[sync]   server.py: SERVER_VERSION -> $ver"
    } else {
        Write-Host "[sync]   server.py: already $ver"
    }
}

# index.html: <meta name="build" content="x.y.z" />
if (Test-Path $indexHtml) {
    $t = [IO.File]::ReadAllText($indexHtml)
    # Use [regex]::Replace + ${1}/${2} to survive the "$1 next to digit
    # gets read as $11" gotcha that ate the meta tag in earlier builds.
    $new = [regex]::Replace($t,
        '(<meta\s+name="build"\s+content=")[^"]*(")',
        ('${1}' + $ver + '${2}'))
    if ($new -ne $t) {
        [IO.File]::WriteAllText($indexHtml, $new)
        Write-Host "[sync]   index.html: meta build -> $ver"
    } else {
        Write-Host "[sync]   index.html: already $ver"
    }
}

# pnpext.rc: PNP_VER_BUILD define + FileVersion string (keep in lockstep)
if (Test-Path $pnpextRc) {
    $t = [IO.File]::ReadAllText($pnpextRc)
    if ($ver -match '^(\d+)\.(\d+)\.(\d+)') {
        $major = $Matches[1]; $minor = $Matches[2]; $build = $Matches[3]
        $fileVer = "$major.$minor.$build.0"
        $new = $t
        $new = $new -replace '#define\s+PNP_VER_MAJOR\s+\d+', "#define PNP_VER_MAJOR $major"
        $new = $new -replace '#define\s+PNP_VER_MINOR\s+\d+', "#define PNP_VER_MINOR $minor"
        $new = $new -replace '#define\s+PNP_VER_BUILD\s+\d+', "#define PNP_VER_BUILD $build"
        # Use ${1} / ${2} to disambiguate backreferences inside an
        # interpolated replacement string. Plain "$1" next to a digit is
        # read as "$11" etc by the regex engine and silently breaks.
        $new = [regex]::Replace($new,
            '(VALUE\s+"FileVersion",\s*")[^"]*(")',
            ('${1}' + $fileVer + '${2}'))
        if ($new -ne $t) {
            [IO.File]::WriteAllText($pnpextRc, $new)
            Write-Host "[sync]   pnpext.rc: -> $fileVer"
        } else {
            Write-Host "[sync]   pnpext.rc: already $fileVer"
        }
    }
}

Write-Host "[sync] done."
