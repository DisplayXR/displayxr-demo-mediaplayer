// SPDX-License-Identifier: Apache-2.0
// Unit tests for the Pro color loaders: .cube 3D LUTs and ASC CDL (.cc/.ccc/.cdl).
#include "color/Cdl.h"
#include "color/CubeLut.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void TestCubeIdentity2() {
    const std::string text =
        "# identity\r\nTITLE \"Ident 2\"\r\nLUT_3D_SIZE 2\r\n\r\n"
        "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    mp::color::CubeLut lut;
    std::string err;
    CHECK(mp::color::ParseCube(text, lut, err));
    CHECK(lut.Valid());
    CHECK(lut.size == 2);
    CHECK(lut.title == "Ident 2");
    // Red varies fastest: row 1 is (1,0,0).
    CHECK(Near(lut.rgb[3], 1.f) && Near(lut.rgb[4], 0.f) && Near(lut.rgb[5], 0.f));
}

static void TestCubeDomainAndErrors() {
    mp::color::CubeLut lut;
    std::string err;
    std::string t = "LUT_3D_SIZE 2\nDOMAIN_MIN -0.1 -0.1 -0.1\nDOMAIN_MAX 1.2 1.2 1.2\n";
    for (int i = 0; i < 8; ++i) t += "0.5 0.5 0.5\n";
    CHECK(mp::color::ParseCube(t, lut, err));
    CHECK(Near(lut.domainMin[0], -0.1f) && Near(lut.domainMax[2], 1.2f));

    CHECK(!mp::color::ParseCube("LUT_3D_SIZE 2\n0 0 0\n", lut, err));          // too few rows
    CHECK(err.find("needs 8") != std::string::npos);
    CHECK(!mp::color::ParseCube("LUT_1D_SIZE 1024\n", lut, err));               // 1D rejected
    CHECK(!mp::color::ParseCube("0 0 0\n", lut, err));                          // data first
    CHECK(!mp::color::ParseCube("LUT_3D_SIZE 1\n", lut, err));                  // bad size
}

static void TestCdlCc() {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ColorCorrection id=\"A001C003\">\n"
        "  <SOPNode>\n"
        "    <Description>day 12</Description>\n"
        "    <Slope>1.1 1.0 0.9</Slope>\n"
        "    <Offset>0.01 0 -0.02</Offset>\n"
        "    <Power>1.0 1.05 1.0</Power>\n"
        "  </SOPNode>\n"
        "  <SatNode><Saturation>0.8</Saturation></SatNode>\n"
        "</ColorCorrection>\n";
    mp::color::Cdl cdl;
    std::string err;
    CHECK(mp::color::ParseCdl(xml, cdl, err));
    CHECK(cdl.id == "A001C003");
    CHECK(Near(cdl.slope[0], 1.1f) && Near(cdl.slope[2], 0.9f));
    CHECK(Near(cdl.offset[2], -0.02f));
    CHECK(Near(cdl.power[1], 1.05f));
    CHECK(Near(cdl.saturation, 0.8f));
    CHECK(!cdl.IsIdentity());
}

static void TestCdlCollectionPick() {
    const std::string xml =
        "<ColorCorrectionCollection xmlns=\"urn:ASC:CDL:v1.01\">"
        "<ColorCorrection id=\"one\"><SOPNode><Slope>2 2 2</Slope></SOPNode></ColorCorrection>"
        "<ColorCorrection id=\"two\"><SOPNode><Slope>3 3 3</Slope></SOPNode></ColorCorrection>"
        "</ColorCorrectionCollection>";
    mp::color::Cdl cdl;
    std::string err;
    CHECK(mp::color::ParseCdl(xml, cdl, err));
    CHECK(cdl.id == "one" && Near(cdl.slope[0], 2.f));
    CHECK(mp::color::ParseCdl(xml, cdl, err, "two"));
    CHECK(cdl.id == "two" && Near(cdl.slope[0], 3.f));
    CHECK(!mp::color::ParseCdl("<ColorCorrection id=\"x\"></ColorCorrection>", cdl, err));
}

int main() {
    TestCubeIdentity2();
    TestCubeDomainAndErrors();
    TestCdlCc();
    TestCdlCollectionPick();
    if (g_fail) {
        std::fprintf(stderr, "color_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("color_test: all passed\n");
    return 0;
}
