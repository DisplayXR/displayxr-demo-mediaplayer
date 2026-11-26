// SPDX-License-Identifier: Apache-2.0
//
// dxrmedia — XMP packet and Calibration JSON (FORMAT.md 1.3, 1.5).
#pragma once

#include "dxrmedia/metadata.h"

#include <string>

namespace dxrmedia {

// A complete XMP packet (<?xpacket ...?> ... <?xpacket end="w"?>), UTF-8, with one
// rdf:Description carrying the dxr: attributes of `m` that are present. dxr:Version
// is always written (kFormatVersion). The packet is always well-formed XML 1.0:
// characters XML forbids (C0 controls other than tab/LF/CR) are dropped and
// invalid UTF-8 becomes U+FFFD. A non-finite dxr: number is left out (FORMAT.md 0).
// Fails (false, `error` set) only when the structured Calibration holds a
// non-finite number: calibration is never silently changed.
bool xmp_serialize(const StereoMeta& m, std::string& packet, std::string* error = nullptr);

// Parse a packet. Tolerant: the dxr namespace is found by URI (any prefix), and both
// the attribute form (dxr:X="v") and the element form (<dxr:X>v</dxr:X>, what
// exiftool writes when it rewrites a packet) are read. Unknown attributes are
// ignored; a value that does not parse as its type is ignored (left absent).
// Returns true when the packet has a dxr:Version (i.e. it is ours, FORMAT.md 4).
bool xmp_parse(const std::string& packet, StereoMeta& out);

// Calibration JSON, compact, one line. False when any number is non-finite.
bool calibration_to_json(const Calibration& c, std::string& json, std::string* error = nullptr);
// Returns false (and leaves `out` unspecified) when the text is not valid JSON or
// not an object. Missing members keep their defaults; a member of the wrong type
// is ignored.
bool calibration_from_json(const std::string& json, Calibration& out);

// Locale-independent shortest round-trip decimal, never in exponent form, e.g.
// "0.0125". |v| < kZeroBelow is written "0" (FORMAT.md 0); a non-finite v gives ""
// (callers leave the value out). Used for every number in XMP and JSON.
constexpr double kZeroBelow = 1e-9;
std::string format_number(double v);
bool parse_number(const std::string& s, double& out);

}  // namespace dxrmedia
