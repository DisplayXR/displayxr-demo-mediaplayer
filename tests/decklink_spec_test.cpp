// SPDX-License-Identifier: Apache-2.0
// The DeckLink input spec and the setup dialog's fields (pro/DeckLinkSpec.h): the dialog
// opens on the input that is set up, e.g. a pair started from the command line
// (--decklink=0 --decklink-right=1 -> "0+1") shows both eyes, and its Open writes the same
// spec back.
#include "pro/DeckLinkSpec.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                           \
        }                                                                       \
    } while (0)

using namespace mp;
using namespace mp::pro;

// DeckLinkSource::Enumerate()'s list on an 8K Pro G2 in the 4-sub-device profile.
static const std::vector<std::string> kDevices = {"0: DeckLink 8K Pro (1)", "1: DeckLink 8K Pro (2)",
                                                  "2: DeckLink 8K Pro (3)", "3: DeckLink 8K Pro (4)"};

static bool Show(const std::string& spec, DeckLinkDialogFields& f) {
    DeckLinkConfig c;
    return ParseDeckLinkSpec(spec, c) && DeckLinkDialogFromConfig(c, kDevices, f);
}

int main() {
    DeckLinkDialogFields f;

    // The station's case: a pair from the command line shows both eyes, not "right: none".
    CHECK(Show("0+1", f) && f.left == 0 && f.right == 2 && f.conn == 0 && f.stereo == 0 && f.mode.empty());
    CHECK(DeckLinkSpecFromDialog(f) == "0+1");
    CHECK(Show("0+2,sdi,mode=Hp60", f) && f.left == 0 && f.right == 3 && f.conn == 1 && f.mode == "Hp60");
    CHECK(DeckLinkSpecFromDialog(f) == "0+2,sdi,mode=Hp60");

    // One cable: connection and packing.
    CHECK(Show("1,hdmi,dual", f) && f.left == 1 && f.right == 0 && f.conn == 2 && f.stereo == 2);
    CHECK(DeckLinkSpecFromDialog(f) == "1,hdmi,dual");
    CHECK(Show("3,sbs", f) && f.left == 3 && f.stereo == 1);
    CHECK(Show("2,mono", f) && f.left == 2 && f.stereo == 3);
    CHECK(Show("0,auto", f) && f.stereo == 0 && DeckLinkSpecFromDialog(f) == "0");  // auto is the default
    CHECK(Show("", f) && f.left == 0 && f.right == 0);                              // device defaults to 0

    // Name selectors, as DeckLinkSource picks them: the first name containing it, any case,
    // never the list's "N: " prefix.
    CHECK(Show("8K Pro (2)+8k pro (4)", f) && f.left == 1 && f.right == 4);
    CHECK(DeckLinkDialogDevice("DeckLink", kDevices) == 0);
    CHECK(DeckLinkDialogDevice("1: Deck", kDevices) == -1);
    CHECK(DeckLinkDialogDevice("", kDevices) == -1);

    // A spec the dialog cannot show leaves the fields as they were.
    DeckLinkDialogFields keep;
    keep.left = 2;
    keep.right = 4;
    f = keep;
    CHECK(!Show("5+1", f) && f.left == 2 && f.right == 4);   // left not on this card
    CHECK(!Show("0+9", f) && f.left == 2 && f.right == 4);   // right not on this card
    CHECK(!Show("Quad 2", f) && f.left == 2);                // no such name
    DeckLinkConfig c;
    CHECK(!ParseDeckLinkSpec("0,xyz", c));                   // unknown token

    // Every dialog choice survives Open -> spec -> dialog unchanged.
    int combos = 0;
    for (int left = 0; left < 4; ++left)
        for (int right = 0; right <= 4; ++right) {
            if (right > 0 && right - 1 == left) continue;  // the dialog refuses the same input twice
            for (int conn = 0; conn < 3; ++conn)
                for (int stereo = 0; stereo < 4; ++stereo)
                    for (const char* mode : {"", "Hp25", "4k60"}) {
                        if (right > 0 && stereo != 0) continue;  // packing is one-cable only
                        DeckLinkDialogFields in;
                        in.left = left;
                        in.right = right;
                        in.conn = conn;
                        in.stereo = stereo;
                        in.mode = mode;
                        DeckLinkDialogFields out;
                        const std::string spec = DeckLinkSpecFromDialog(in);
                        const bool ok = Show(spec, out) && out.left == in.left && out.right == in.right &&
                                        out.conn == in.conn && out.stereo == in.stereo && out.mode == in.mode;
                        if (!ok) std::fprintf(stderr, "round trip failed for spec '%s'\n", spec.c_str()), ++g_fail;
                        ++combos;
                    }
        }

    std::printf("decklink spec: %d dialog round trips\n", combos);
    if (g_fail) std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return g_fail ? 1 : 0;
}
