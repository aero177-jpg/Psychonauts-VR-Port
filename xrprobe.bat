@echo off
setlocal
rem Builds and runs the OpenXR probe (src\test\xrprobe.cpp): a standalone
rem 32-bit exe that reports what the active OpenXR runtime offers.
rem   xrprobe.bat                 the registered 32-bit runtime
rem   xrprobe.bat path\to.json    a specific runtime manifest

set "ROOT=%~dp0"
set "OUT=%ROOT%build\test"
if not exist "%OUT%" mkdir "%OUT%"
if not "%~1"=="" set "XR_RUNTIME_JSON=%~1"

call "%ROOT%msvc.bat"
if errorlevel 1 exit /b 1

cl /nologo /EHsc /MT /O2 /W4 /std:c++17 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%third_party\openxr\include" /I"%ROOT%third_party\vulkan\include" ^
   /Fo"%OUT%\\" /Fe"%OUT%\xrprobe.exe" "%ROOT%src\test\xrprobe.cpp" ^
   /link /MACHINE:X86 "%ROOT%third_party\openxr\lib\win32\openxr_loader.lib" ^
   advapi32.lib kernel32.lib user32.lib pathcch.lib
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
"%OUT%\xrprobe.exe" > "%OUT%\xrprobe.log" 2>&1
type "%OUT%\xrprobe.log"
exit /b 0
