@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "PROJECT=%SCRIPT_DIR%LibrePodsKmdf.vcxproj"
set "ROOT_DIR=%SCRIPT_DIR%"
set "CONFIGURATION=Release"
set "PLATFORM=x64"
set "TRANSPORT_MODE=Stub"
if "%VS_BASE%"=="" set "VS_BASE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "DEV_CMD=%VS_BASE%\Common7\Tools\LaunchDevCmd.bat"
set "MSBUILD=%VS_BASE%\MSBuild\Current\Bin\amd64\MSBuild.exe"
set "OUTPUT_DIR=%SCRIPT_DIR%bin\%CONFIGURATION%\%PLATFORM%"
set "PACKAGE_DIR=%OUTPUT_DIR%\LibrePodsKmdf"
set "ARTIFACTS_ROOT=%SCRIPT_DIR%artifacts"
set "PACKAGE_INF_SOURCE=librepods-kmdf-root.inf"
set "APPDATA=%ROOT_DIR%.appdata"
set "NUGET_PACKAGES=%ROOT_DIR%.nuget\packages"
set "NUGET_HTTP_CACHE_PATH=%ROOT_DIR%.nuget\http-cache"
set "NUGET_PLUGINS_CACHE_PATH=%ROOT_DIR%.nuget\plugins-cache"
set "RESTORE_CONFIG=%SCRIPT_DIR%nuget.config"
set "WDK_PACKAGE_BIN_X64=%NUGET_PACKAGES%\microsoft.windows.wdk.x64\10.0.26100.6584\c\bin\10.0.26100.0\x64"
set "WDK_PACKAGE_BIN_X86=%NUGET_PACKAGES%\microsoft.windows.wdk.x64\10.0.26100.6584\c\bin\10.0.26100.0\x86"
set "SDK_PACKAGE_BIN_X64=%NUGET_PACKAGES%\microsoft.windows.sdk.cpp\10.0.26100.1\c\bin\10.0.26100.0\x64"
set "SDK_PACKAGE_BIN_X86=%NUGET_PACKAGES%\microsoft.windows.sdk.cpp\10.0.26100.1\c\bin\10.0.26100.0\x86"

if /I "%~1"=="loopback" set "TRANSPORT_MODE=Loopback"
if /I "%~1"=="stub" set "TRANSPORT_MODE=Stub"
if /I "%~1"=="bthl2cap" set "TRANSPORT_MODE=BthL2Cap"
if /I "%TRANSPORT_MODE%"=="BthL2Cap" set "PACKAGE_INF_SOURCE=librepods-kmdf-bthenum.inf"

if not exist "%PROJECT%" (
    echo Driver project not found: %PROJECT%
    exit /b 1
)

if not exist "%DEV_CMD%" goto :missing_devcmd
goto :devcmd_ok
:missing_devcmd
echo LaunchDevCmd.bat was not found: %DEV_CMD%
echo Set VS_BASE to your Visual Studio 2022 Build Tools installation root.
exit /b 1
:devcmd_ok

if not exist "%MSBUILD%" goto :missing_msbuild
goto :msbuild_ok
:missing_msbuild
echo MSBuild.exe was not found: %MSBUILD%
exit /b 1
:msbuild_ok

if not exist "%NUGET_PACKAGES%" mkdir "%NUGET_PACKAGES%"
if not exist "%APPDATA%\NuGet" mkdir "%APPDATA%\NuGet"
if not exist "%NUGET_HTTP_CACHE_PATH%" mkdir "%NUGET_HTTP_CACHE_PATH%"
if not exist "%NUGET_PLUGINS_CACHE_PATH%" mkdir "%NUGET_PLUGINS_CACHE_PATH%"

if not exist "%RESTORE_CONFIG%" (
    echo NuGet config was not found: %RESTORE_CONFIG%
    exit /b 1
)

call "%DEV_CMD%" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %ERRORLEVEL%

set "APPDATA=%ROOT_DIR%.appdata"
set "NUGET_PACKAGES=%ROOT_DIR%.nuget\packages"
set "NUGET_HTTP_CACHE_PATH=%ROOT_DIR%.nuget\http-cache"
set "NUGET_PLUGINS_CACHE_PATH=%ROOT_DIR%.nuget\plugins-cache"
set "PATH=%WDK_PACKAGE_BIN_X64%;%WDK_PACKAGE_BIN_X86%;%SDK_PACKAGE_BIN_X64%;%SDK_PACKAGE_BIN_X86%;%PATH%"

echo Restoring WDK/SDK NuGet packages...
"%MSBUILD%" "%PROJECT%" /t:Restore /p:RestoreConfigFile=%RESTORE_CONFIG%;RestorePackagesPath=%NUGET_PACKAGES%
if errorlevel 1 exit /b %ERRORLEVEL%

rem Assemble a local VCTargets overlay: VS's v170 toolset + the WDK NuGet's
rem ImportAfter files + our WindowsKernelModeDriver10.0 toolset shim. This
rem replaces what a full WDK install would drop into the VS tree.
set "REAL_VCTARGETS=%VS_BASE%\MSBuild\Microsoft\VC\v170"
if not exist "%REAL_VCTARGETS%\Microsoft.Cpp.Common.props" goto :missing_vctargets
goto :vctargets_ok
:missing_vctargets
echo Visual Studio VC targets not found: %REAL_VCTARGETS%
exit /b 1
:vctargets_ok
set "VCTARGETS_OVERLAY=%ROOT_DIR%.vctargets"
if not exist "%VCTARGETS_OVERLAY%\Microsoft.Cpp.Common.props" (
    echo Assembling local VCTargets overlay from "%REAL_VCTARGETS%"...
    robocopy "%REAL_VCTARGETS%" "%VCTARGETS_OVERLAY%" /E /NFL /NDL /NJH /NJS /NC /NS >nul
    if errorlevel 8 exit /b 1
)
set "KMDF_TOOLSET_DIR=%VCTARGETS_OVERLAY%\Platforms\%PLATFORM%\PlatformToolsets\WindowsKernelModeDriver10.0"
if not exist "%KMDF_TOOLSET_DIR%" mkdir "%KMDF_TOOLSET_DIR%"
copy /Y "%SCRIPT_DIR%..\toolset\Toolset.props" "%KMDF_TOOLSET_DIR%\Toolset.props" >nul
copy /Y "%SCRIPT_DIR%..\toolset\Toolset.targets" "%KMDF_TOOLSET_DIR%\Toolset.targets" >nul
set "WDK_IMPORT_AFTER=%NUGET_PACKAGES%\microsoft.windows.wdk.x64\10.0.26100.6584\c\build\10.0.26100.0\%PLATFORM%\ImportAfter"
set "OVERLAY_IMPORT_AFTER=%VCTARGETS_OVERLAY%\Platforms\%PLATFORM%\ImportAfter"
if not exist "%OVERLAY_IMPORT_AFTER%" mkdir "%OVERLAY_IMPORT_AFTER%"
copy /Y "%WDK_IMPORT_AFTER%\*" "%OVERLAY_IMPORT_AFTER%\" >nul

echo Building %PROJECT% [%CONFIGURATION%^|%PLATFORM%] transport=%TRANSPORT_MODE%...
"%MSBUILD%" "%PROJECT%" /t:Build /p:Configuration=%CONFIGURATION%;Platform=%PLATFORM%;LibrePodsTransportMode=%TRANSPORT_MODE%;SignMode=Off;VCTargetsPath=%VCTARGETS_OVERLAY%\;VCTargetsPath17=%VCTARGETS_OVERLAY%\;WDKContentRoot=%NUGET_PACKAGES%\microsoft.windows.wdk.x64\10.0.26100.6584\c\;SDKContentRoot=%NUGET_PACKAGES%\microsoft.windows.sdk.cpp\10.0.26100.1\c\
if errorlevel 1 exit /b %ERRORLEVEL%

set "PFX=%SCRIPT_DIR%certs\librepods-test.pfx"
if not exist "%PFX%" goto :no_pfx
goto :have_pfx
:no_pfx
echo Test signing PFX not found: %PFX%
echo Create it with the New-SelfSignedCertificate command shown in README.md.
exit /b 1
:have_pfx

if not exist "%PACKAGE_DIR%" mkdir "%PACKAGE_DIR%"
copy /Y "%OUTPUT_DIR%\%PACKAGE_INF_SOURCE%" "%PACKAGE_DIR%\librepods-kmdf.inf" >nul
copy /Y "%OUTPUT_DIR%\librepods-kmdf.sys" "%PACKAGE_DIR%\librepods-kmdf.sys" >nul

echo Generating catalog and test-signing driver package...
inf2cat /driver:"%PACKAGE_DIR%" /os:10_X64 /uselocaltime
if errorlevel 1 exit /b %ERRORLEVEL%

signtool sign /fd SHA256 /f "%PFX%" /p librepods-test "%OUTPUT_DIR%\librepods-kmdf.sys" "%PACKAGE_DIR%\librepods-kmdf.sys" "%PACKAGE_DIR%\librepods-kmdf.cat"
if errorlevel 1 exit /b %ERRORLEVEL%
copy /Y "%SCRIPT_DIR%certs\librepods-test.cer" "%OUTPUT_DIR%\librepods-kmdf.cer" >nul

set "MODE_ARTIFACT_DIR=%ARTIFACTS_ROOT%\%TRANSPORT_MODE%"
set "MODE_PACKAGE_DIR=%MODE_ARTIFACT_DIR%\package"

if not exist "%ARTIFACTS_ROOT%" mkdir "%ARTIFACTS_ROOT%"
if not exist "%MODE_ARTIFACT_DIR%" mkdir "%MODE_ARTIFACT_DIR%"
if not exist "%MODE_PACKAGE_DIR%" mkdir "%MODE_PACKAGE_DIR%"

copy /Y "%OUTPUT_DIR%\librepods-kmdf.sys" "%MODE_ARTIFACT_DIR%\librepods-kmdf.sys" >nul
copy /Y "%OUTPUT_DIR%\librepods-kmdf.pdb" "%MODE_ARTIFACT_DIR%\librepods-kmdf.pdb" >nul
copy /Y "%OUTPUT_DIR%\librepods-kmdf.cer" "%MODE_ARTIFACT_DIR%\librepods-kmdf.cer" >nul
copy /Y "%PACKAGE_DIR%\librepods-kmdf.inf" "%MODE_PACKAGE_DIR%\librepods-kmdf.inf" >nul
copy /Y "%PACKAGE_DIR%\librepods-kmdf.sys" "%MODE_PACKAGE_DIR%\librepods-kmdf.sys" >nul
copy /Y "%PACKAGE_DIR%\librepods-kmdf.cat" "%MODE_PACKAGE_DIR%\librepods-kmdf.cat" >nul

echo Archived signed driver artifacts to %MODE_ARTIFACT_DIR%
exit /b 0
