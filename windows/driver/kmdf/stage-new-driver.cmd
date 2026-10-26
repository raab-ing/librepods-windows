@echo off
set "LOG=%~dp0stage-new.log"
echo === Stage %DATE% %TIME% === > "%LOG%"
pnputil /add-driver "%~dp0bin\Release\x64\LibrePodsKmdf\librepods-kmdf.inf" /install >> "%LOG%" 2>&1
echo add-driver exit=%ERRORLEVEL% >> "%LOG%"
pnputil /restart-device "BTHENUM\DEV_AC0775A48F56\7&6A05F28&0&BLUETOOTHDEVICE_AC0775A48F56" >> "%LOG%" 2>&1
echo restart exit=%ERRORLEVEL% >> "%LOG%"
powershell -NoProfile -Command "(Get-FileHash C:\Windows\System32\drivers\librepods-kmdf.sys -Algorithm SHA256).Hash" >> "%LOG%" 2>&1
type "%LOG%"
