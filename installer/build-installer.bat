@echo off
:: SPDX-License-Identifier: BSL-1.0
:: Build the Windows NSIS installer for the Stereo Media Player demo.
:: Requires the app to be built first (cmake --build), so its exe + staged
:: runtime DLLs (SDL3 + FFmpeg) sit in the build dir.
setlocal
set "REPO=%~dp0.."
set "OUT_DIR=%~dp0"
if "%OUT_DIR:~-1%"=="\" set "OUT_DIR=%OUT_DIR:~0,-1%"

:: Locate the built binary. VS multi-config drops it in build\Release; the Ninja
:: single-config build (CI) drops it in build\. Honor a caller-set BIN_DIR first.
if "%BIN_DIR%"=="" (
    if exist "%REPO%\build\Release\mediaplayer_handle_vk_win.exe" (
        set "BIN_DIR=%REPO%\build\Release"
    ) else (
        set "BIN_DIR=%REPO%\build"
    )
)

if "%VERSION%"=="" set "VERSION=0.0.0"
:: Derive MAJOR/MINOR/PATCH from VERSION (e.g. 1.4.2 -> 1 / 4 / 2) so callers
:: (CI on a v* tag) only need to set VERSION.
for /f "tokens=1,2,3 delims=." %%a in ("%VERSION%") do (
    set "VERSION_MAJOR=%%a"
    set "VERSION_MINOR=%%b"
    set "VERSION_PATCH=%%c"
)
if "%VERSION_MAJOR%"=="" set "VERSION_MAJOR=0"
if "%VERSION_MINOR%"=="" set "VERSION_MINOR=0"
if "%VERSION_PATCH%"=="" set "VERSION_PATCH=0"

if not exist "%BIN_DIR%\mediaplayer_handle_vk_win.exe" (
    echo ERROR: demo binary not found at %BIN_DIR%\mediaplayer_handle_vk_win.exe
    echo Build it first: cmake --build build --config Release
    exit /b 1
)

set "MAKENSIS=C:\Program Files (x86)\NSIS\makensis.exe"
if not exist "%MAKENSIS%" set "MAKENSIS=makensis.exe"

"%MAKENSIS%" /DVERSION=%VERSION% /DVERSION_MAJOR=%VERSION_MAJOR% /DVERSION_MINOR=%VERSION_MINOR% /DVERSION_PATCH=%VERSION_PATCH% "/DBIN_DIR=%BIN_DIR%" "/DSOURCE_DIR=%REPO%" "/DOUTPUT_DIR=%OUT_DIR%" "%~dp0DisplayXRMediaPlayerInstaller.nsi" || exit /b 1

echo === DONE ===
echo Installer: %OUT_DIR%\DisplayXRMediaPlayerSetup-%VERSION%.exe
