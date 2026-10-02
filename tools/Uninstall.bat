@echo off
rem Removes Max2 Reframe. Close DaVinci Resolve first. Windows asks once for admin rights.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
pause
