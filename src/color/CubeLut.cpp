// SPDX-License-Identifier: Apache-2.0
#include "color/CubeLut.h"

#include <cstdio>
#include <cstdlib>
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

}  // namespace mp::color
