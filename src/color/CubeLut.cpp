// SPDX-License-Identifier: Apache-2.0
#include "color/CubeLut.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mp::color {

namespace {

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// IEEE 754 binary32 -> binary16 (round half up on the dropped bits), for the RGBA16F LUT.
uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t e32 = (x >> 23) & 0xffu;
    int32_t exp = (int32_t)e32 - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (e32 == 0xffu) return (uint16_t)(sign | 0x7c00u | (mant ? 0x200u : 0));
    if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exp);
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) h += 1;
        return (uint16_t)(sign | h);
    }
    uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
    if (mant & 0x1000u) h += 1;  // a carry into the exponent is the correct rounding
    return (uint16_t)h;
}

bool Fail(std::string& error, int line, const char* what) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "line %d: %s", line, what);
    error = buf;
    return false;
}

}  // namespace

bool ParseCube(const std::string& text, CubeLut& out, std::string& error) {
    out = CubeLut{};
    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    size_t expected = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        std::string line = Trim(raw);
        if (line.empty() || line[0] == '#') continue;

        // Data rows start with a digit, a sign or a dot; keywords with a letter.
        const char c0 = line[0];
        const bool isData = (c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '+' || c0 == '.';
        if (!isData) {
            std::istringstream ks(line);
            std::string key;
            ks >> key;
            if (key == "TITLE") {
                const size_t q0 = line.find('"');
                const size_t q1 = line.rfind('"');
                out.title = (q0 != std::string::npos && q1 > q0) ? line.substr(q0 + 1, q1 - q0 - 1)
                                                                  : Trim(line.substr(5));
            } else if (key == "LUT_3D_SIZE") {
                int n = 0;
                if (!(ks >> n) || n < 2 || n > 256) return Fail(error, lineNo, "LUT_3D_SIZE must be 2..256");
                out.size = n;
                expected = (size_t)n * n * n;
                out.rgb.reserve(expected * 3);
            } else if (key == "LUT_1D_SIZE") {
                return Fail(error, lineNo, "1D LUTs are not supported; export a 3D .cube");
            } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
                float* d = key == "DOMAIN_MIN" ? out.domainMin : out.domainMax;
                if (!(ks >> d[0] >> d[1] >> d[2])) return Fail(error, lineNo, "DOMAIN needs 3 numbers");
            } else if (key == "LUT_3D_INPUT_RANGE") {
                float lo = 0.f, hi = 1.f;
                if (!(ks >> lo >> hi)) return Fail(error, lineNo, "LUT_3D_INPUT_RANGE needs 2 numbers");
                for (int i = 0; i < 3; ++i) { out.domainMin[i] = lo; out.domainMax[i] = hi; }
            }
            // Unknown keywords (e.g. vendor extensions) are ignored, as Resolve does.
            continue;
        }

        if (out.size == 0) return Fail(error, lineNo, "data before LUT_3D_SIZE");
        float r = 0.f, g = 0.f, b = 0.f;
        if (std::sscanf(line.c_str(), "%f %f %f", &r, &g, &b) != 3)
            return Fail(error, lineNo, "expected three numbers");
        if (out.rgb.size() >= expected * 3) return Fail(error, lineNo, "more rows than LUT_3D_SIZE^3");
        out.rgb.push_back(r);
        out.rgb.push_back(g);
        out.rgb.push_back(b);
    }
    if (out.size == 0) return Fail(error, lineNo, "no LUT_3D_SIZE found");
    for (int i = 0; i < 3; ++i)
        if (!(out.domainMax[i] > out.domainMin[i])) return Fail(error, lineNo, "DOMAIN_MAX must exceed DOMAIN_MIN");
    if (out.rgb.size() != expected * 3) {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "found %zu rows, LUT_3D_SIZE %d needs %zu", out.rgb.size() / 3,
                      out.size, expected);
        error = buf;
        return false;
    }
    return true;
}

bool LoadCube(const std::string& path, CubeLut& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ParseCube(ss.str(), out, error);
}

std::vector<uint16_t> PackRgba16f(const float* rgb, size_t count) {
    std::vector<uint16_t> px(count * 4);
    for (size_t i = 0; i < count; ++i) {
        px[i * 4 + 0] = FloatToHalf(rgb[i * 3 + 0]);
        px[i * 4 + 1] = FloatToHalf(rgb[i * 3 + 1]);
        px[i * 4 + 2] = FloatToHalf(rgb[i * 3 + 2]);
        px[i * 4 + 3] = 0x3c00u;  // 1.0
    }
    return px;
}

}  // namespace mp::color
