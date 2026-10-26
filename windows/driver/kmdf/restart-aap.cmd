@echo off
set "LOG=%~dp0restart-aap.log"
echo === %DATE% %TIME% === > "%LOG%"
pnputil /restart-device "BTHENUM\{74EC2172-0BAD-4D01-8F77-997B2BE0722A}_VID&0001004C_PID&2024\7&6A05F28&0&AC0775A48F56_C00000000" >> "%LOG%" 2>&1
echo aap exit=%ERRORLEVEL% >> "%LOG%"
pnputil /restart-device "BTHENUM\{4715650B-5E9D-4AC2-B898-A4FC0AA5DF78}_VID&0001004C_PID&2024\7&6A05F28&0&AC0775A48F56_C00000000" >> "%LOG%" 2>&1
echo uarps exit=%ERRORLEVEL% >> "%LOG%"
type "%LOG%"
