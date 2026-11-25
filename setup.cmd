@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  YCGrid one-step setup from source: installs everything this PC needs, builds YCGrid and installs it
rem    setup.cmd              ask before installing Git or Visual Studio, then build and install
rem    setup.cmd /yes         install what is missing without asking
rem    setup.cmd /noinstall   stop after the build: run dist_release\ycgrid.exe, nothing is installed
rem    setup.cmd /silent      no prompts and no pause (implies /yes)
rem  1) Git and the Visual Studio C++ build tools with vcpkg, installed with winget when missing
rem  2) the ImGui / ImPlot sources at the commits pinned in third_party\deps.lock
rem  3) YCGrid's own portable Python, as in a build from source (scripts\setup_runtime.cmd; the system Python is untouched)
rem  4) release build + unit tests (scripts\build.cmd), laid out in dist_release\
rem  5) install for the current user (dist_release\install.cmd): %LOCALAPPDATA%\Programs\YCGrid, the .ycg file
rem     association and desktop and Start menu shortcuts; this step needs no administrator rights
rem  Installing Visual Studio shows the Visual Studio Installer's own administrator prompt.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
rem error codes are always checked with if not "!errorlevel!"=="0": the linker may return -1.
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "YES=0"
set "SILENT=0"
set "INSTALL=1"
for %%A in (%*) do (
  if /I "%%~A"=="/yes" set "YES=1"
  if /I "%%~A"=="/silent" (
    set "SILENT=1"
    set "YES=1"
  )
  if /I "%%~A"=="/noinstall" set "INSTALL=0"
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSSETUP=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe"
set "VC=Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
set "VCPKG=Microsoft.VisualStudio.Component.Vcpkg"
set "CMAKEVS=Microsoft.VisualStudio.Component.VC.CMake.Project"

echo [YCGrid] Setting up YCGrid in %ROOT%

rem --- 1a. Git --------------------------------------------------------------------
where git >nul 2>&1
if not "!errorlevel!"=="0" if exist "%ProgramFiles%\Git\cmd\git.exe" set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
where git >nul 2>&1
if not "!errorlevel!"=="0" (
  call :need_winget || goto :fail
  call :ask "Git is not installed. Install it with winget" || (set "ERR=Git is needed: https://git-scm.com/download/win" & goto :fail)
  echo [YCGrid] Installing Git
  winget install --id Git.Git --exact --source winget --silent --accept-package-agreements --accept-source-agreements
  set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
  where git >nul 2>&1
  if not "!errorlevel!"=="0" (
    set "ERR=Installing Git failed; install it from https://git-scm.com/download/win and run setup.cmd again."
    goto :fail
  )
)
for /f "usebackq delims=" %%V in (`git --version`) do echo [YCGrid] %%V

rem --- 1b. Visual Studio C++ tools with vcpkg -------------------------------------------
call :find_vs
if not defined VSDIR (
  set "ANYVS="
  set "PRODUCT="
  if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "ANYVS=%%I"
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property productId`) do set "PRODUCT=%%I"
  )
  if defined ANYVS (
    rem Visual Studio is there but without the C++ tools or vcpkg: add them to it
    set "WORKLOAD=Microsoft.VisualStudio.Workload.NativeDesktop"
    if /I "!PRODUCT!"=="Microsoft.VisualStudio.Product.BuildTools" set "WORKLOAD=Microsoft.VisualStudio.Workload.VCTools"
    call :ask "Visual Studio in !ANYVS! lacks the C++ tools or vcpkg. Add them" || (set "ERR=The Visual Studio C++ tools and vcpkg are needed." & goto :fail)
    echo [YCGrid] Adding the C++ tools and vcpkg to !ANYVS! ^(this takes a while^)
    start "" /wait "%VSSETUP%" modify --installPath "!ANYVS!" --add !WORKLOAD! --includeRecommended --add %VCPKG% --add %CMAKEVS% --passive --norestart
  ) else (
    call :need_winget || goto :fail
    call :ask "Visual Studio 2026 Build Tools (C++ and vcpkg, several GB) are not installed. Install them with winget" || (set "ERR=The Visual Studio C++ tools and vcpkg are needed: https://visualstudio.microsoft.com/downloads/" & goto :fail)
    echo [YCGrid] Installing Visual Studio 2026 Build Tools ^(this takes a while^)
    winget install --id Microsoft.VisualStudio.BuildTools --exact --source winget --accept-package-agreements --accept-source-agreements --override "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add %VCPKG% --add %CMAKEVS%"
  )
  call :find_vs
  if not defined VSDIR (
    set "ERR=The Visual Studio C++ tools with vcpkg are still missing. Install Visual Studio 2026 with Desktop development with C++ and the vcpkg package manager, then run setup.cmd again. If the installer asked for a restart, restart first."
    goto :fail
  )
)
echo [YCGrid] Visual Studio: %VSDIR%

rem --- 2. ImGui / ImPlot sources ------------------------------------------------------
pushd "%ROOT%"
if exist ".git" (
  git submodule update --init --recursive
  if not "!errorlevel!"=="0" (
    popd
    set "ERR=git submodule update failed; make sure GitHub is reachable."
    goto :fail
  )
) else (
  rem a source download without .git: fetch the pinned commits listed in deps.lock
  call :fetch imgui imgui.h || (popd & goto :fail)
  call :fetch implot implot.h || (popd & goto :fail)
)
popd

rem --- 3. Portable Python with its packages ---------------------------------------------
call "%ROOT%\scripts\setup_runtime.cmd" /silent
if not "!errorlevel!"=="0" (
  set "ERR=Installing YCGrid's Python failed, see the messages above."
  goto :fail
)

rem --- 4. Build -----------------------------------------------------------------------
call "%ROOT%\scripts\build.cmd" release /silent
if not "!errorlevel!"=="0" (
  set "ERR=The build failed, see the messages above."
  goto :fail
)
if not exist "%ROOT%\dist_release\ycgrid.exe" (
  set "ERR=dist_release\ycgrid.exe was not created."
  goto :fail
)

rem --- 5. Install -----------------------------------------------------------------------
if "%INSTALL%"=="0" (
  echo [YCGrid] Done: run %ROOT%\dist_release\ycgrid.exe
  goto :done
)
call "%ROOT%\dist_release\install.cmd" /silent
if not "!errorlevel!"=="0" (
  set "ERR=Installing failed, see the messages above. If YCGrid is running, close it and run setup.cmd again."
  goto :fail
)
echo [YCGrid] Done: start YCGrid from the Start menu, or double-click a .ycg file.
echo [YCGrid] Installed in %LOCALAPPDATA%\Programs\YCGrid; uninstall.cmd there removes it.

:done
if "%SILENT%"=="0" pause
endlocal & exit /b 0

rem ============================================================================
rem VSDIR = a Visual Studio with the C++ tools, vcpkg and CMake, or empty
:find_vs
set "VSDIR="
if not exist "%VSWHERE%" exit /b 0
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires %VC% %VCPKG% %CMAKEVS% -property installationPath`) do set "VSDIR=%%I"
exit /b 0

rem errorlevel 0 when winget is available
:need_winget
where winget >nul 2>&1
if "!errorlevel!"=="0" exit /b 0
set "ERR=winget was not found. Install App Installer from the Microsoft Store, or install Git and Visual Studio 2026 (Desktop development with C++, vcpkg) yourself, then run setup.cmd again."
exit /b 1

rem errorlevel 0 = yes; /yes and /silent answer yes
:ask
if "%YES%"=="1" exit /b 0
choice /c YN /m "[YCGrid] %~1"
if "!errorlevel!"=="1" exit /b 0
exit /b 1

rem :fetch <name> <header>: clone <name>.url from deps.lock into <name>.path and check out <name>.commit
:fetch
set "F_URL="
set "F_PATH="
set "F_COMMIT="
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"%~1.url=" "%ROOT%\third_party\deps.lock"') do set "F_URL=%%B"
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"%~1.path=" "%ROOT%\third_party\deps.lock"') do set "F_PATH=%%B"
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"%~1.commit=" "%ROOT%\third_party\deps.lock"') do set "F_COMMIT=%%B"
set "F_PATH=!F_PATH:/=\!"
if exist "%ROOT%\!F_PATH!\%~2" exit /b 0
if exist "%ROOT%\!F_PATH!" rmdir /s /q "%ROOT%\!F_PATH!"
echo [YCGrid] Fetching %~1 !F_COMMIT!
git clone --quiet "!F_URL!" "%ROOT%\!F_PATH!"
if not "!errorlevel!"=="0" (
  set "ERR=Cloning !F_URL! failed; make sure GitHub is reachable."
  exit /b 1
)
git -C "%ROOT%\!F_PATH!" checkout --quiet --detach !F_COMMIT!
if not "!errorlevel!"=="0" (
  set "ERR=Commit !F_COMMIT! of %~1 was not found."
  exit /b 1
)
exit /b 0

:fail
echo.
echo [YCGrid] Failed: %ERR%
if "%SILENT%"=="0" pause
endlocal & exit /b 1
