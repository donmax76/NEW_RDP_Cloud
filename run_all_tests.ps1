# run_all_tests.ps1 - execute every stage-2 smoke test in sequence.
#
# Run from anywhere; CD's into the repo itself.
# Output is a compact PASS/FAIL table at the end.

Set-Location 'D:\Android_Projects\NEW_RDP_Cloud'

$tests = @(
    @{ name = 'Stage2Test         (AES-GCM + reflective load)'
       exe  = 'build\bin\Stage2Test.exe'
       args = @('build\stage2\Stage2Sample.bin', 'dev-token')   },
    @{ name = 'Stage2LoaderTest   (Registry + dispatch + wipe)'
       exe  = 'build\bin\Stage2LoaderTest.exe'
       args = @()                                               },
    @{ name = 'Stage2FilemgrTest  (6 file-mutation commands)'
       exe  = 'build\bin\Stage2FilemgrTest.exe'
       args = @()                                               },
    @{ name = 'Stage2ProcmgrTest  (8 proc/svc/reg commands)'
       exe  = 'build\bin\Stage2ProcmgrTest.exe'
       args = @()                                               },
    @{ name = 'Stage2DefenderTest (defender_status+restart+evtlog)'
       exe  = 'build\bin\Stage2DefenderTest.exe'
       args = @()                                               }
)

$results = @()
foreach ($t in $tests) {
    if (-not (Test-Path $t.exe)) {
        $results += [PSCustomObject]@{ name = $t.name; status = 'MISSING'; exit = -1 }
        continue
    }
    $out = Join-Path $env:TEMP ('_test_' + [IO.Path]::GetFileNameWithoutExtension($t.exe) + '.out')
    $err = Join-Path $env:TEMP ('_test_' + [IO.Path]::GetFileNameWithoutExtension($t.exe) + '.err')
    # Start-Process rejects empty -ArgumentList arrays; branch on whether args present.
    if ($t.args -and $t.args.Count -gt 0) {
        $p = Start-Process -FilePath $t.exe -ArgumentList $t.args -NoNewWindow -Wait -PassThru `
                -RedirectStandardOutput $out -RedirectStandardError $err
    } else {
        $p = Start-Process -FilePath $t.exe -NoNewWindow -Wait -PassThru `
                -RedirectStandardOutput $out -RedirectStandardError $err
    }
    $exit = $p.ExitCode
    $status = if ($exit -eq 0) { 'PASS' } else { 'FAIL' }
    $results += [PSCustomObject]@{ name = $t.name; status = $status; exit = $exit; log = $out }
}

Write-Host "`n══════════════════════════════════════════════════════════"
Write-Host "Stage-2 test results"
Write-Host "══════════════════════════════════════════════════════════"
foreach ($r in $results) {
    $color = if ($r.status -eq 'PASS') { 'Green' } else { 'Red' }
    Write-Host ("  [{0}]  {1}  (exit={2})" -f $r.status, $r.name, $r.exit) -ForegroundColor $color
}
$failed = ($results | Where-Object { $_.status -ne 'PASS' }).Count
Write-Host "══════════════════════════════════════════════════════════"
if ($failed -eq 0) {
    Write-Host "ALL $($results.Count) TESTS PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failed of $($results.Count) tests FAILED - see $env:TEMP\_test_*.out" -ForegroundColor Red
    exit 1
}
