// SPDX-License-Identifier: Apache-2.0
//
// CaptureLevers - every switch that changes Media Player Pro's live capture path, in one
// table, so a run can say exactly which combination it measured (docs/CAPTURE-LEVERS.md).
//
// Why: two 2160p59.94 DeckLink inputs publish ~50-54 pairs/s on the 8K station, below the
// signal (#238). The fixes are levers, most of them new and none of them proven on the card
// yet, and the station is not always reachable. So each lever is an environment switch
// (default = today's behaviour), the capture logs one `capture-config:` line and the
// station's status JSON carries a `capture` block naming every lever's requested and
// effective value, and scripts/capture_matrix.ps1 runs the combinations and tabulates them.
//
// The levers also live in a file (pro.env, ReadEnvFile) so a station keeps its settings
// across a reinstall: the installer rewrites the Start-menu shortcut, the file stays. A
// variable already set in the process environment wins over the file.
//
// Pure (no SDK, no Vulkan): tests/capture_levers_test.cpp.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace mp::pro {

// One lever: its variable, what it does, its default when unset, and what the user set.
struct Lever {
    const char* name;
    const char* what;
    const char* def;    // the value used when unset (text, as a user would type it)
    std::string value;  // as set ("" = unset)
    bool Set() const { return !value.empty(); }
    std::string Effective() const { return Set() ? value : std::string(def); }
};

// The table, in the order the report prints it. Grouped: capture (card side), pairing,
// upload, render-thread extras, runtime.
inline std::vector<Lever> CaptureLeverTable() {
    return {
        // --- capture: where each eye's pixels go between the card and the ring
        {"MEDIAPLAYER_GPU_V210", "decode v210 on the GPU (#240): needed for RAW_V210", "0", {}},
        {"MEDIAPLAYER_DECKLINK_RAW_V210",
         "DeckLink lanes publish the card's v210 rows as they are (no CPU convert, no compose; #238 phase 2)", "0", {}},
        {"MEDIAPLAYER_DECKLINK_ALLOCATOR",
         "the card writes into page-aligned buffers this app owns (IDeckLinkVideoBufferAllocatorProvider)", "0", {}},
        {"MEDIAPLAYER_DECKLINK_ALLOC_NODE", "NUMA node of those buffers (-1 = the OS's choice)", "-1", {}},
        {"MEDIAPLAYER_DECKLINK_HOLD",
         "the card callback keeps the frame's buffer instead of copying it out (needs ALLOCATOR)", "0", {}},
        {"MEDIAPLAYER_DECKLINK_CONVERT_THREADS", "convert threads per input (default: half the CPUs / eyes, 1-8)",
         "auto", {}},
        {"MEDIAPLAYER_CAPTURE_PIN", "pin input lane N + its convert threads to L3 group N (\"auto\" = 0,1)", "off",
         {}},
        // --- pairing
        {"MEDIAPLAYER_DECKLINK_DIRECT_PAIR", "each eye converted straight into its half of the ring frame (0.9.8)",
         "1", {}},
        {"MEDIAPLAYER_DECKLINK_STRICT_PAIRS", "matched pairs only, one card (0.9.7)", "1", {}},
        {"MEDIAPLAYER_LIVE_QUEUE", "live ring queue depth (0 = latest-wins)", "3", {}},
        // --- upload
        {"MEDIAPLAYER_ASYNC_UPLOAD", "staging copy on the upload thread (0.9.6)", "1", {}},
        {"MEDIAPLAYER_UPLOAD_THREADS", "upload copy threads (default: a quarter of the CPUs, 1-4)", "auto", {}},
        {"MEDIAPLAYER_UPLOAD_PIN", "pin the upload thread + its copy threads to an L3 group (\"auto\" = 2)", "off",
         {}},
        // --- render-thread extras (pro/PerfSwitches.h)
        {"MEDIAPLAYER_PRO_RIG", "rig check", "1", {}},
        {"MEDIAPLAYER_PRO_THUMBS", "live thumbnails", "1", {}},
        {"MEDIAPLAYER_PRO_AUTOCONV", "auto-convergence", "1", {}},
        {"MEDIAPLAYER_PRO_UI", "UI while live", "1", {}},
        // --- the runtime (read by the DisplayXR runtime in this process)
        {"DXR_VK_HUB_DEFER_COMPOSE_WAIT", "runtime: no CPU wait on the compose fence (v2.66.17+)", "0", {}},
    };
}

// The table with each variable's value through `get` (a getenv; nullptr = unset).
template <class Getenv>
std::vector<Lever> ReadCaptureLevers(Getenv&& get) {
    std::vector<Lever> t = CaptureLeverTable();
    for (Lever& l : t)
        if (const char* v = get(l.name)) l.value = v;
    return t;
}
inline std::vector<Lever> ReadCaptureLevers() {
    return ReadCaptureLevers([](const char* n) { return std::getenv(n); });
}

// "NAME=value" for the levers that are set, comma-separated ("" when none is): the short
// form the `capture-config:` line and the matrix script's arm label use.
inline std::string SetLeversText(const std::vector<Lever>& t) {
    std::string s;
    for (const Lever& l : t)
        if (l.Set()) s += (s.empty() ? "" : ", ") + std::string(l.name) + "=" + l.value;
    return s;
}

// ---- pro.env: KEY=VALUE lines. '#' starts a comment line; blank lines are skipped; spaces
// around the key and value are trimmed; a value may be quoted ("..." or '...'). Lines that
// are not KEY=VALUE are returned in `bad` (1-based line numbers) and otherwise ignored.
struct EnvEntry {
    std::string key, value;
    int line = 0;
};
inline std::string TrimSpaces(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
inline std::vector<EnvEntry> ParseEnvText(const std::string& text, std::vector<int>* bad = nullptr) {
    std::vector<EnvEntry> out;
    int lineNo = 0;
    size_t p = 0;
    while (p <= text.size()) {
        size_t q = text.find('\n', p);
        if (q == std::string::npos) q = text.size();
        std::string line = text.substr(p, q - p);
        ++lineNo;
        p = q + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lineNo == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line = line.substr(3);  // a UTF-8 BOM (Notepad)
        const std::string t = TrimSpaces(line);
        if (t.empty() || t[0] == '#') {
            if (q == text.size()) break;
            continue;
        }
        const size_t eq = t.find('=');
        std::string key = eq == std::string::npos ? std::string() : TrimSpaces(t.substr(0, eq));
        bool ok = !key.empty();
        for (char c : key) ok = ok && (std::isalnum((unsigned char)c) || c == '_');
        if (!ok) {
            if (bad) bad->push_back(lineNo);
        } else {
            std::string v = TrimSpaces(t.substr(eq + 1));
            if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
                v = v.substr(1, v.size() - 2);
            out.push_back({key, v, lineNo});
        }
        if (q == text.size()) break;
    }
    return out;
}

// What applying a file did, per entry: set from the file, or kept because the process
// environment already had the variable (the environment wins).
struct EnvApplied {
    std::string key, value;
    bool fromFile = false;  // false: the environment's value was kept
};
// Applies entries through `get` (getenv) / `set(key, value)`; returns what happened to each.
template <class Getenv, class Setenv>
std::vector<EnvApplied> ApplyEnvEntries(const std::vector<EnvEntry>& entries, Getenv&& get, Setenv&& set) {
    std::vector<EnvApplied> out;
    for (const EnvEntry& e : entries) {
        const char* cur = get(e.key.c_str());
        if (cur && *cur) {
            out.push_back({e.key, cur, false});
        } else {
            set(e.key, e.value);
            out.push_back({e.key, e.value, true});
        }
    }
    return out;
}

}  // namespace mp::pro
