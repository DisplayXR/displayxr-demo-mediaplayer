#!/usr/bin/env bash
# SPDX-License-Identifier: BSL-1.0
#
# Run the macOS media player against a local DisplayXR dev runtime.
# Points XR_RUNTIME_JSON at the runtime's dev manifest so the OpenXR loader picks
# the dev build instead of any installed runtime.
#
# Usage: scripts/run_mediaplayer_handle_vk_macos.sh [extra args...]
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_DIR}/build/mediaplayer_handle_vk_macos"

# Default to the sibling runtime checkout's dev manifest; override by exporting
# XR_RUNTIME_JSON before invoking.
: "${XR_RUNTIME_JSON:=${REPO_DIR}/../displayxr-runtime/build/openxr_displayxr-dev.json}"
export XR_RUNTIME_JSON

if [[ ! -f "${XR_RUNTIME_JSON}" ]]; then
    echo "warning: XR_RUNTIME_JSON not found: ${XR_RUNTIME_JSON}" >&2
    echo "         build the runtime or set XR_RUNTIME_JSON to its dev manifest." >&2
fi
if [[ ! -x "${BIN}" ]]; then
    echo "error: ${BIN} not built. Run: cmake -S . -B build -G Ninja && cmake --build build" >&2
    exit 1
fi

echo "XR_RUNTIME_JSON=${XR_RUNTIME_JSON}"
exec "${BIN}" "$@"
