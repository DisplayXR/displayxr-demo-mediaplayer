// SPDX-License-Identifier: Apache-2.0
#include "color/Cdl.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mp::color {

namespace {

// Find "<Tag" or "<ns:Tag" opening (followed by '>' or whitespace) at or after `from`.
size_t FindOpen(const std::string& s, const std::string& tag, size_t from) {
    size_t p = from;
    while ((p = s.find(tag, p)) != std::string::npos) {
        const size_t lt = s.rfind('<', p);
        const bool opens = lt != std::string::npos && s.find('>', lt) >= p &&
                           s[lt + 1] != '/' && (lt + 1 == p || s[p - 1] == ':');
        const char after = p + tag.size() < s.size() ? s[p + tag.size()] : '\0';
        if (opens && (after == '>' || after == ' ' || after == '\t' || after == '\r' || after == '\n' ||
                      after == '/'))
            return lt;
        p += tag.size();
    }
    return std::string::npos;
}

// Text content of the first <tag>...</tag> in [from, to).
bool ElementText(const std::string& s, const std::string& tag, size_t from, size_t to, std::string& out) {
    const size_t lt = FindOpen(s, tag, from);
    if (lt == std::string::npos || lt >= to) return false;
    const size_t gt = s.find('>', lt);
    if (gt == std::string::npos) return false;
    const size_t end = s.find('<', gt);
    if (end == std::string::npos || end > to) return false;
    out = s.substr(gt + 1, end - gt - 1);
    return true;
}

bool Read3(const std::string& text, float v[3]) {
    float a, b, c;
    if (std::sscanf(text.c_str(), "%f %f %f", &a, &b, &c) != 3) return false;
    v[0] = a; v[1] = b; v[2] = c;
    return true;
}

std::string AttrId(const std::string& s, size_t lt) {
    const size_t gt = s.find('>', lt);
    const size_t p = s.find("id=", lt);
    if (p == std::string::npos || p > gt) return {};
    const char q = s[p + 3];
    if (q != '"' && q != '\'') return {};
    const size_t e = s.find(q, p + 4);
    return e == std::string::npos ? std::string() : s.substr(p + 4, e - p - 4);
}

}  // namespace

bool ParseCdl(const std::string& xml, Cdl& out, std::string& error, const std::string& wantId) {
    out = Cdl{};
    // Pick the correction: the one with the wanted id, else the first.
    size_t cc = FindOpen(xml, "ColorCorrection", 0);
    if (cc == std::string::npos) {
        error = "no <ColorCorrection> element";
        return false;
    }
    if (!wantId.empty()) {
        for (size_t p = cc; p != std::string::npos;
             p = FindOpen(xml, "ColorCorrection", p + 1 + std::strlen("ColorCorrection"))) {
            if (AttrId(xml, p) == wantId) { cc = p; break; }
        }
    }
    out.id = AttrId(xml, cc);
    size_t ccEnd = xml.find("ColorCorrection>", cc + 1);
    while (ccEnd != std::string::npos && xml[ccEnd - 1] != '/' && xml[ccEnd - 1] != ':')
        ccEnd = xml.find("ColorCorrection>", ccEnd + 1);
    if (ccEnd == std::string::npos) ccEnd = xml.size();

    std::string t;
    bool any = false;
    if (ElementText(xml, "Slope", cc, ccEnd, t)) {
        if (!Read3(t, out.slope)) { error = "<Slope> needs 3 numbers"; return false; }
        any = true;
    }
    if (ElementText(xml, "Offset", cc, ccEnd, t)) {
        if (!Read3(t, out.offset)) { error = "<Offset> needs 3 numbers"; return false; }
        any = true;
    }
    if (ElementText(xml, "Power", cc, ccEnd, t)) {
        if (!Read3(t, out.power)) { error = "<Power> needs 3 numbers"; return false; }
        any = true;
    }
    if (ElementText(xml, "Saturation", cc, ccEnd, t)) {
        if (std::sscanf(t.c_str(), "%f", &out.saturation) != 1) { error = "<Saturation> needs a number"; return false; }
        any = true;
    }
    if (!any) {
        error = "<ColorCorrection> has no Slope/Offset/Power/Saturation";
        return false;
    }
    for (float p : out.power)
        if (p <= 0.f) { error = "<Power> values must be > 0"; return false; }
    return true;
}

bool LoadCdl(const std::string& path, Cdl& out, std::string& error, const std::string& wantId) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ParseCdl(ss.str(), out, error, wantId);
}

}  // namespace mp::color
