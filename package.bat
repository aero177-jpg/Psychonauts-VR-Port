@echo off
setlocal enabledelayedexpansion
rem Assembles the drag-and-drop release package from the current build.
rem
rem   package.bat                       into %USERPROFILE%\Desktop\PsyVRmod
rem   package.bat <folder>              somewhere else
rem   package.bat <folder> zip          also writes <folder>.zip beside it
rem   package.bat <folder> zip <dist>   take the DLLs from <dist> instead of dist\
rem                                     (or set PSYVR_DIST; the argument wins)
rem
rem Layout (the zip's contents):
rem   Install.bat, Uninstall.bat, READ ME FIRST.txt, psyvr.sample.ini,
rem   VERSION.txt, psyvr-package.txt
rem   installer\psyvr_install.ps1
rem   Game files\   the files that go beside Psychonauts.exe
rem   Optional\PsyVR\   the Astralathe part, installed only if Astralathe is
rem   Licences\
rem
rem Run build.bat and build_dxvk.bat first. It refuses to write into a
rem folder that exists and is not one of ours: a mistyped path must never
rem make a packaging script drop a d3d9.dll into somebody else's mod. Ours
rem carry psyvr-package.txt at the top.

set "ROOT=%~dp0"
set "DEST=%USERPROFILE%\Desktop\PsyVRmod"
if not "%~1"=="" set "DEST=%~1"
set "DIST=%ROOT%dist"
if defined PSYVR_DIST set "DIST=%PSYVR_DIST%"
if not "%~3"=="" set "DIST=%~3"
if "!DIST:~-1!"=="\" set "DIST=!DIST:~0,-1!"
set "GAMEF=!DEST!\Game files"
set "MARK=!DEST!\psyvr-package.txt"

if exist "!DEST!\" (
    if not exist "!MARK!" (
        dir /b "!DEST!" 2>nul | findstr "." >nul
        if not errorlevel 1 (
            echo ERROR: !DEST!
            echo   exists and is not a Psychonauts VR package ^(no psyvr-package.txt^).
            echo   Nothing was written. Pass a different folder.
            exit /b 1
        )
    )
)

for %%F in ("!DIST!\d3d9.dll" "!DIST!\psyvr_logic.dll" "!DIST!\d3d9_dxvk.dll") do (
    if not exist "%%~F" (
        echo ERROR: %%~F is missing. Run build.bat and build_dxvk.bat first, or set PSYVR_DIST.
        exit /b 1
    )
)
for %%F in ("release\READ ME FIRST.txt" "release\psyvr.sample.ini" "release\VERSION.txt" "release\Play Psychonauts VR.bat" "release\Play Psychonauts UNMODDED.bat" "release\installer\Install.bat" "release\installer\Uninstall.bat" "release\installer\psyvr_install.ps1" "mod\PsyVR\mod.json") do (
    if not exist "!ROOT!%%~F" (
        echo ERROR: !ROOT!%%~F is missing.
        exit /b 1
    )
)

if not exist "!GAMEF!" mkdir "!GAMEF!"
if not exist "!DEST!\installer" mkdir "!DEST!\installer"
if not exist "!DEST!\Optional\PsyVR" mkdir "!DEST!\Optional\PsyVR"
copy /y "!DIST!\d3d9.dll"                       "!GAMEF!\d3d9.dll" >nul
copy /y "!DIST!\psyvr_logic.dll"                "!GAMEF!\psyvr_logic.dll" >nul
copy /y "!DIST!\d3d9_dxvk.dll"                  "!GAMEF!\d3d9_dxvk.dll" >nul
copy /y "!ROOT!release\Play Psychonauts VR.bat" "!GAMEF!\Play Psychonauts VR.bat" >nul
copy /y "!ROOT!release\Play Psychonauts UNMODDED.bat" "!GAMEF!\Play Psychonauts UNMODDED.bat" >nul
copy /y "!ROOT!mod\PsyVR\mod.json"              "!DEST!\Optional\PsyVR\mod.json" >nul
copy /y "!ROOT!release\installer\Install.bat"   "!DEST!\Install.bat" >nul
copy /y "!ROOT!release\installer\Uninstall.bat" "!DEST!\Uninstall.bat" >nul
copy /y "!ROOT!release\installer\psyvr_install.ps1" "!DEST!\installer\psyvr_install.ps1" >nul
copy /y "!ROOT!release\READ ME FIRST.txt"       "!DEST!\READ ME FIRST.txt" >nul
copy /y "!ROOT!release\psyvr.sample.ini"        "!DEST!\psyvr.sample.ini" >nul
copy /y "!ROOT!release\VERSION.txt"             "!DEST!\VERSION.txt" >nul

rem Licences: the mod's (Apache 2.0) and those of what ships with it (DXVK;
rem the OpenXR loader is linked into d3d9.dll; Dear ImGui is linked in too).
if not exist "!DEST!\Licences" mkdir "!DEST!\Licences"
copy /y "!ROOT!LICENSE"                         "!DEST!\Licences\LICENSE.txt" >nul
copy /y "!ROOT!NOTICE"                          "!DEST!\Licences\NOTICE.txt" >nul
copy /y "!ROOT!THIRD_PARTY_NOTICES.md"          "!DEST!\Licences\THIRD_PARTY_NOTICES.txt" >nul
copy /y "!ROOT!dxvk-psyvr\LICENSE"              "!DEST!\Licences\DXVK-LICENSE.txt" >nul
copy /y "!ROOT!third_party\openxr\LICENSE"      "!DEST!\Licences\OpenXR-SDK-LICENSE.txt" >nul
copy /y "!ROOT!third_party\imgui\LICENSE.txt"   "!DEST!\Licences\DearImGui-LICENSE.txt" >nul

> "!MARK!" echo Psychonauts VR mod package. Safe to overwrite with package.bat.

echo.
echo Package written to:
echo   !DEST!
echo   Install.bat, Uninstall.bat, READ ME FIRST.txt
echo   installer\psyvr_install.ps1
echo   Game files\d3d9.dll, psyvr_logic.dll, d3d9_dxvk.dll and the Play ... .bat files
echo   Optional\PsyVR ^(Astralathe, optional^)

if /i "%~2"=="zip" (
    set "ZIP=!DEST!.zip"
    if exist "!ZIP!" del /q "!ZIP!"
    powershell -NoProfile -Command "Compress-Archive -Path '!DEST!\*' -DestinationPath '!ZIP!'"
    if errorlevel 1 (
        echo ERROR: could not write !ZIP!
        exit /b 1
    )
    echo   !ZIP!
)
exit /b 0
