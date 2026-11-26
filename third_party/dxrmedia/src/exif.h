// SPDX-License-Identifier: Apache-2.0
// Internal: EXIF TIFF block (the APP1 payload after "Exif\0\0"), FORMAT.md 1.2.
#pragma once

#include "dxrmedia/metadata.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dxrmedia {

std::vector<uint8_t> exif_build(const ExifMeta& m, const std::string& facing);
bool exif_parse(const uint8_t* tiff, size_t n, ExifMeta& out);

}  // namespace dxrmedia
