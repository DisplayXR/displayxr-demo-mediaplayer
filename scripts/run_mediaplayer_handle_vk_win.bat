:: SPDX-License-Identifier: BSL-1.0
:: Run the Windows media player against a local DisplayXR dev runtime.
:: NOTE: Windows is scaffolded but unverified in M0 (dev machine is macOS).
:: Use a NON-elevated terminal — elevated processes ignore XR_RUNTIME_JSON and
:: fall back to the HKLM ActiveRuntime.
@echo off
setlocal

set "REPO_DIR=%~dp0.."
set "BIN=%REPO_DIR%\build\mediaplayer_handle_vk_win.exe"

if not defined XR_RUNTIME_JSON (
    set "XR_RUNTIME_JSON=%REPO_DIR%\..\displayxr-runtime\build\openxr_displayxr-dev.json"
)

if not exist "%BIN%" (
    echo error: %BIN% not built. Run: cmake -S . -B build ^&^& cmake --build build
    exit /b 1
)

echo XR_RUNTIME_JSON=%XR_RUNTIME_JSON%
"%BIN%" %*
