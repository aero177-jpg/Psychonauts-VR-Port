@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\psyvr_install.ps1" -Uninstall %*
exit /b %errorlevel%
