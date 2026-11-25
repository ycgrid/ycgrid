@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  YCGrid one-step install (double-click to run; no administrator needed)
rem    install.cmd             copy to %LOCALAPPDATA%\Programs\YCGrid, set the .ycg file association and put
rem                            YCGrid on the desktop and in the Start menu
rem    install.cmd /portable   don't copy; point the .ycg association at ycgrid.exe in this folder
rem    install.cmd /silent     no prompts (for company-wide deployment)
rem    install.cmd /nodesktop  no desktop shortcut (the Start menu one is still made)
rem  Only writes HKCU (current user). Run uninstall.cmd to remove it.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "DEST=%LOCALAPPDATA%\Programs\YCGrid"
set "SILENT=0"
set "PORTABLE=0"
set "NODESKTOP=0"
for %%A in (%*) do (
  if /I "%%~A"=="/silent" set "SILENT=1"
  if /I "%%~A"=="/portable" set "PORTABLE=1"
  if /I "%%~A"=="/nodesktop" set "NODESKTOP=1"
)
set "FLAG="
if "%SILENT%"=="1" set "FLAG=--silent"
if "%NODESKTOP%"=="1" set "FLAG=!FLAG! --no-desktop"

if not exist "%SRC%\ycgrid.exe" (
  set "ERR=ycgrid.exe not found: run install.cmd from the extracted YCGrid folder."
  goto :fail
)

if "%PORTABLE%"=="1" (
  set "TARGET=%SRC%"
) else (
  echo [YCGrid] Installing to %DEST%
  robocopy "%SRC%" "%DEST%" /E /NFL /NDL /NJH /NJS /NP >nul
  if !errorlevel! geq 8 (
    set "ERR=Copying files failed (robocopy !errorlevel!). If YCGrid is running, close it and try again."
    goto :fail
  )
  rem an upgrade leaves nothing of the old version in YCGrid's own folders: they are made the same as this one's
  rem (runtime keeps the packages you installed, examples keeps your changes)
  for %%D in (python skill mcp licenses) do (
    if exist "%SRC%\%%D" (
      robocopy "%SRC%\%%D" "%DEST%\%%D" /MIR /NFL /NDL /NJH /NJS /NP >nul
      if !errorlevel! geq 8 (
        set "ERR=Copying %%D failed (robocopy !errorlevel!). If YCGrid is running, close it and try again."
        goto :fail
      )
    )
  )
  rem the demo and the trading sample of versions before examples\ (now examples\demo and examples\txo_market_maker)
  if exist "%SRC%\examples" (
    if exist "%DEST%\demo\demo.ycg" rmdir /s /q "%DEST%\demo"
    if exist "%DEST%\samples\txo_market_maker\txo_mm.ycg" rmdir /s /q "%DEST%\samples"
  )
  set "TARGET=%DEST%"
)

echo [YCGrid] Setting the .ycg file association and the shortcuts
start "" /wait "%TARGET%\ycgrid.exe" --install %FLAG%
if not "!errorlevel!"=="0" (
  set "ERR=Setting the file association failed (ycgrid.exe --install returned !errorlevel!)."
  goto :fail
)

echo [YCGrid] Done: double-click a .ycg file to open it with YCGrid.
if "%SILENT%"=="0" pause
endlocal & exit /b 0

:fail
echo.
echo [YCGrid] Failed: %ERR%
if "%SILENT%"=="0" pause
endlocal & exit /b 1
