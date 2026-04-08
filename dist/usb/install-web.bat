@echo off
set SERVER=https://64.226.66.66
if not "%~1"=="" set SERVER=%~1
powershell -ExecutionPolicy Bypass -Command "Start-Process powershell -ArgumentList '-ExecutionPolicy Bypass -File \"%~dp0install-web.ps1\" -Server %SERVER%' -Verb RunAs"
