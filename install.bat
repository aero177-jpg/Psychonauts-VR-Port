@echo off
setlocal
rem Copies the proxy (and the DXVK fork, if built) into the game folder.
rem   install.bat                 Steam's default install location
rem   install.bat "D:\Games\Psychonauts"  any other folder with Psychonauts.exe
rem
rem An existing d3d9.dll that is not ours (another mod) is moved aside to
rem d3d9.dll.psyvr-backup rather than overwritten.

set "ROOT=%~dp0"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%ProgramFiles(x86)%\Steam\steamapps\common\Psychonauts"

if not exist "%GAME%\Psychonauts.exe" (
    echo ERROR: Psychonauts.exe not found in "%GAME%"
    exit /b 1
)
if not exist "%ROOT%dist\d3d9.dll" (
    echo ERROR: dist\d3d9.dll not built yet. Run build.bat first.
    exit /b 1
)
if not exist "%ROOT%dist\psyvr_logic.dll" (
    echo ERROR: dist\psyvr_logic.dll not built yet. Run build.bat first.
    exit /b 1
)

if exist "%GAME%\d3d9.dll" (
    findstr /m /c:"psyvr.ini" "%GAME%\d3d9.dll" >nul 2>&1
    if errorlevel 1 (
        echo Moving the existing d3d9.dll aside to d3d9.dll.psyvr-backup
        move /y "%GAME%\d3d9.dll" "%GAME%\d3d9.dll.psyvr-backup" >nul
    )
)

copy /y "%ROOT%dist\d3d9.dll" "%GAME%\d3d9.dll" >nul || exit /b 1
echo Installed d3d9.dll
copy /y "%ROOT%dist\psyvr_logic.dll" "%GAME%\psyvr_logic.dll" >nul || exit /b 1
echo Installed psyvr_logic.dll
rem The DXVK fork with the VR interop (build_dxvk.bat).
if exist "%ROOT%dist\d3d9_dxvk.dll" (
    copy /y "%ROOT%dist\d3d9_dxvk.dll" "%GAME%\d3d9_dxvk.dll" >nul || exit /b 1
    echo Installed d3d9_dxvk.dll
) else (
    echo No dist\d3d9_dxvk.dll ^(run build_dxvk.bat^): the proxy will use the system d3d9, no VR.
)

rem Optional: the PsyVR Astralathe mod (the VR mod writes its script).
if exist "%GAME%\ModResource" (
    xcopy /e /i /y /q "%ROOT%mod\PsyVR" "%GAME%\ModResource\PsyVR" >nul
    echo Installed ModResource\PsyVR
) else (
    echo No ModResource folder ^(Astralathe not installed^): skipped, not needed.
)

exit /b 0
