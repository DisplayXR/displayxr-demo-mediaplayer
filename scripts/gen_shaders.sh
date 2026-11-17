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
# The browser's 3D grid (#180, #252): rhi/StereoGridRenderer, the same names as the Android
# leg's build-time headers.
glslangValidator -V --target-env vulkan1.2 shaders/stereo_grid.vert --vn stereo_grid_vert_data -o /tmp/_gv.h
glslangValidator -V --target-env vulkan1.2 shaders/stereo_grid.frag --vn stereo_grid_frag_data -o /tmp/_gf.h

{
  cat <<'HDR'
// SPDX-License-Identifier: Apache-2.0
// Embedded SPIR-V for the SBS textured-blit shaders, the v210 decode pass and the 3D grid.
//
// GENERATED — do not edit by hand. Regenerate with scripts/gen_shaders.sh after
// changing shaders/fullscreen.vert, shaders/sbs.frag, shaders/v210_* or shaders/stereo_grid.*.
#pragma once

#include <cstdint>

HDR
  grep -v -E '^\s*//|#pragma once' /tmp/_v.h
  echo
  grep -v -E '^\s*//|#pragma once' /tmp/_f.h
  echo
  grep -v -E '^\s*//|#pragma once' /tmp/_c.h
  echo
  grep -v -E '^\s*//|#pragma once' /tmp/_gv.h
  echo
  grep -v -E '^\s*//|#pragma once' /tmp/_gf.h
} > src/rhi/Shaders.h

echo "Wrote src/rhi/Shaders.h"
