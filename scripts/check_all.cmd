@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  YCGrid: every check before a commit, so a fix doesn't break something else
rem    scripts\check_all.cmd [/quick]
rem  1) release build + Catch2 unit tests  2) Python tests  3) translations, UI command coverage, boundary
rem  4) ui_sweep: every command, every dialog with Enter / Escape, every example screen
rem     feature_check: every feature clicked where the user finds it, and its result checked
rem  5) trader_session: a trader's and a researcher's day, timed (frozen screens, lost clicks)
rem     calc_fuzz: random edits checked against CalculateFull, Ctrl+Z / Ctrl+Y and save / reopen (.ycg, .xlsx)
rem     formula_fuzz: random formulas checked against answers worked out in Python (Excel's rules)
rem  6) debug build + a replay of every seed in scripts\chaos_seeds.txt (seeds that found a problem once)
rem  2), 4), 5) and the seeds run side by side (scripts\check_parallel.py); each job logs to %TEMP%\yc_check_*.txt
rem  /quick stops after 3). A new problem found by chaos.py goes into chaos_seeds.txt with what it found.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
rem error codes are always checked with if not "!errorlevel!"=="0": the linker may return -1.

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "QUICK=0"
for %%A in (%*) do if /I "%%~A"=="/quick" set "QUICK=1"
set "PY=%ROOT%\dist_release\runtime\python\python.exe"
set "FAILED="

echo [check] 1/6 release build and unit tests
call "%ROOT%\scripts\build.cmd" release /silent
if not "!errorlevel!"=="0" (
  set "ERR=the release build or its unit tests failed"
  goto :fail
)

echo [check] 3/6 translations, UI command coverage, boundary
"%PY%" -E -s "%ROOT%\scripts\i18n_build.py" check
if not "!errorlevel!"=="0" set "FAILED=!FAILED! i18n"
"%PY%" -E -s "%ROOT%\scripts\check_commands.py"
if not "!errorlevel!"=="0" set "FAILED=!FAILED! commands"
"%PY%" -E -s "%ROOT%\scripts\check_boundary.py"
if not "!errorlevel!"=="0" set "FAILED=!FAILED! boundary"
"%PY%" -E -s "%ROOT%\scripts\check_model_fields.py"
if not "!errorlevel!"=="0" set "FAILED=!FAILED! model-fields"
"%PY%" -E -s "%ROOT%\scripts\check_excel_parity.py"
if not "!errorlevel!"=="0" set "FAILED=!FAILED! excel-parity"
"%PY%" -E -s "%ROOT%\scripts\check_lessons.py"
if not "!errorlevel!"=="0" set "FAILED=!FAILED! lessons"
if "%QUICK%"=="1" goto :quick_tests

echo [check] 4/6 in parallel: Python tests, feature_check, ui_sweep, calc_fuzz, formula_fuzz, then trader_session
"%PY%" -E -s "%ROOT%\scripts\check_parallel.py"
if not "!errorlevel!"=="0" set "FAILED=!FAILED! parallel-checks"

echo [check] 6/6 debug build and the chaos seeds, in parallel
call "%ROOT%\scripts\build.cmd" debug /silent
if not "!errorlevel!"=="0" (
  set "FAILED=!FAILED! debug-build"
  goto :done
)
"%PY%" -E -s "%ROOT%\scripts\check_parallel.py" --chaos
if not "!errorlevel!"=="0" set "FAILED=!FAILED! chaos"
goto :done

:quick_tests
echo [check] 2/6 Python tests, in parallel
"%PY%" -E -s "%ROOT%\scripts\check_parallel.py" --quick
if not "!errorlevel!"=="0" set "FAILED=!FAILED! python-tests"

:done
if defined FAILED (
  set "ERR=failed:!FAILED!"
  goto :fail
)
echo [check] all passed
endlocal
exit /b 0

:fail
echo [check] !ERR!
endlocal
exit /b 1
