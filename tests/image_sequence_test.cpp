// SPDX-License-Identifier: Apache-2.0
//
// Image sequences (Media Player Pro, phase 4): pattern, range and folder key from real
// files in a temporary folder.

#include "media/ImageSequence.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

void Touch(const fs::path& p) { std::ofstream(p) << "x"; }

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "mp_imageseq_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    for (int n = 1001; n <= 1010; ++n) Touch(dir / ("shot_L.v002." + std::to_string(n) + ".exr"));
    Touch(dir / "shot_L.v002.999.exr");    // shorter padding: not this sequence
    Touch(dir / "other.1001.exr");         // another sequence
    Touch(dir / "shot_L.v002.1005.dpx");   // same prefix, other extension
    Touch(dir / "plate.exr");              // no frame number

    mp::imageseq::Info s;
    CHECK(mp::imageseq::Detect((dir / "shot_L.v002.1004.exr").string(), s), "a numbered EXR is a sequence");
    CHECK(s.first == 1001 && s.last == 1010 && s.count == 10 && s.digits == 4, "range + padding");
    CHECK(s.pattern.size() > 9 && s.pattern.substr(s.pattern.size() - 9) == ".%04d.exr", "printf pattern");
    CHECK(mp::imageseq::Key((dir / "shot_L.v002.1001.exr").string()) ==
              mp::imageseq::Key((dir / "shot_L.v002.1010.exr").string()),
          "one folder key per sequence");
    CHECK(mp::imageseq::Key((dir / "shot_L.v002.1001.exr").string()) !=
              mp::imageseq::Key((dir / "shot_L.v002.1005.dpx").string()),
          "extension separates sequences");
    CHECK(!mp::imageseq::Detect((dir / "plate.exr").string(), s), "a still without a number is not a sequence");
    CHECK(!mp::imageseq::Detect((dir / "clip.1001.mov").string(), s), "movies are not sequences");
    CHECK(mp::imageseq::IsSequenceExtension("A.TIFF") && mp::imageseq::IsSequenceExtension("a.dpx"), "extensions");
    fs::remove_all(dir, ec);
    if (g_failures) {
        std::fprintf(stderr, "image_sequence_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("image_sequence_test: all passed\n");
    return 0;
}
