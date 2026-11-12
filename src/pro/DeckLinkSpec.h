// SPDX-License-Identifier: Apache-2.0
//
// The DeckLink station input's spec, "<dev>[+<right>][,sdi|hdmi][,auto|sbs|dual|mono][,mode=Hp25]"
// (the part after "decklink:"), and the DeckLink setup dialog's fields for it. Pure (no SDK,
// no UI) so tests/decklink_spec_test.cpp can check that the dialog shows the input that is
// set up and that its Open writes the same spec back.
#pragma once

#include "pro/DeckLinkSource.h"  // DeckLinkConfig (the header needs no SDK)

#include <string>
#include <vector>

namespace mp::pro {

// The spec -> config. False on an unknown token.
bool ParseDeckLinkSpec(const std::string& args, DeckLinkConfig& c);

// The setup dialog's fields. `devices` is its list, DeckLinkSource::Enumerate()'s
// "0: DeckLink 8K Pro (1)" entries, in the order an index selector picks from.
struct DeckLinkDialogFields {
    int left = 0;       // device list index
    int right = 0;      // 0 = none (one cable), else device list index + 1
    int conn = 0;       // 0 as set in Desktop Video, 1 SDI, 2 HDMI
    int stereo = 0;     // one cable: 0 auto, 1 side by side, 2 dual-stream 3D, 3 mono
    std::string mode;   // starting format FourCC; empty = the default
};

// The device a selector names, as DeckLinkSource opens it: an index ("1"), else the first
// device whose name contains it (case-insensitive). -1 if none.
int DeckLinkDialogDevice(const std::string& selector, const std::vector<std::string>& devices);

// The fields that show `c`. False (out untouched) when a device it names is not in the list.
bool DeckLinkDialogFromConfig(const DeckLinkConfig& c, const std::vector<std::string>& devices,
                              DeckLinkDialogFields& out);

// The spec the dialog's Open sends.
std::string DeckLinkSpecFromDialog(const DeckLinkDialogFields& f);

}  // namespace mp::pro
