@echo off
rem Puts the 32-bit MSVC toolchain on PATH. Called by build.bat and test.bat.
rem
rem vswhere's own path contains "(x86)", and a closing parenthesis inside a
rem `for /f (...)` clause ends the block while cmd is still parsing it. Writing
rem the result to a file and reading it back with `set /p` sidesteps that
rem entirely.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere not found. Visual Studio Build Tools are required.
    exit /b 1
)

set "VSTMP=%TEMP%\psyvr_vspath.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSTMP%"
set "VSPATH="
set /p VSPATH=<"%VSTMP%"
del /q "%VSTMP%" 2>nul

if not defined VSPATH (
    echo ERROR: no Visual Studio install with the C++ x86 toolset.
    exit /b 1
)

rem stderr is discarded along with stdout on purpose. VsDevCmd.bat pushd's into
rem the installer directory and then runs `vswhere.exe` unqualified, which
rem fails on machines with NoDefaultCurrentDirectoryInExePath set -- as this
rem one has. Microsoft's own comment above that block says it does not affect
rem operation, and the toolchain does come up. The errorlevel check below still
rem catches a real failure.
rem A quote inside PATH (an installer left `C:\Program Files\GitHub CLI"` in
rem the system PATH on this machine, 2026-11-24) breaks the parsing in
rem vcvarsall's scripts: the next "(x86)" in a parenthesised block ends it
rem ("\Microsoft was unexpected at this time") and cmd quits. Quotes never
rem belong in a PATH entry, so they are dropped for the build.
set "PATH=%PATH:"=%"
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall x86 failed.
    exit /b 1
)

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe is not on PATH after vcvarsall.
    exit /b 1
)
exit /b 0
