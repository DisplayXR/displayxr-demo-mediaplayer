// SPDX-License-Identifier: Apache-2.0
#include "media/ImageSequence.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <filesystem>

namespace mp::imageseq {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Split "shot.1001.exr" into prefix "shot.", digits "1001", ext ".exr". False if the stem
// does not end in digits.
bool Split(const std::string& filename, std::string& prefix, std::string& digits, std::string& ext) {
    const size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    ext = filename.substr(dot);
    const std::string stem = filename.substr(0, dot);
    size_t i = stem.size();
    while (i > 0 && std::isdigit((unsigned char)stem[i - 1])) --i;
    if (i == stem.size() || stem.size() - i > 9) return false;
    prefix = stem.substr(0, i);
    digits = stem.substr(i);
    return true;
}

}  // namespace

bool IsSequenceExtension(const std::string& path) {
    const std::string e = Lower(std::filesystem::path(path).extension().string());
    return e == ".exr" || e == ".dpx" || e == ".tif" || e == ".tiff";
}

std::string Key(const std::string& path) {
    if (!IsSequenceExtension(path)) return {};
    std::string prefix, digits, ext;
    if (!Split(std::filesystem::path(path).filename().string(), prefix, digits, ext)) return {};
    return (std::filesystem::path(path).parent_path() / prefix).string() + "#" + Lower(ext);
}

bool Detect(const std::string& path, Info& out) {
    namespace fs = std::filesystem;
    if (!IsSequenceExtension(path)) return false;
    const fs::path p(path);
    std::string prefix, digits, ext;
    if (!Split(p.filename().string(), prefix, digits, ext)) return false;
    const int width = (int)digits.size();
    int first = INT_MAX, last = INT_MIN, count = 0;
    std::error_code ec;
    for (fs::directory_iterator it(p.parent_path().empty() ? fs::path(".") : p.parent_path(), ec), end;
         !ec && it != end; it.increment(ec)) {
        std::string pf, dg, ex;
        const std::string name = it->path().filename().string();
        if (!Split(name, pf, dg, ex) || pf != prefix || Lower(ex) != Lower(ext)) continue;
        // frame numbers may be unpadded past the width ("shot.10000.exr") but never shorter
        if ((int)dg.size() != width && !(dg.size() > (size_t)width && dg[0] != '0')) continue;
        const int n = std::atoi(dg.c_str());
        first = std::min(first, n);
        last = std::max(last, n);
        ++count;
    }
    if (count == 0) return false;
    char pat[16];
    std::snprintf(pat, sizeof(pat), "%%0%dd", width);
    out.pattern = (p.parent_path() / (prefix + pat + ext)).generic_string();
    out.first = first;
    out.last = last;
    out.count = count;
    out.digits = width;
    out.key = (p.parent_path() / prefix).string() + "#" + Lower(ext);
    return true;
}

}  // namespace mp::imageseq
