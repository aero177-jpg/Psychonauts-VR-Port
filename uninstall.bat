@echo off
setlocal
rem Removes the mod's files from the game folder and puts back a d3d9.dll that
rem install.bat moved aside. Settings (psyvr.ini) and logs are kept.

set "ROOT=%~dp0"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%ProgramFiles(x86)%\Steam\steamapps\common\Psychonauts"
if not exist "%GAME%\Psychonauts.exe" (
    echo ERROR: Psychonauts.exe not found in "%GAME%"
    exit /b 1
)
rem Only our own d3d9.dll is removed (it names psyvr.ini, the check install.bat
rem uses): another mod's d3d9.dll, e.g. ReShade, stays where it is.
if exist "%GAME%\d3d9.dll" (
    findstr /m /c:"psyvr.ini" "%GAME%\d3d9.dll" >nul 2>&1
    if not errorlevel 1 del /q "%GAME%\d3d9.dll"
)
del /q "%GAME%\psyvr_logic.dll" "%GAME%\d3d9_dxvk.dll" 2>nul
if exist "%GAME%\d3d9.dll.psyvr-backup" if not exist "%GAME%\d3d9.dll" move /y "%GAME%\d3d9.dll.psyvr-backup" "%GAME%\d3d9.dll" >nul
rem The mod sets the game's resolution in DisplaySettings.ini: put back the
rem player's own file, or remove one the mod created (it leaves a
rem .psyvr-created mark then). A file with neither is the player's: kept.
if exist "%GAME%\DisplaySettings.ini.psyvr-backup" (
    move /y "%GAME%\DisplaySettings.ini.psyvr-backup" "%GAME%\DisplaySettings.ini" >nul
) else if exist "%GAME%\DisplaySettings.ini.psyvr-created" (
    del /q "%GAME%\DisplaySettings.ini" 2>nul
)
del /q "%GAME%\DisplaySettings.ini.psyvr-created" 2>nul
if exist "%GAME%\ModResource\PsyVR" rmdir /s /q "%GAME%\ModResource\PsyVR"
echo Removed.
exit /b 0
