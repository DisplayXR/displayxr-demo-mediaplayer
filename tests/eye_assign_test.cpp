// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for EyeAssign (#80): tracked eyes split by X; collapsed (untracked) eyes all
// get the left half, so a display that keeps weaving for one eye point shows clean 2D.

#include "media/EyeAssign.h"

#include <cstdio>

using mp::eyes::IsLeftView;

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

int CountLeft(const float* xs, uint32_t n)
{
    int left = 0;
    for (uint32_t v = 0; v < n; ++v) left += IsLeftView(xs, n, v) ? 1 : 0;
    return left;
}

void TrackedPair()
{
    const float xs[2] = {-0.032f, 0.032f};
    CHECK(IsLeftView(xs, 2, 0), "tracked: view 0 (x<0) must be left");
    CHECK(!IsLeftView(xs, 2, 1), "tracked: view 1 (x>0) must be right");
    // Pose wins over order: a runtime that enumerated right first still gets it right.
    const float rev[2] = {0.032f, -0.032f};
    CHECK(!IsLeftView(rev, 2, 0), "tracked reversed: view 0 (x>0) must be right");
    CHECK(IsLeftView(rev, 2, 1), "tracked reversed: view 1 (x<0) must be left");
}

void UntrackedPair()
{
    // Both eyes at the nominal position: the vendor's "go flat" -> both views left.
    const float xs[2] = {0.0f, 0.0f};
    CHECK(IsLeftView(xs, 2, 0), "untracked: view 0 must be left");
    CHECK(IsLeftView(xs, 2, 1), "untracked: view 1 must ALSO be left (same half = clean 2D)");
    const float off[2] = {0.013f, 0.013f};  // nominal position off the panel centre
    CHECK(CountLeft(off, 2) == 2, "untracked off-centre: both views left");
}

void QuadViews()
{
    const float tracked[4] = {-0.05f, -0.02f, 0.02f, 0.05f};
    CHECK(CountLeft(tracked, 4) == 2, "tracked quad: two left views");
    CHECK(IsLeftView(tracked, 4, 1) && !IsLeftView(tracked, 4, 2), "tracked quad: split at the centre");
    const float collapsed[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(CountLeft(collapsed, 4) == 4, "collapsed quad: every view left");
}

void SingleView()
{
    // 2D mode: the one view stays on the left half, as before the fix.
    const float xs[1] = {0.0f};
    CHECK(IsLeftView(xs, 1, 0), "single view must sample the left half");
}

void TinyJitterCountsAsCollapsed()
{
    const float xs[2] = {0.0f, 0.000001f};
    CHECK(CountLeft(xs, 2) == 2, "sub-micron spread counts as collapsed: both left");
}

}  // namespace

int main()
{
    TrackedPair();
    UntrackedPair();
    QuadViews();
    SingleView();
    TinyJitterCountsAsCollapsed();
    if (g_failures != 0) {
        std::fprintf(stderr, "eye_assign_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("eye_assign_test: all passed\n");
    return 0;
}
