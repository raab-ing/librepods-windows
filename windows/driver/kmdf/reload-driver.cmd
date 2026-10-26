@echo off
setlocal
set "LOG=%~dp0reload.log"
echo === Parent subtree restart %DATE% %TIME% === > "%LOG%"
echo Restarting BTHENUM parent (tears down both PDOs)... >> "%LOG%"
pnputil /restart-device "BTHENUM\DEV_AC0775A48F56\7&6A05F28&0&BLUETOOTHDEVICE_AC0775A48F56" >> "%LOG%" 2>&1
echo --- fallback: scan for new hardware to rebind >> "%LOG%"
pnputil /scan-devices >> "%LOG%" 2>&1
echo DONE >> "%LOG%"
type "%LOG%"
pause
