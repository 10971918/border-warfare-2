@echo off
rem Runs every offline test (no game needed). Build first with build.bat.
rem Needs a 64-bit Python 3.10+ on PATH as "python" (standard library only), or set PY to its path.
setlocal
cd /d "%~dp0"
if not defined PY set PY=python
set FAILED=0
for %%t in (layout_test_x64 layout_test_x86 field_test walk_test despeckle_test hook_loop_test) do (
  if not exist bin\%%t.exe (echo MISSING: bin\%%t.exe - run build.bat & set FAILED=1) else (
    bin\%%t.exe >nul 2>&1 && echo ok    %%t || (echo FAIL  %%t & set FAILED=1)
  )
)
for %%t in (test_scan test_native_scan test_bl_mod) do (
  "%PY%" tests\%%t.py >nul 2>&1 && echo ok    %%t || (echo FAIL  %%t ^(run: %PY% tests\%%t.py^) & set FAILED=1)
)
if "%FAILED%"=="0" (echo ALL TESTS PASSED) else (echo SOME TESTS FAILED)
exit /b %FAILED%
