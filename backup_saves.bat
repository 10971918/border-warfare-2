@echo off
rem Copies your Borderlands 2 saves and MW2 profile/saves to backups\<date_time>\. Run before any in-game test.
rem Restore = copy a backup folder's contents back over the original folder (with both games closed).
setlocal
cd /d "%~dp0"
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%t
for /f "delims=" %%d in ('powershell -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')"') do set DOCS=%%d
call "%~dp0paths.bat"
set OUT=%~dp0backups\%STAMP%
robocopy "%DOCS%\My Games\Borderlands 2\WillowGame\SaveData" "%OUT%\BL2_SaveData" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (echo BL2 save backup FAILED & exit /b 1)
robocopy "%MW2%\players" "%OUT%\MW2_players" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (echo MW2 players backup FAILED & exit /b 1)
echo Saves backed up to %OUT%
