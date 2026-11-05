# SPDX-License-Identifier: Apache-2.0
# Build OpenColorIO for Media Player Pro on Windows, static with the dynamic CRT, so the
# player links it into the exe and has no extra DLLs to ship or sign. CI and the signing
# build both call this; pass the printed folder to CMake as -DMP_OCIO_ROOT.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build-ocio-win.ps1 -Root C:\ci-cache\ocio
#
# The result is cached under -Root by the vcpkg commit, so a box builds it once (about 15 min).
# Keep $VcpkgCommit pinned: it fixes the OpenColorIO version (2.5.1) and its dependencies.
param(
    [Parameter(Mandatory = $true)][string]$Root
)
$ErrorActionPreference = "Stop"
$VcpkgCommit = "5d90b0d5d0317336e65662f2bf0d671b0902c632"
$Triplet = "x64-windows-static-md"

$vcpkgDir = Join-Path $Root "vcpkg-$($VcpkgCommit.Substring(0, 10))"
$installRoot = Join-Path $Root "installed-$($VcpkgCommit.Substring(0, 10))"
$prefix = Join-Path $installRoot $Triplet
$marker = Join-Path $prefix "share\opencolorio\OpenColorIOConfig.cmake"

if (Test-Path $marker) {
    Write-Host "OpenColorIO: cached at $prefix"
    Write-Output $prefix
    exit 0
}

New-Item -ItemType Directory -Force $Root | Out-Null
if (-not (Test-Path (Join-Path $vcpkgDir ".git"))) {
    & git clone -q https://github.com/microsoft/vcpkg $vcpkgDir
    if ($LASTEXITCODE) { throw "git clone vcpkg failed" }
}
& git -C $vcpkgDir fetch -q origin $VcpkgCommit
if ($LASTEXITCODE) { throw "git fetch vcpkg failed" }
& git -C $vcpkgDir checkout -q $VcpkgCommit
if ($LASTEXITCODE) { throw "git checkout vcpkg failed" }

$exe = Join-Path $vcpkgDir "vcpkg.exe"
if (-not (Test-Path $exe)) {
    & cmd /c "`"$vcpkgDir\bootstrap-vcpkg.bat`" -disableMetrics"
    if ($LASTEXITCODE -or -not (Test-Path $exe)) { throw "vcpkg bootstrap failed" }
}

# Classic mode: the player's own manifest, if any, must not steer this.
Push-Location $vcpkgDir
try {
    & $exe install "opencolorio:$Triplet" --classic "--x-install-root=$installRoot" --clean-after-build --disable-metrics
    if ($LASTEXITCODE) { throw "vcpkg install opencolorio failed" }
} finally {
    Pop-Location
}
if (-not (Test-Path $marker)) { throw "OpenColorIO package not found at $marker" }
Write-Host "OpenColorIO: built at $prefix"
Write-Output $prefix
