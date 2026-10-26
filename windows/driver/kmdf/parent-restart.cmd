@echo off
set "LOG=%~dp0parent-restart.log"
echo === Parent restart %DATE% %TIME% === > "%LOG%"
pnputil /restart-device "BTHENUM\DEV_AC0775A48F56\7&6A05F28&0&BLUETOOTHDEVICE_AC0775A48F56" >> "%LOG%" 2>&1
echo exit=%ERRORLEVEL% >> "%LOG%"
type "%LOG%"
