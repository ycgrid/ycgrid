@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  YCGrid C++ line coverage of the Catch2 unit tests (OpenCppCoverage)
rem    scripts\coverage.cmd [debug|release] [catch2 test filter]
rem  Needs OpenCppCoverage (winget install OpenCppCoverage.OpenCppCoverage) and a
rem  build of that configuration (scripts\build.cmd debug /silent). Writes
rem  build\coverage\<config>\index.html (per file, per line) and cobertura.xml
rem  (for tools), and prints the per-directory summary (scripts\coverage_summary.py).
rem  Debug measures every line; release is optimized and undercounts inlined code.
rem ============================================================================
setlocal EnableDelayedExpansion
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=debug"
set "FILTER=%~2"
set "OCC=%ProgramFiles%\OpenCppCoverage\OpenCppCoverage.exe"
if not exist "%OCC%" (
  echo [coverage] OpenCppCoverage not found: winget install OpenCppCoverage.OpenCppCoverage
  exit /b 1
)
set "EXE=%ROOT%\build\msvc-%CONFIG%\tests\yc_unit_tests.exe"
if not exist "%EXE%" (
  echo [coverage] %EXE% not found: run scripts\build.cmd %CONFIG% /silent first
  exit /b 1
)
set "OUT=%ROOT%\build\coverage\%CONFIG%"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
set "PY=%ROOT%\dist_%CONFIG%\runtime\python\python.exe"
if not exist "%PY%" set "PY=%ROOT%\dist_release\runtime\python\python.exe"
echo [coverage] %CONFIG% unit tests under OpenCppCoverage...
pushd "%ROOT%\build\msvc-%CONFIG%\tests"
"%OCC%" --quiet --sources "%ROOT%\src" --sources "%ROOT%\apps" --excluded_sources "%ROOT%\src\core\geo_data.inc" --excluded_sources "%ROOT%\src\core\i18n_zh.inc" --export_type "html:%OUT%" --export_type "cobertura:%OUT%\cobertura.xml" -- "%EXE%" %FILTER%
set "RC=!errorlevel!"
popd
if not "!RC!"=="0" (
  echo [coverage] the unit tests failed under the coverage runner: exit code !RC! ^(the report still covers what ran^)
)
if not exist "%OUT%\cobertura.xml" (
  echo [coverage] no report was written
  exit /b 1
)
"%PY%" -E -s "%ROOT%\scripts\coverage_summary.py" "%OUT%\cobertura.xml"
echo [coverage] report: %OUT%\index.html
exit /b !RC!
