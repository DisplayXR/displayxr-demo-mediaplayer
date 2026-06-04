#!/usr/bin/env bash
# SPDX-License-Identifier: BSL-1.0
# Regenerate src/rhi/Shaders.h (embedded SPIR-V) from shaders/*.glsl.
# Requires glslangValidator (Homebrew: `brew install glslang`).
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_DIR}"

glslangValidator -V shaders/fullscreen.vert --vn g_fullscreenVertSpv -o /tmp/_v.h
glslangValidator -V shaders/sbs.frag        --vn g_sbsFragSpv        -o /tmp/_f.h

{
  cat <<'HDR'
// SPDX-License-Identifier: BSL-1.0
// Embedded SPIR-V for the SBS textured-blit shaders.
//
// GENERATED — do not edit by hand. Regenerate with scripts/gen_shaders.sh after
// changing shaders/fullscreen.vert or shaders/sbs.frag.
#pragma once

#include <cstdint>

HDR
  grep -v -E '^\s*//|#pragma once' /tmp/_v.h
  echo
  grep -v -E '^\s*//|#pragma once' /tmp/_f.h
} > src/rhi/Shaders.h

echo "Wrote src/rhi/Shaders.h"
