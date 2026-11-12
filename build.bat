@echo off
rem Build savestate.dll and GTA5-Savestates.exe (x64) with the MSVC Build Tools.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\recon mkdir build\recon
if "%1"=="recon" goto recon
if "%1"=="test" goto test
ml64 /nologo /c /Fobuild\stack.obj src\stack.asm >nul || exit /b 1
rem /GS-: our frames are on every saved stack, and a stack cookie made in one game session fails in the next.
cl /nologo /O2 /MT /LD /EHsc /std:c++17 /W3 /DNDEBUG /Zi /GS- src\main.cpp src\common.cpp src\addr.cpp src\arena.cpp src\threads.cpp src\memory.cpp src\kobjects.cpp src\timewarp.cpp src\audio.cpp src\d3d.cpp src\overlay.cpp src\census.cpp src\store.cpp src\locks.cpp src\foreign.cpp src\state.cpp /Fobuild\ /Fdbuild\ /Fe:build\savestate.dll /link build\stack.obj /DEBUG /OPT:REF /OPT:ICF user32.lib kernel32.lib ole32.lib winmm.lib d3d11.lib gdi32.lib psapi.lib || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DNDEBUG /DUNICODE /D_UNICODE src\launcher.cpp /Fobuild\ /Fe:build\GTA5-Savestates.exe /link /SUBSYSTEM:WINDOWS user32.lib kernel32.lib shell32.lib ole32.lib advapi32.lib || exit /b 1
if not exist dist mkdir dist
copy /y build\savestate.dll dist\ >nul && copy /y build\GTA5-Savestates.exe dist\ >nul && copy /y README.md dist\ >nul
if not exist dist\savestate.ini copy /y savestate.ini dist\ >nul
echo built build\savestate.dll and build\GTA5-Savestates.exe, copied to dist\
exit /b 0

:recon
cl /nologo /O2 /MT /LD /EHsc /std:c++17 /W3 tools\recon\recon.cpp /Fobuild\recon\ /Fe:build\recon\recon.asi /link user32.lib kernel32.lib ole32.lib || exit /b 1
echo built build\recon\recon.asi
exit /b 0

:test
if not exist build\test mkdir build\test
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 tests\arena_test.cpp src\arena.cpp src\addr.cpp /Fobuild\test\ /Fe:build\test\arena_test.exe /link kernel32.lib || exit /b 1
build\test\arena_test.exe
