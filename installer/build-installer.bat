@echo off
:: SPDX-License-Identifier: Apache-2.0
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

REM ── Code signing (gated on %SIGN_CMD%) ──────────────────────────────
REM Signing is OFF unless SIGN_CMD is set in the environment (a signing-
REM capable build/release machine points it at the configured signer).
REM This repo carries NO cert and NO secret. A clone without SIGN_CMD
REM builds unsigned, exactly as before.
REM
REM The shipped binaries (demo exe + bundled SDL3 + FFmpeg DLLs) MUST be
REM signed BEFORE makensis packs them — Smart App Control checks the
REM binaries extracted at install/load time, so signing the finished
REM installer alone is not enough. The installer .exe and the uninstaller
REM are signed at makensis time by the .nsi (!finalize + the two-pass
REM uninstaller block), driven by the /DSIGN_CMD passed below.
REM SIGN_ARG carries its own quotes so the empty (unsigned) case expands to
REM nothing rather than an empty quoted "" arg that makensis would choke on.
set "SIGN_ARG="
if defined SIGN_CMD (
    echo === Signing demo binaries [SIGN_CMD set] ===
    powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO%\scripts\sign-release.ps1" -Path "%BIN_DIR%" -SignCmd "%SIGN_CMD%" || exit /b 1
    set SIGN_ARG="/DSIGN_CMD=%SIGN_CMD%"
) else (
    echo === Signing skipped [SIGN_CMD not set] - installer will be UNSIGNED ===
)

"%MAKENSIS%" /DVERSION=%VERSION% /DVERSION_MAJOR=%VERSION_MAJOR% /DVERSION_MINOR=%VERSION_MINOR% /DVERSION_PATCH=%VERSION_PATCH% "/DBIN_DIR=%BIN_DIR%" "/DSOURCE_DIR=%REPO%" "/DOUTPUT_DIR=%OUT_DIR%" %SIGN_ARG% "%~dp0DisplayXRMediaPlayerInstaller.nsi" || exit /b 1

echo === DONE ===
echo Installer: %OUT_DIR%\DisplayXRMediaPlayerSetup-%VERSION%.exe
