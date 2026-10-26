@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%SCRIPT_DIR%"
set "ORIGINAL_ARGS=%*"
set "MODE=Stub"
set "PACKAGE_DIR=%SCRIPT_DIR%artifacts\%MODE%\package"
set "INF=%PACKAGE_DIR%\librepods-kmdf.inf"
set "CERT=%SCRIPT_DIR%artifacts\%MODE%\librepods-kmdf.cer"
set "INSTALL_CERT=0"
set "ENABLE_TESTSIGNING=0"
set "ALREADY_ELEVATED=0"
set "HARDWARE_ID=Root\LibrePodsKmdf"
set "AIRPODS_PROFILE_SERVICE_GUID_PRIMARY=4715650b-5e9d-4ac2-b898-a4fc0aa5df78"
set "AIRPODS_PROFILE_SERVICE_GUID_SECONDARY=74ec2172-0bad-4d01-8f77-997b2be0722a"
set "DEVCON=%ROOT_DIR%\.nuget\packages\microsoft.windows.wdk.x64\10.0.26100.6584\c\tools\10.0.26100.0\x64\devcon.exe"
set "REBOOT_REQUIRED=0"
set "REBOOT_AFTER=0"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="stub" set "MODE=Stub" & shift & goto parse_args
if /I "%~1"=="loopback" set "MODE=Loopback" & shift & goto parse_args
if /I "%~1"=="bthl2cap" set "MODE=BthL2Cap" & shift & goto parse_args
if /I "%~1"=="/trustcert" set "INSTALL_CERT=1" & shift & goto parse_args
if /I "%~1"=="/enabletestsigning" set "ENABLE_TESTSIGNING=1" & shift & goto parse_args
if /I "%~1"=="/reboot" set "REBOOT_AFTER=1" & shift & goto parse_args
if /I "%~1"=="/elevated" set "ALREADY_ELEVATED=1" & shift & goto parse_args
if /I "%~1"=="/?" goto usage
if /I "%~1"=="-h" goto usage
if /I "%~1"=="--help" goto usage
echo Unknown argument: %~1
goto usage

:args_done

set "PACKAGE_DIR=%SCRIPT_DIR%artifacts\%MODE%\package"
set "INF=%PACKAGE_DIR%\librepods-kmdf.inf"
set "CERT=%SCRIPT_DIR%artifacts\%MODE%\librepods-kmdf.cer"

if not exist "%INF%" (
    echo Built driver package not found: %INF%
    echo Run driver-build.cmd %MODE% first.
    exit /b 1
)

call :ensure_admin
set "ENSURE_ADMIN_ERROR=%ERRORLEVEL%"
if "%ENSURE_ADMIN_ERROR%"=="200" exit /b 0
if not "%ENSURE_ADMIN_ERROR%"=="0" exit /b %ENSURE_ADMIN_ERROR%

if "%INSTALL_CERT%"=="1" (
    if not exist "%CERT%" (
        echo Driver certificate not found: %CERT%
        exit /b 1
    )

    echo Importing test certificate into Root...
    certutil -f -addstore Root "%CERT%"
    if errorlevel 1 exit /b %ERRORLEVEL%

    echo Importing test certificate into TrustedPublisher...
    certutil -f -addstore TrustedPublisher "%CERT%"
    if errorlevel 1 exit /b %ERRORLEVEL%
)

if "%ENABLE_TESTSIGNING%"=="1" (
    echo Enabling Windows TESTSIGNING boot mode...
    bcdedit /set testsigning on
    if errorlevel 1 (
        echo Failed to enable TESTSIGNING.
        echo Secure Boot may be blocking this change. Disable Secure Boot first if necessary.
        exit /b %ERRORLEVEL%
    )

    set "REBOOT_REQUIRED=1"
)

echo Installing LibrePods KMDF driver package [%MODE%]...
if /I "%MODE%"=="BthL2Cap" goto install_bthenum

if not exist "%DEVCON%" (
    echo DevCon was not found, falling back to pnputil /install.
    pnputil /add-driver "%INF%" /install
    set "INSTALL_ERROR=%ERRORLEVEL%"
    goto done
)

pnputil /add-driver "%INF%"
if errorlevel 1 (
    set "INSTALL_ERROR=%ERRORLEVEL%"
    goto done
)

"%DEVCON%" findall =root | findstr /i /c:"ROOT\\LIBREPODSKMDF" >nul
if errorlevel 1 (
    echo Creating root-enumerated LibrePods device...
    "%DEVCON%" install "%INF%" "%HARDWARE_ID%"
    set "INSTALL_ERROR=%ERRORLEVEL%"
    goto done
)

echo Updating existing root-enumerated LibrePods device...
"%DEVCON%" update "%INF%" "%HARDWARE_ID%"
set "INSTALL_ERROR=%ERRORLEVEL%"
goto done

:install_bthenum
call :has_airpods_profile_service "%AIRPODS_PROFILE_SERVICE_GUID_PRIMARY%"
if not errorlevel 1 goto install_bthenum_package

call :has_airpods_profile_service "%AIRPODS_PROFILE_SERVICE_GUID_SECONDARY%"
if errorlevel 1 (
    echo No paired Apple AirPods control-profile PDO was found for %AIRPODS_PROFILE_SERVICE_GUID_PRIMARY% or %AIRPODS_PROFILE_SERVICE_GUID_SECONDARY%.
    echo Pair the AirPods in Windows first, open the case once so BTHENUM enumerates the service, and rerun this script.
    exit /b 1
)

:install_bthenum_package
pnputil /add-driver "%INF%" /install
set "INSTALL_ERROR=%ERRORLEVEL%"

:done
if not "%INSTALL_ERROR%"=="0" exit /b %INSTALL_ERROR%

if "%REBOOT_REQUIRED%"=="1" (
    echo TESTSIGNING was enabled. Reboot is required before Windows will load the test-signed LibrePods driver.
    if "%REBOOT_AFTER%"=="1" (
        echo Rebooting now...
        shutdown /r /t 0
        exit /b 0
    )

    exit /b 3010
)

exit /b 0

:ensure_admin
fltmc >nul 2>&1
if not errorlevel 1 exit /b 0

if "%ALREADY_ELEVATED%"=="1" (
    echo Failed to obtain Administrator rights.
    exit /b 1
)

echo Requesting Administrator approval...
set "ELEVATE_VBS=%TEMP%\librepods-elevate-%RANDOM%%RANDOM%.vbs"
> "%ELEVATE_VBS%" echo Set UAC = CreateObject("Shell.Application")
>> "%ELEVATE_VBS%" echo UAC.ShellExecute "cmd.exe", "/c """"%~f0"""" %ORIGINAL_ARGS% /elevated", "", "runas", 1
cscript //nologo "%ELEVATE_VBS%" >nul
set "ELEVATE_ERROR=%ERRORLEVEL%"
del "%ELEVATE_VBS%" >nul 2>&1
if not "%ELEVATE_ERROR%"=="0" (
    echo Failed to start the elevated installer.
    exit /b %ELEVATE_ERROR%
)

exit /b 200

:has_airpods_profile_service
reg query HKLM\SYSTEM\CurrentControlSet\Enum\BTHENUM /s /f %~1 | findstr /i /c:"0001004c" >nul
exit /b %ERRORLEVEL%

:usage
echo Usage: install-driver.cmd [stub^|loopback^|bthl2cap] [/trustcert] [/enabletestsigning] [/reboot]
echo.
echo   /trustcert         Imports the generated test certificate into Root and TrustedPublisher.
echo   /enabletestsigning Enables Windows TESTSIGNING boot mode. Reboot is required afterward.
echo   /reboot            Reboots automatically after enabling TESTSIGNING.
exit /b 1
