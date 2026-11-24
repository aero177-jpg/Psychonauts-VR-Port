@echo off
rem Puts the DXVK build tools on PATH if they are in their usual locations
rem (listed below). Called by build_dxvk.bat; harmless when they are already
rem on PATH.
rem   glslang  : %USERPROFILE%\tools\glslang\bin  (KhronosGroup/glslang main-tot release),
rem              or the LunarG Vulkan SDK on PATH
rem   meson    : the Python user-scripts folder (py -3 -m pip install --user meson ninja)
rem   ninja    : the same, or the one bundled with Visual Studio's CMake

if exist "%USERPROFILE%\tools\glslang\bin\glslang.exe" call :add "%USERPROFILE%\tools\glslang\bin"

rem python.org installs put --user scripts under %APPDATA%\Python\PythonXY.
for /d %%P in ("%APPDATA%\Python\Python*") do if exist "%%~P\Scripts\meson.exe" call :add "%%~P\Scripts"
rem Microsoft Store Python keeps them in its package folder.
for /d %%D in ("%LOCALAPPDATA%\Packages\PythonSoftwareFoundation.Python.*") do (
    for /d %%P in ("%%~D\LocalCache\local-packages\Python*") do if exist "%%~P\Scripts\meson.exe" call :add "%%~P\Scripts"
)

where ninja >nul 2>&1
if not errorlevel 1 exit /b 0
for /d %%V in ("%ProgramFiles%\Microsoft Visual Studio\2022\*" "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\*") do (
    if exist "%%~V\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" call :add "%%~V\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
)
exit /b 0

rem PATH is extended in a subroutine: expanding %PATH% inside a ( ) block
rem breaks on the ')' of 'Program Files (x86)'.
:add
set "PATH=%~1;%PATH%"
exit /b 0
