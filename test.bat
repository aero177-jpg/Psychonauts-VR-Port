@echo off
setlocal
rem Offline checks, no game and no OpenXR runtime:
rem   src\test\mathtest.cpp    pose mirroring, recentre, projection, inverse
rem   src\test\configtest.cpp  the settings loader (first-run file, clamping,
rem                            live reload)
rem   the embedded default psyvr.ini matches release\psyvr.sample.ini
rem Exits non-zero if anything fails. Run by full path.

set "ROOT=%~dp0"
set "OUT=%ROOT%build\test"
if not exist "%OUT%" mkdir "%OUT%"

call "%ROOT%msvc.bat"
if errorlevel 1 exit /b 1

cl /nologo /EHsc /MT /O2 /W4 /std:c++17 /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%OUT%\\" /Fe"%OUT%\mathtest.exe" ^
   "%ROOT%src\test\mathtest.cpp" "%ROOT%src\vr\vr_math.cpp" "%ROOT%src\vr\pad_script.cpp" "%ROOT%src\vr\dpad_gesture.cpp" "%ROOT%src\vr\god_seat.cpp" ^
   /link /MACHINE:X86
if errorlevel 1 (
    echo BUILD FAILED ^(mathtest^)
    exit /b 1
)
"%OUT%\mathtest.exe"
if errorlevel 1 exit /b 1

cl /nologo /EHsc /MT /O2 /W4 /std:c++17 /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%OUT%\\" /Fe"%OUT%\configtest.exe" ^
   "%ROOT%src\test\configtest.cpp" "%ROOT%src\common\config.cpp" "%ROOT%src\common\log.cpp" ^
   /link /MACHINE:X86 user32.lib advapi32.lib
if errorlevel 1 (
    echo BUILD FAILED ^(configtest^)
    exit /b 1
)
"%OUT%\configtest.exe"
if errorlevel 1 exit /b 1

where python >nul 2>&1
if not errorlevel 1 (
    python -I "%ROOT%tools\gen_default_ini.py" --check
    if errorlevel 1 exit /b 1
)
exit /b 0
