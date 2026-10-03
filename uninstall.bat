@echo off
rem Removes every Border Warfare 2 file from both games (probes and mod). Your saves are not touched.
setlocal
call "%~dp0paths.bat"
tasklist /fi "imagename eq iw4sp.exe" | find /i "iw4sp.exe" >nul && (echo Close Modern Warfare 2 first. & exit /b 1)
tasklist /fi "imagename eq Borderlands2.exe" | find /i "Borderlands2.exe" >nul && (echo Close Borderlands 2 first. & exit /b 1)
rem winmm.dll is only removed when our bw2.ini marker sits next to it
if exist "%MW2%\bw2.ini" (
	del /q "%MW2%\winmm.dll" 2>nul
	del /q "%MW2%\bw2.ini"
)
if exist "%BL2%\sdk_mods\bw2_recon" rmdir /s /q "%BL2%\sdk_mods\bw2_recon"
if exist "%BL2%\sdk_mods\bw2" rmdir /s /q "%BL2%\sdk_mods\bw2"
del /q "%BL2%\Binaries\Win32\Plugins\bw2_host.dll" 2>nul
echo Border Warfare 2 removed from both games.
