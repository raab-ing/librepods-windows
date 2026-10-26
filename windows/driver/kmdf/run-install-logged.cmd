@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "LOG=%SCRIPT_DIR%install.log"
echo === LibrePods test-driver install %DATE% %TIME% === > "%LOG%"
call "%SCRIPT_DIR%install-driver.cmd" bthl2cap /trustcert /enabletestsigning >> "%LOG%" 2>&1
echo EXIT=%ERRORLEVEL% >> "%LOG%"
echo.
type "%LOG%"
echo.
echo Log written to %LOG%
pause
