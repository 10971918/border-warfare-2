@echo off
rem Installs the built Border Warfare 2 files into both games. uninstall.bat removes them again.
rem   install.bat           current milestone: MW2 guest winmm.dll + BL2 "Border Warfare 2" sdkmod
rem   install.bat mw        only the MW2 guest DLL (Borderlands 2 may stay open)
rem   install.bat bl        only the BL2 mod (Modern Warfare 2 may stay open)
rem   install.bat recon     Phase 0b probes (read-only): MW2 winmm.dll probe + BL2 "BW2 Recon" sdkmod
rem Double-clicked, it waits for a key at the end so you can read the result (set BW2_NOPAUSE=1 to skip).
setlocal
cd /d "%~dp0"
call "%~dp0paths.bat"
set RESULT=0
if /i not "%1"=="bl" tasklist /fi "imagename eq iw4sp.exe" | find /i "iw4sp.exe" >nul && (echo FAILED: Modern Warfare 2 is still running - quit it completely, then run this again. & set RESULT=1 & goto end)
if not exist "%MW2%\iw4sp.exe" (echo FAILED: MW2 not found at %MW2% - edit paths.bat & set RESULT=1 & goto end)
if not exist "%BL2%\Binaries\Win32\Plugins\pyunrealsdk.dll" (echo FAILED: BL2 Python SDK not found at %BL2% & set RESULT=1 & goto end)
if exist "%MW2%\winmm.dll" if not exist "%MW2%\bw2.ini" (echo FAILED: a winmm.dll that isn't ours is in the MW2 folder - not touching it. & set RESULT=1 & goto end)

set LOGS=%~dp0logs
if not exist "%LOGS%" mkdir "%LOGS%"
type nul > "%LOGS%\mw_cmd.txt"
type nul > "%LOGS%\bl_cmd.txt"

if /i "%1"=="recon" goto recon
if /i "%1"=="mw" goto mw
if /i "%1"=="bl" goto bl
if "%1"=="" goto main
echo Usage: install.bat [recon^|mw^|bl]
set RESULT=1
goto end

:main
call :mwdll || goto end
set RESULT_MW= + MW2 guest
:bl
if exist "%BL2%\sdk_mods\bw2_recon" rmdir /s /q "%BL2%\sdk_mods\bw2_recon"
if not exist "%BL2%\sdk_mods\bw2" mkdir "%BL2%\sdk_mods\bw2"
copy /y sdk_mods\bw2\*.py "%BL2%\sdk_mods\bw2\" >nul || (echo FAILED: copying the BL2 mod & set RESULT=1 & goto end)
copy /y sdk_mods\bw2\pyproject.toml "%BL2%\sdk_mods\bw2\" >nul
copy /y tools\bw2_proto.py "%BL2%\sdk_mods\bw2\" >nul
copy /y tools\bw2_coords.py "%BL2%\sdk_mods\bw2\" >nul
fc /b bin\bw2_scan.dll "%BL2%\sdk_mods\bw2\bw2_scan.dll" >nul 2>nul || copy /y bin\bw2_scan.dll "%BL2%\sdk_mods\bw2\" >nul || echo WARNING: bw2_scan.dll is in use by Borderlands 2 - quit BL2 and run this again to update it.
fc /b bin\bw2_overlay.dll "%BL2%\sdk_mods\bw2\bw2_overlay.dll" >nul 2>nul || copy /y bin\bw2_overlay.dll "%BL2%\sdk_mods\bw2\" >nul || echo WARNING: bw2_overlay.dll is in use by Borderlands 2 - quit BL2 and run this again to update it.
(echo LogDir=%LOGS%) > "%BL2%\sdk_mods\bw2\bw2.ini"
del /q "%BL2%\sdk_mods\bw2\bw2_overlay_?.dll" >nul 2>nul
echo OK: installed Border Warfare 2 (BL2 mod%RESULT_MW%). Logs: %LOGS%
tasklist /fi "imagename eq Borderlands2.exe" | find /i "Borderlands2.exe" >nul && echo (Borderlands 2 is running: its mod changes apply the next time it starts.)
goto end

:recon
if not exist bin\mw_recon\winmm.dll (echo FAILED: run build.bat recon first. & set RESULT=1 & goto end)
copy /y bin\mw_recon\winmm.dll "%MW2%\winmm.dll" >nul || (echo FAILED: copying winmm.dll & set RESULT=1 & goto end)
(echo [BW2]& echo LogDir=%LOGS%) > "%MW2%\bw2.ini"
if not exist "%BL2%\sdk_mods\bw2_recon" mkdir "%BL2%\sdk_mods\bw2_recon"
copy /y sdk_mods\bw2_recon\*.py "%BL2%\sdk_mods\bw2_recon\" >nul
copy /y sdk_mods\bw2_recon\pyproject.toml "%BL2%\sdk_mods\bw2_recon\" >nul
(echo LogDir=%LOGS%) > "%BL2%\sdk_mods\bw2_recon\bw2.ini"
echo OK: installed the Phase 0b recon probes. Logs: %LOGS%
goto end

:mw
call :mwdll || goto end
echo OK: installed the MW2 guest DLL.
goto end

:mwdll
if not exist bin\mw_guest\winmm.dll (echo FAILED: run build.bat first. & set RESULT=1 & exit /b 1)
copy /y bin\mw_guest\winmm.dll "%MW2%\winmm.dll" >nul || (echo FAILED: couldn't replace winmm.dll - is MW2 still closing? Wait a moment and run this again. & set RESULT=1 & exit /b 1)
(echo [BW2]& echo LogDir=%LOGS%) > "%MW2%\bw2.ini"
exit /b 0

:end
if not defined BW2_NOPAUSE echo %cmdcmdline% | find /i "%~nx0" >nul && pause
exit /b %RESULT%
