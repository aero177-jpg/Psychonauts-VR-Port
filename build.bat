@echo off
setlocal
rem Builds the Psychonauts VR d3d9 proxy (dist\d3d9.dll) and its
rem draw logic (dist\psyvr_logic.dll); `build.bat logic` builds only the latter.
rem
rem Psychonauts.exe is 32-bit, so this must be a 32-bit DLL. Static CRT (/MT) keeps it
rem to a single file with no redistributable to install. Run by full path.

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "BIN=%ROOT%dist"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%BIN%" mkdir "%BIN%"

call "%ROOT%msvc.bat"
if errorlevel 1 exit /b 1

set SRC=
set SRC=%SRC% "%ROOT%src\proxy\dllmain.cpp"
set SRC=%SRC% "%ROOT%src\proxy\backend.cpp"
set SRC=%SRC% "%ROOT%src\proxy\proxy_d3d9.cpp"
set SRC=%SRC% "%ROOT%src\proxy\proxy_device.cpp"
set SRC=%SRC% "%ROOT%src\proxy\proxy_swapchain.cpp"
set SRC=%SRC% "%ROOT%src\proxy\window.cpp"
set SRC=%SRC% "%ROOT%src\proxy\game_psy.cpp"
set SRC=%SRC% "%ROOT%src\proxy\trace.cpp"
set SRC=%SRC% "%ROOT%src\proxy\control.cpp"
set SRC=%SRC% "%ROOT%src\proxy\drawlog.cpp"
set SRC=%SRC% "%ROOT%src\proxy\hook.cpp"
set SRC=%SRC% "%ROOT%src\common\log.cpp"
set SRC=%SRC% "%ROOT%src\common\config.cpp"
set SRC=%SRC% "%ROOT%src\vr\xr_runtime.cpp"
set SRC=%SRC% "%ROOT%src\vr\vr_math.cpp"
set SRC=%SRC% "%ROOT%src\vr\vr_session.cpp"
set SRC=%SRC% "%ROOT%src\vr\vr_submit.cpp"
set SRC=%SRC% "%ROOT%src\proxy\logic_host.cpp"
set SRC=%SRC% "%ROOT%src\vr\hud.cpp"
set SRC=%SRC% "%ROOT%src\vr\vr_pad.cpp"
set SRC=%SRC% "%ROOT%src\vr\pad_script.cpp"
set SRC=%SRC% "%ROOT%src\vr\dpad_gesture.cpp"
set SRC=%SRC% "%ROOT%src\vr\god_seat.cpp"
set SRC=%SRC% "%ROOT%src\vr\perf_log.cpp"
set SRC=%SRC% "%ROOT%src\proxy\overlay.cpp"

rem Dear ImGui (third_party\imgui): the desktop settings overlay.
set "IMGUI=%ROOT%third_party\imgui"
for %%F in (imgui imgui_draw imgui_tables imgui_widgets imgui_impl_dx9 imgui_impl_win32) do call set SRC=%%SRC%% "%IMGUI%\%%F.cpp"

rem OpenXR: headers and a static loader (third_party\openxr). Linking the
rem loader in means there is no DLL of ours to go missing; a missing or
rem broken runtime fails xrCreateInstance at run time and the game runs flat.
set "OPENXR=%ROOT%third_party\openxr\include"
set "OPENXRLIB=%ROOT%third_party\openxr\lib\win32\openxr_loader.lib"
rem The VR interop header comes from our DXVK fork (vendored) and pulls in Vulkan;
rem the vendored Vulkan headers are used.
set "DXVKVR=%ROOT%dxvk-psyvr\src\d3d9"
set "VKINC=%ROOT%third_party\vulkan\include"

set CFLAGS_COMMON=/nologo /c /EHsc /MT /O2 /Zi /FS /W4 /std:c++17 /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /I"%OPENXR%" /I"%IMGUI%" /I"%DXVKVR%" /I"%VKINC%"
set CFLAGS=%CFLAGS_COMMON% /Fo"%OUT%\\" /Fd"%OUT%\proxy_vc.pdb"
rem /DEBUG + /OPT:REF,ICF: a PDB next to the DLL for resolving hang/crash addresses, same code size.
rem /PDBALTPATH: the DLL records only the PDB's file name, not the build
rem machine's full path (which would carry the user name).
set LFLAGS=/nologo /DLL /DEBUG /PDBALTPATH:%%_PDB%% /OPT:REF /OPT:ICF /MACHINE:X86 /DEF:"%ROOT%src\proxy\d3d9.def" /OUT:"%OUT%\d3d9.dll"

rem `build.bat logic` builds only psyvr_logic.dll (the fast path while
rem hot-reloading it into a running game).
if /i "%~1"=="logic" goto logic

echo Compiling...
del /q "%OUT%\*.obj" 2>nul
cl %CFLAGS% %SRC%
if errorlevel 1 (
    echo BUILD FAILED ^(compile^)
    exit /b 1
)

echo Linking...
rem Deliberately NOT linking d3d9.lib: this DLL *is* d3d9.dll, and an import
rem library would make it import from itself. Every entry point is resolved
rem from the backend with GetProcAddress instead.
link %LFLAGS% "%OUT%\*.obj" "%OPENXRLIB%" dxguid.lib user32.lib kernel32.lib advapi32.lib pathcch.lib ole32.lib windowscodecs.lib uuid.lib
if errorlevel 1 (
    echo BUILD FAILED ^(link^)
    exit /b 1
)

copy /y "%OUT%\d3d9.dll" "%BIN%\d3d9.dll" >nul
echo.
echo Built: %BIN%\d3d9.dll

:logic
rem The draw logic, a second DLL the shim loads (and, with [debug]
rem hot_reload, reloads): stereo.cpp and god mode's
rem per-frame logic (god_frame.cpp, god_seat.cpp) behind the C interface of src\common\logic_abi.h. No OpenXR, ImGui or D3D import.
set "LOGICOUT=%OUT%\logic"
if not exist "%LOGICOUT%" mkdir "%LOGICOUT%"
set LSRC=
set LSRC=%LSRC% "%ROOT%src\logic\logic_main.cpp"
set LSRC=%LSRC% "%ROOT%src\vr\stereo.cpp"
set LSRC=%LSRC% "%ROOT%src\vr\vr_math.cpp"
set LSRC=%LSRC% "%ROOT%src\vr\god_frame.cpp"
set LSRC=%LSRC% "%ROOT%src\vr\god_seat.cpp"

echo Compiling logic...
del /q "%LOGICOUT%\*.obj" 2>nul
cl %CFLAGS_COMMON% /Fo"%LOGICOUT%\\" /Fd"%LOGICOUT%\logic_vc.pdb" %LSRC%
if errorlevel 1 (
    echo BUILD FAILED ^(logic compile^)
    exit /b 1
)

echo Linking logic...
link /nologo /DLL /DEBUG /PDBALTPATH:%%_PDB%% /OPT:REF /OPT:ICF /MACHINE:X86 /DEF:"%ROOT%src\logic\psyvr_logic.def" /OUT:"%OUT%\psyvr_logic.dll" "%LOGICOUT%\*.obj" user32.lib kernel32.lib
if errorlevel 1 (
    echo BUILD FAILED ^(logic link^)
    exit /b 1
)

copy /y "%OUT%\psyvr_logic.dll" "%BIN%\psyvr_logic.dll" >nul
echo Built: %BIN%\psyvr_logic.dll
echo Use install.bat to put both in the game folder.
exit /b 0
