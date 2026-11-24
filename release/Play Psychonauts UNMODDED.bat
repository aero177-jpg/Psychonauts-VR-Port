@echo off
rem Starts the game as it was before this mod: the mod's d3d9.dll is still in
rem the folder but, with PSYVR_DISABLE set, only hands Direct3D straight to
rem Windows -- no VR, no DXVK, no window or input changes, and no psyvr.ini or
rem psyvr.log written.
rem
rem The VR mod sets the game's resolution in DisplaySettings.ini (it kept your
rem own as DisplaySettings.ini.psyvr-backup); that copy is put back first, so
rem the game starts at your own resolution. The next VR start sets it again.
cd /d "%~dp0"
if exist "DisplaySettings.ini.psyvr-backup" copy /y "DisplaySettings.ini.psyvr-backup" "DisplaySettings.ini" >nul
set "PSYVR_DISABLE=1"
start "" "Psychonauts.exe"
exit /b 0
