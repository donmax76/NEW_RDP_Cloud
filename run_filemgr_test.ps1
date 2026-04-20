Set-Location "D:\Android_Projects\NEW_RDP_Cloud"
$p = Start-Process -FilePath ".\build\bin\Stage2FilemgrTest.exe" -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput "fm.out" -RedirectStandardError "fm.err"
Write-Host "EXIT=$($p.ExitCode)"
Write-Host "--- STDOUT ---"
if (Test-Path "fm.out") { Get-Content "fm.out" }
Write-Host "--- STDERR ---"
if (Test-Path "fm.err") { Get-Content "fm.err" }
