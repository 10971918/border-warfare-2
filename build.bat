@echo off
rem Builds Border Warfare 2 into bin\ (MSVC Build Tools 2022, low priority).
rem   build.bat            everything
rem   build.bat recon      only the Phase 0b recon probe
setlocal
cd /d "%~dp0"
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build
set OPTS=/nologo /O2 /MT /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN
for %%d in (bin bin\obj bin\obj\x86 bin\obj\x64 bin\mw_recon bin\mw_guest) do if not exist %%d mkdir %%d

rem ---- x64: MW2 (iw4sp.exe) side + layout test ----
setlocal
call "%VS%\vcvars64.bat" >nul 2>nul || (echo vcvars64 failed & exit /b 1)
del /q bin\mw_recon\winmm.dll bin\mw_guest\winmm.dll bin\layout_test_x64.exe 2>nul
start "" /low /wait /b cl %OPTS% /LD recon_dll\mw_recon\mw_recon.cpp /Fobin\obj\x64\ /Febin\mw_recon\winmm.dll /link user32.lib
if not exist bin\mw_recon\winmm.dll (echo BUILD FAILED: mw_recon & exit /b 1)
if /i "%1"=="recon" goto done64
start "" /low /wait /b cl %OPTS% /LD src\mw_guest\main.cpp /Fobin\obj\x64\ /Febin\mw_guest\winmm.dll /link user32.lib
if not exist bin\mw_guest\winmm.dll (echo BUILD FAILED: mw_guest & exit /b 1)
start "" /low /wait /b cl %OPTS% tests\layout_test.cpp /Fobin\obj\x64\ /Febin\layout_test_x64.exe
if not exist bin\layout_test_x64.exe (echo BUILD FAILED: layout_test_x64 & exit /b 1)
del /q bin\field_test.exe 2>nul
start "" /low /wait /b cl %OPTS% tests\field_test.cpp /Fobin\obj\x64\ /Febin\field_test.exe
if not exist bin\field_test.exe (echo BUILD FAILED: field_test & exit /b 1)
del /q bin\despeckle_test.exe 2>nul
start "" /low /wait /b cl %OPTS% tests\despeckle_test.cpp /Fobin\obj\x64\ /Febin\despeckle_test.exe /link user32.lib
if not exist bin\despeckle_test.exe (echo BUILD FAILED: despeckle_test & exit /b 1)
del /q bin\walk_test.exe 2>nul
start "" /low /wait /b cl %OPTS% tests\walk_test.cpp /Fobin\obj\x64\ /Febin\walk_test.exe
if not exist bin\walk_test.exe (echo BUILD FAILED: walk_test & exit /b 1)
del /q bin\bw2_scan_x64.dll 2>nul
start "" /low /wait /b cl %OPTS% /LD src\bl_scan\scan.cpp /Fobin\obj\x64\ /Febin\bw2_scan_x64.dll
if not exist bin\bw2_scan_x64.dll (echo BUILD FAILED: bw2_scan_x64 & exit /b 1)
:done64
endlocal
if /i "%1"=="recon" goto done

rem ---- x86: BL2 side + layout test ----
setlocal
call "%VS%\vcvars32.bat" >nul 2>nul || (echo vcvars32 failed & exit /b 1)
del /q bin\layout_test_x86.exe 2>nul
start "" /low /wait /b cl %OPTS% tests\layout_test.cpp /Fobin\obj\x86\ /Febin\layout_test_x86.exe
if not exist bin\layout_test_x86.exe (echo BUILD FAILED: layout_test_x86 & exit /b 1)
del /q bin\bw2_scan.dll 2>nul
start "" /low /wait /b cl %OPTS% /LD src\bl_scan\scan.cpp /Fobin\obj\x86\ /Febin\bw2_scan.dll
if not exist bin\bw2_scan.dll (echo BUILD FAILED: bw2_scan x86 & exit /b 1)
del /q bin\bw2_overlay.dll 2>nul
start "" /low /wait /b cl %OPTS% /LD src\bl_overlay\overlay.cpp /Fobin\obj\x86\ /Febin\bw2_overlay.dll /link user32.lib
if not exist bin\bw2_overlay.dll (echo BUILD FAILED: bw2_overlay & exit /b 1)
del /q bin\hook_loop_test.exe 2>nul
start "" /low /wait /b cl %OPTS% tests\hook_loop_test.cpp /Fobin\obj\x86\ /Febin\hook_loop_test.exe /link user32.lib
if not exist bin\hook_loop_test.exe (echo BUILD FAILED: hook_loop_test & exit /b 1)
endlocal

:done
echo BUILD OK
