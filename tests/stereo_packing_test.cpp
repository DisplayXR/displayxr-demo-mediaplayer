// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for StereoPacking (#128): eye rects, per-eye aspect, the top-bottom
// full/half choice, and the filename hints.

#include "media/MediaSource.h"
#include "media/StereoPacking.h"

#include <cmath>
#include <cstdio>

using namespace mp;
using namespace mp::packing;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAIL (%s:%d): ", __FILE__, __LINE__);  \
            std::fprintf(stderr, __VA_ARGS__);                             \
            std::fprintf(stderr, "\n");                                    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

bool Same(EyeRect r, float x, float y, float w, float h) {
    return Near(r.x, x) && Near(r.y, y) && Near(r.w, w) && Near(r.h, h);
}

void EyeRects() {
    CHECK(Same(Eye(StereoLayout::SbsFull, true), 0, 0, 0.5f, 1), "SBS left = left half");
    CHECK(Same(Eye(StereoLayout::SbsHalf, false), 0.5f, 0, 0.5f, 1), "SBS right = right half");
    CHECK(Same(Eye(StereoLayout::TbFull, true), 0, 0, 1, 0.5f), "TB left = top half");
    CHECK(Same(Eye(StereoLayout::TbHalf, false), 0, 0.5f, 1, 0.5f), "TB right = bottom half");
    CHECK(Same(Eye(StereoLayout::Mono, false), 0, 0, 1, 1), "mono = whole frame");
    CHECK(Same(Eye(StereoLayout::Dual, true), 0, 0, 1, 1), "dual = whole frame");
}

void Aspects() {
    // 16:9 eyes packed every way come back as 16:9.
    CHECK(Near(PerEyeAspect(StereoLayout::SbsFull, 3840, 1080), 16.f / 9.f), "SBS-full 3840x1080");
    CHECK(Near(PerEyeAspect(StereoLayout::SbsHalf, 1920, 1080), 16.f / 9.f), "SBS-half 1920x1080");
    CHECK(Near(PerEyeAspect(StereoLayout::TbFull, 1920, 2160), 16.f / 9.f), "TB-full 1920x2160");
    CHECK(Near(PerEyeAspect(StereoLayout::TbHalf, 1920, 1080), 16.f / 9.f), "TB-half 1920x1080");
    CHECK(Near(PerEyeAspect(StereoLayout::Mono, 1920, 1080), 16.f / 9.f), "mono 1920x1080");
    CHECK(Near(PerEyeAspect(StereoLayout::TbFull, 0, 0), 1.0f), "degenerate frame -> 1");
}

void TbFullOrHalf() {
    CHECK(ChooseTbFullOrHalf(1920.f / 2160.f) == StereoLayout::TbFull, "1920x2160 is TB-full");
    CHECK(ChooseTbFullOrHalf(1920.f / 1080.f) == StereoLayout::TbHalf, "1920x1080 is TB-half");
    CHECK(ChooseTbFullOrHalf(1.0f) == StereoLayout::TbFull, "square (4:3 eyes stacked ~0.67..1) is TB-full");
    CHECK(ChooseTbFullOrHalf(0.0f) == StereoLayout::TbHalf, "no aspect -> TB-half");
}

void Filenames() {
    StereoLayout l = StereoLayout::Mono;
    CHECK(MediaSource::LayoutFromFilename("clip_1x2.mp4", l) && l == StereoLayout::TbFull, "_1x2 -> TB-full");
    CHECK(MediaSource::LayoutFromFilename("clip_half_1x2.mp4", l) && l == StereoLayout::TbHalf,
          "_half_1x2 -> TB-half");
    CHECK(MediaSource::LayoutFromFilename("Movie.H-OU.mkv", l) && l == StereoLayout::TbHalf, "H-OU -> TB-half");
    CHECK(MediaSource::LayoutFromFilename("movie_tb.mp4", l) && l == StereoLayout::TbHalf, "_tb -> TB-half");
    CHECK(MediaSource::LayoutFromFilename("movie-ou-1080p.mp4", l) && l == StereoLayout::TbHalf, "-ou- -> TB-half");
    CHECK(MediaSource::LayoutFromFilename("movie_full_tb.mp4", l) && l == StereoLayout::TbFull,
          "full_tb -> TB-full");
    CHECK(MediaSource::LayoutFromFilename("movie_tab.mp4", l) && l == StereoLayout::TbHalf, "_tab -> TB-half");
    // Words that merely contain the letters must not match.
    CHECK(!MediaSource::LayoutFromFilename("tabletop_tour.mp4", l), "tabletop_tour is not TB");
    CHECK(!MediaSource::LayoutFromFilename("output.mp4", l), "output is not TB");
    CHECK(!MediaSource::LayoutFromFilename("C:/tb/video.mp4", l), "a directory named tb is not a hint");
    // The SBS names keep working, and win over a stray TB token.
    CHECK(MediaSource::LayoutFromFilename("x_half_2x1.mp4", l) && l == StereoLayout::SbsHalf, "half_2x1 -> SBS-half");
    CHECK(MediaSource::LayoutFromFilename("x_2x1.png", l) && l == StereoLayout::SbsFull, "2x1 -> SBS-full");
    CHECK(MediaSource::LayoutFromFilename("x_tb_2x1.png", l) && l == StereoLayout::SbsFull, "2x1 beats tb");
}

void Names() {
    CHECK(std::string(MediaSource::LayoutName(StereoLayout::TbFull)) == "TB-full", "TB-full name");
    CHECK(std::string(MediaSource::LayoutName(StereoLayout::TbHalf)) == "TB-half", "TB-half name");
}

}  // namespace

int main() {
    EyeRects();
    Aspects();
    TbFullOrHalf();
    Filenames();
    Names();
    if (g_failures != 0) {
        std::fprintf(stderr, "stereo_packing_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("stereo_packing_test: all passed\n");
    return 0;
}
