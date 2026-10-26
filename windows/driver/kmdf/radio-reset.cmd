@echo off
set "LOG=%~dp0radio-reset.log"
echo === Radio reset %DATE% %TIME% === > "%LOG%"
pnputil /disable-device "USB\VID_10D7&PID_B012\ACTIONS1234" >> "%LOG%" 2>&1
echo disable exit=%ERRORLEVEL% >> "%LOG%"
timeout /t 3 /nobreak >> "%LOG%" 2>&1
pnputil /enable-device "USB\VID_10D7&PID_B012\ACTIONS1234" >> "%LOG%" 2>&1
echo enable exit=%ERRORLEVEL% >> "%LOG%"
type "%LOG%"
