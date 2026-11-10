@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall_driver.ps1" %*
