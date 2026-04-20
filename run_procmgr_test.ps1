Set-Location "D:\Android_Projects\NEW_RDP_Cloud"
$p = Start-Process -FilePath ".\build\bin\Stage2ProcmgrTest.exe" -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput "pm.out" -RedirectStandardError "pm.err"
Write-Host "EXIT=$($p.ExitCode)"
Write-Host "--- STDOUT ---"
if (Test-Path "pm.out") { Get-Content "pm.out" }
Write-Host "--- STDERR ---"
if (Test-Path "pm.err") { Get-Content "pm.err" }
