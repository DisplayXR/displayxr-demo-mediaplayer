#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Regenerate src/rhi/Shaders.h (embedded SPIR-V) from shaders/*.glsl.
# Requires glslangValidator (Homebrew: `brew install glslang`).
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_DIR}"

glslangValidator -V shaders/fullscreen.vert --vn g_fullscreenVertSpv -o /tmp/_v.h
# -DMP_GRADE: the desktop build carries the Pro grade + QC views (see shaders/sbs.frag).
# The Android leg compiles shaders/sbs.frag itself, without it.
glslangValidator -V -DMP_GRADE shaders/sbs.frag --vn g_sbsFragSpv  -o /tmp/_f.h
# The GPU v210 decode (#238); its arithmetic is shaders/v210_decode.inc, shared with the C++
# reference in src/media/V210.h.
glslangValidator -V shaders/v210_to_p016.comp --vn g_v210ToP016CompSpv -o /tmp/_c.h

{
  cat <<'HDR'
// SPDX-License-Identifier: Apache-2.0
// Embedded SPIR-V for the SBS textured-blit shaders and the v210 decode pass.
//
// GENERATED — do not edit by hand. Regenerate with scripts/gen_shaders.sh after
// changing shaders/fullscreen.vert, shaders/sbs.frag or shaders/v210_*.
#pragma once

#include <cstdint>

HDR
  grep -v -E '^\s*//|#pragma once' /tmp/_v.h
  echo
  grep -v -E '^\s*//|#pragma once' /tmp/_f.h
  echo
  grep -v -E '^\s*//|#pragma once' /tmp/_c.h
} > src/rhi/Shaders.h

echo "Wrote src/rhi/Shaders.h"
