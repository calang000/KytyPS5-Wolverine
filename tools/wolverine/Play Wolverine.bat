@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-Wolverine.ps1" %*
if errorlevel 1 pause
