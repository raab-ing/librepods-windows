@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "ORIGINAL_ARGS=%*"
set "MODE=Stub"
set "ALREADY_ELEVATED=0"

::parse_args
if /I "%~1"=="/elevated" set "ALREADY_ELEVATED=1" & shift & goto parse_args
if /I "%~1"=="stub" set "MODE=Stub"
if /I "%~1"=="loopback" set "MODE=Loopback"
if /I "%~1"=="bthl2cap" set "MODE=BthL2Cap"
if /I "%~1"=="/?" goto usage
if /I "%~1"=="-h" goto usage
if /I "%~1"=="--help" goto usage
if not "%~1"=="" if /I not "%~1"=="stub" if /I not "%~1"=="loopback" if /I not "%~1"=="bthl2cap" goto usage

call :ensure_admin
set "ENSURE_ADMIN_ERROR=%ERRORLEVEL%"
if "%ENSURE_ADMIN_ERROR%"=="200" exit /b 0
if not "%ENSURE_ADMIN_ERROR%"=="0" exit /b %ENSURE_ADMIN_ERROR%

echo Preparing LibrePods test driver install [%MODE%]...
call "%SCRIPT_DIR%install-driver.cmd" %MODE% /trustcert /enabletestsigning %2 %3 %4 %5
set "INSTALL_ERROR=%ERRORLEVEL%"

if "%INSTALL_ERROR%"=="3010" (
    echo.
    echo Reboot required. After Windows restarts, rerun this script once to finish loading the test-signed driver cleanly.
    exit /b 0
)

exit /b %INSTALL_ERROR%

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

:usage
echo Usage: install-test-driver.cmd [stub^|loopback^|bthl2cap] [/reboot]
echo.
echo This is the one-shot development installer for test-signed LibrePods drivers.
echo It imports the test certificate, enables TESTSIGNING, and installs the selected driver package.
echo For bthl2cap, pair the AirPods in Windows and open the case once before running the installer.
exit /b 1
