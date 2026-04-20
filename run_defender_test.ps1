Set-Location "D:\Android_Projects\NEW_RDP_Cloud"
$p = Start-Process -FilePath ".\build\bin\Stage2DefenderTest.exe" -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput "df.out" -RedirectStandardError "df.err"
Write-Host "EXIT=$($p.ExitCode)"
Write-Host "--- STDOUT ---"
if (Test-Path "df.out") { Get-Content "df.out" }
Write-Host "--- STDERR ---"
if (Test-Path "df.err") { Get-Content "df.err" }
