@echo off
set "LOG=%~dp0disable-aap.log"
echo === %DATE% %TIME% === > "%LOG%"
pnputil /disable-device "BTHENUM\{74EC2172-0BAD-4D01-8F77-997B2BE0722A}_VID&0001004C_PID&2024\7&6A05F28&0&AC0775A48F56_C00000000" >> "%LOG%" 2>&1
echo disable exit=%ERRORLEVEL% >> "%LOG%"
timeout /t 2 /nobreak >> "%LOG%" 2>&1
pnputil /enable-device "BTHENUM\{74EC2172-0BAD-4D01-8F77-997B2BE0722A}_VID&0001004C_PID&2024\7&6A05F28&0&AC0775A48F56_C00000000" >> "%LOG%" 2>&1
echo enable exit=%ERRORLEVEL% >> "%LOG%"
type "%LOG%"
