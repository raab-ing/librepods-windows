@echo off
setlocal
set "LOG=%~dp0rebind.log"
set "NODE=BTHENUM\{74ec2172-0bad-4d01-8f77-997b2be0722a}_VID&0001004c_PID&2024\7&6a05f28&0&AC0775A48F56_C00000000"
echo === Rebind AACP PDO to newest driver %DATE% %TIME% === > "%LOG%"
echo disable >> "%LOG%"
pnputil /disable-device "%NODE%" >> "%LOG%" 2>&1
echo enable >> "%LOG%"
pnputil /enable-device "%NODE%" >> "%LOG%" 2>&1
echo restart >> "%LOG%"
pnputil /restart-device "%NODE%" >> "%LOG%" 2>&1
echo current binding: >> "%LOG%"
pnputil /enum-devices /class Bluetooth >> "%LOG%" 2>&1
echo DONE >> "%LOG%"
type "%LOG%" | findstr /i "fail\|error\|restart\|librepods\|oem"
pause
