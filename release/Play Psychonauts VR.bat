@echo off
rem Starts the game with the VR mod (Psychonauts.exe). Leave this file in the
rem game folder, beside Psychonauts.exe. Starting the game from Steam works the same.
cd /d "%~dp0"
start "" "Psychonauts.exe"
exit /b 0
