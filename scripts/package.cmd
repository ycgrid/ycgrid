@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  YCGrid one-step packaging (for developers; PLAN §13.16)
rem    scripts\package.cmd [--with-python] [/silent]
rem  1) run build.cmd (Release)  2) lay out the release files in dist\YCGrid\
rem  3) --with-python: download a pinned uv and install a portable CPython into dist\YCGrid\runtime\python
rem  4) zip it into dist\YCGrid-<version>.zip with the tar.exe built into Windows
rem  Only uses tools built into Windows 10 1803 and later (curl.exe, tar.exe, robocopy); no PowerShell.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "WITH_PY=0"
set "SILENT=0"
for %%A in (%*) do (
  if /I "%%~A"=="--with-python" set "WITH_PY=1"
  if /I "%%~A"=="/silent" set "SILENT=1"
)
set /p VER=<"%ROOT%\VERSION"
set "LOCK=%ROOT%\third_party\deps.lock"
set "DIST=%ROOT%\dist"
set "STAGE=%DIST%\YCGrid"
set "ZIP=%DIST%\YCGrid-%VER%.zip"
echo [YCGrid] Packaging version %VER%

rem --- 1. Build ------------------------------------------------------------------
call "%~dp0build.cmd" release /silent
if not "!errorlevel!"=="0" (
  set "ERR=The build failed, see the messages above."
  goto :fail
)
set "EXE=%ROOT%\build\msvc-release\apps\ycgrid\ycgrid.exe"
if not exist "%EXE%" (
  set "ERR=%EXE% not found."
  goto :fail
)

rem --- 2. Lay out the release files ------------------------------------------------------------
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\licenses" || (set "ERR=Couldn't create %STAGE%." & goto :fail)
copy /y "%EXE%" "%STAGE%\ycgrid.exe" >nul
copy /y "%ROOT%\packaging\install.cmd" "%STAGE%\install.cmd" >nul
copy /y "%ROOT%\packaging\uninstall.cmd" "%STAGE%\uninstall.cmd" >nul
copy /y "%ROOT%\packaging\README.txt" "%STAGE%\README.txt" >nul
copy /y "%ROOT%\LICENSE" "%STAGE%\licenses\ycgrid-LICENSE.txt" >nul
copy /y "%ROOT%\third_party\imgui\LICENSE.txt" "%STAGE%\licenses\imgui-LICENSE.txt" >nul
copy /y "%ROOT%\third_party\implot\LICENSE" "%STAGE%\licenses\implot-LICENSE.txt" >nul
copy /y "%ROOT%\third_party\fluent-icons\LICENSE" "%STAGE%\licenses\fluent-icons-LICENSE.txt" >nul
copy /y "%ROOT%\third_party\uv-LICENSE-MIT.txt" "%STAGE%\licenses\uv-LICENSE.txt" >nul
copy /y "%ROOT%\NOTICE" "%STAGE%\licenses\NOTICE.txt" >nul
copy /y "%ROOT%\THIRD-PARTY-NOTICES.md" "%STAGE%\licenses\THIRD-PARTY-NOTICES.md" >nul
rem the libraries linked into ycgrid.exe (vcpkg keeps each license as share\<name>\copyright)
set "VSHARE=%ROOT%\build\vcpkg_installed\x64-windows-static\share"
for %%P in (freetype pugixml nlohmann-json zlib zstd) do (
  if exist "%VSHARE%\%%P\copyright" (
    copy /y "%VSHARE%\%%P\copyright" "%STAGE%\licenses\%%P-LICENSE.txt" >nul
  ) else (
    set "ERR=The license of %%P was not found in %VSHARE%."
    goto :fail
  )
)
rem Python interface (yc package), the skill for AI agents, the MCP tool list
robocopy "%ROOT%\python\yc" "%STAGE%\python\yc" /E /XD __pycache__ /NFL /NDL /NJH /NJS /NP >nul
robocopy "%ROOT%\skill" "%STAGE%\skill" /E /NFL /NDL /NJH /NJS /NP >nul
robocopy "%ROOT%\mcp" "%STAGE%\mcp" /E /NFL /NDL /NJH /NJS /NP >nul
rem the tool definitions read by the MCP server (yc.mcp) go next to the package
copy /y "%ROOT%\mcp\tools.json" "%STAGE%\python\yc\mcp\tools.json" >nul
rem the examples that README.txt describes (every one a project folder, with lib, the code they share), and
rem open_demo.cmd (as in dist_release)
robocopy "%ROOT%\examples" "%STAGE%\examples" /E /XD __pycache__ .ycgrid /NFL /NDL /NJH /NJS /NP >nul
(
  echo @echo off
  echo chcp 65001 ^>nul
  echo rem Opens the demo project with this folder's ycgrid.exe ^(the Python code is in examples\demo\main.py^)
  echo start "" "%%~dp0ycgrid.exe" "%%~dp0examples\demo\demo.ycg"
) > "%STAGE%\open_demo.cmd"
for %%F in (ycgrid.exe open_demo.cmd examples\demo\demo.ycg examples\txo_market_maker\txo_mm.ycg examples\backtest_report\backtest_report.ycg examples\lib\ex_util.py) do (
  if not exist "%STAGE%\%%F" (
    set "ERR=%%F is missing from %STAGE%."
    goto :fail
  )
)
if not exist "%STAGE%\ycgrid.exe" (
  set "ERR=Copying files failed."
  goto :fail
)

rem --- 3. Portable Python (optional) ---------------------------------------------------
if "%WITH_PY%"=="1" (
  call :python
  if not "!errorlevel!"=="0" goto :fail
)

rem --- 4. Compress -----------------------------------------------------------------
if exist "%ZIP%" del /q "%ZIP%"
"%SystemRoot%\System32\tar.exe" -a -c -f "%ZIP%" -C "%DIST%" YCGrid
if not "!errorlevel!"=="0" (
  set "ERR=Compression failed (tar.exe)."
  goto :fail
)
rem the release is not code-signed: its SHA-256 goes next to it, for the release page and for users to check
call :sha256 "%ZIP%"
if not "!errorlevel!"=="0" goto :fail
echo [YCGrid] Done: %ZIP% (SHA-256 !HASH!)
endlocal & exit /b 0

rem ============================================================================
:sha256
set "HASH="
for /f "skip=1 delims=" %%H in ('%SystemRoot%\System32\certutil.exe -hashfile "%~1" SHA256') do if not defined HASH set "HASH=%%H"
if not defined HASH (
  set "ERR=certutil couldn't hash %~nx1."
  exit /b 1
)
set "HASH=!HASH: =!"
> "%~1.sha256" <nul set /p "=!HASH!  %~nx1"
exit /b 0

rem ============================================================================
:python
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"uv.url=" "%LOCK%"') do set "UV_URL=%%B"
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"python.version=" "%LOCK%"') do set "PY_VER=%%B"
if not defined UV_URL (
  set "ERR=deps.lock has no uv.url."
  exit /b 1
)
set "WORK=%DIST%\_work"
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%"
mkdir "%STAGE%\runtime"
echo [YCGrid] Downloading uv: %UV_URL%
"%SystemRoot%\System32\curl.exe" -sSL --fail -o "%WORK%\uv.zip" "%UV_URL%"
if not "!errorlevel!"=="0" (
  set "ERR=Downloading uv failed; check the network connection."
  exit /b 1
)
"%SystemRoot%\System32\tar.exe" -xf "%WORK%\uv.zip" -C "%STAGE%\runtime"
if not exist "%STAGE%\runtime\uv.exe" (
  set "ERR=Extracting uv failed."
  exit /b 1
)
rem caches and temporary files go to dist\_work, never the user's %%LOCALAPPDATA%%\uv; --no-registry: no Windows registry entry (PEP 514), the system Python is untouched
set "UV_CACHE_DIR=%WORK%\cache"
set "UV_PYTHON_INSTALL_DIR=%WORK%\python"
echo [YCGrid] Installing portable CPython %PY_VER%
"%STAGE%\runtime\uv.exe" python install %PY_VER% --install-dir "%WORK%\python" --no-bin --no-registry
if not "!errorlevel!"=="0" (
  set "ERR=uv python install failed."
  exit /b 1
)
set "PY_SRC="
for /d %%D in ("%WORK%\python\cpython-%PY_VER%*") do set "PY_SRC=%%~fD"
if not defined PY_SRC (
  set "ERR=The installed CPython %PY_VER% was not found."
  exit /b 1
)
robocopy "%PY_SRC%" "%STAGE%\runtime\python" /E /NFL /NDL /NJH /NJS /NP >nul
if !errorlevel! geq 8 (
  set "ERR=Copying the Python runtime failed."
  exit /b 1
)
rem this runtime belongs to YCGrid, it is not the system Python: remove the "externally managed" marker so uv / pip can install packages
if exist "%STAGE%\runtime\python\Lib\EXTERNALLY-MANAGED" del /q "%STAGE%\runtime\python\Lib\EXTERNALLY-MANAGED"
"%STAGE%\runtime\python\python.exe" -E -s -c "import sys; print('[YCGrid] Python', sys.version.split()[0], sys.prefix)"
if not "!errorlevel!"=="0" (
  set "ERR=The portable Python doesn't run."
  exit /b 1
)
> "%STAGE%\runtime\python\.yc-python.json" echo {"version": "%PY_VER%", "source": "python-build-standalone via uv", "abi": "cp312"}
rmdir /s /q "%WORK%"
exit /b 0

:fail
echo.
echo [YCGrid] Failed: %ERR%
if "%SILENT%"=="0" pause
endlocal & exit /b 1
