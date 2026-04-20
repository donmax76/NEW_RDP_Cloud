Set-Location "D:\Android_Projects\NEW_RDP_Cloud"
$proc = Start-Process -FilePath ".\build\bin\Stage2LoaderTest.exe" -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput "loader_test.out" -RedirectStandardError "loader_test.err"
Write-Host "EXIT=$($proc.ExitCode)"
Write-Host "--- STDOUT ---"
if (Test-Path "loader_test.out") { Get-Content "loader_test.out" }
Write-Host "--- STDERR ---"
if (Test-Path "loader_test.err") { Get-Content "loader_test.err" }
