@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  YCGrid one-step removal
rem    uninstall.cmd           remove the .ycg file association and the desktop and Start menu shortcuts, and ask whether to delete the install folder
rem    uninstall.cmd /silent   don't ask: delete %LOCALAPPDATA%\Programs\YCGrid too
rem  Your workbooks (.ycg) and Python code are not deleted.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "DEST=%LOCALAPPDATA%\Programs\YCGrid"
set "SILENT=0"
for %%A in (%*) do if /I "%%~A"=="/silent" set "SILENT=1"
set "FLAG="
if "%SILENT%"=="1" set "FLAG=--silent"

set "EXE=%HERE%\ycgrid.exe"
if not exist "%EXE%" set "EXE=%DEST%\ycgrid.exe"
if exist "%EXE%" (
  echo [YCGrid] Removing the file association and shortcuts
  start "" /wait "%EXE%" --uninstall %FLAG%
) else (
  echo [YCGrid] ycgrid.exe not found, skipping the file association removal.
)

if not exist "%DEST%" goto :done
set "REMOVE=1"
if "%SILENT%"=="0" (
  choice /C YN /M "Also delete the install folder %DEST%"
  if errorlevel 2 set "REMOVE=0"
)
if "%REMOVE%"=="0" goto :done
echo [YCGrid] Deleting %DEST%
if /I "%HERE%"=="%DEST%" (
  rem this very file runs from the install folder: leave the batch file first, then delete
  cd /d "%TEMP%"
  (goto) 2>nul & rmdir /s /q "%DEST%" & echo [YCGrid] Done.
)
rmdir /s /q "%DEST%"

:done
echo [YCGrid] Done.
if "%SILENT%"=="0" pause
endlocal & exit /b 0
