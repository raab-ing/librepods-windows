@echo off
REM activate-v3-driver.cmd — replace all stale librepods-kmdf packages with the current build.
setlocal
set "SCRIPT_DIR=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%activate-v3-driver.ps1"
exit /b %ERRORLEVEL%
