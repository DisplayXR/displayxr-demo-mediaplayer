// SPDX-License-Identifier: Apache-2.0
//
// ThreadPin - put a thread on one group of logical processors that share a last-level cache
// (an L3: one CCX / CCD on an AMD Threadripper or EPYC, the whole package on most Intel
// desktops). An A/B lever for Media Player Pro's capture path (docs/CAPTURE-LEVERS.md): the
// DeckLink input lanes, their convert threads and the upload copy fight over memory, and on
// a chiplet CPU where a thread runs decides which cache and which fabric link it uses.
//
// Off unless asked for: nothing is pinned by default, and every call is a no-op off Windows.
// Header-only, so the trunk pools (rhi/BandCopy, pro/ConvertPool) and their tests need no
// extra source file; include it from .cpp files only (it pulls in windows.h).
#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// (not WIN32_LEAN_AND_MEAN: a file that includes this before the DeckLink SDK's MIDL header
// needs the COM declarations the full windows.h brings)
#include <windows.h>
#endif

namespace mp::pin {

// One last-level-cache group: its processor group and mask (Windows' GROUP_AFFINITY).
struct CacheGroup {
    unsigned short group = 0;
    unsigned long long mask = 0;
    int processors = 0;  // logical processors in it
};

// The last-level-cache groups, in the order Windows lists them (empty off Windows, or when
// the query fails). Computed once.
inline const std::vector<CacheGroup>& CacheGroups() {
    static const std::vector<CacheGroup> groups = [] {
        std::vector<CacheGroup> out;
#if defined(_WIN32)
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
        if (len == 0) return out;
        std::vector<unsigned char> buf(len);
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
        if (!GetLogicalProcessorInformationEx(RelationCache, info, &len)) return out;
        // The last level present (3 on every CPU this runs on; 2 if a CPU has no L3).
        int top = 0;
        for (DWORD off = 0; off < len;) {
            auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
            if (e->Relationship == RelationCache && (e->Cache.Type == CacheUnified || e->Cache.Type == CacheData))
                top = e->Cache.Level > top ? e->Cache.Level : top;
            off += e->Size;
        }
        for (DWORD off = 0; off < len;) {
            auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
            if (e->Relationship == RelationCache && e->Cache.Level == top &&
                (e->Cache.Type == CacheUnified || e->Cache.Type == CacheData)) {
                CacheGroup g;
                g.group = e->Cache.GroupMask.Group;
                g.mask = (unsigned long long)e->Cache.GroupMask.Mask;
                for (unsigned long long m = g.mask; m; m &= m - 1) ++g.processors;
                out.push_back(g);
            }
            off += e->Size;
        }
#endif
        return out;
    }();
    return groups;
}

// Pins the calling thread to cache group `index` (modulo the count, so "2" on a CPU with two
// groups lands on group 0). False when not pinned: index < 0, no groups, or the call failed.
inline bool PinCurrentThread(int index) {
#if defined(_WIN32)
    const std::vector<CacheGroup>& g = CacheGroups();
    if (index < 0 || g.empty()) return false;
    const CacheGroup& c = g[(size_t)index % g.size()];
    GROUP_AFFINITY a{};
    a.Group = c.group;
    a.Mask = (KAFFINITY)c.mask;
    return SetThreadGroupAffinity(GetCurrentThread(), &a, nullptr) != 0;
#else
    (void)index;
    return false;
#endif
}

// A pin request from the environment: unset, "", "off" or a negative number = -1 (not
// pinned); a number = that group. `slot` picks one entry of a comma list ("0,1": slot 0 is 0,
// slot 1 is 1; a list shorter than slot + 1 repeats its last entry). "auto" = `autoGroup`.
inline int GroupFromEnv(const char* value, int slot, int autoGroup) {
    if (!value || !*value) return -1;
    std::string s(value);
    for (char& c : s) c = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    if (s == "off" || s == "no" || s == "false") return -1;
    if (s == "auto" || s == "on" || s == "yes" || s == "true") return autoGroup;
    std::vector<int> list;
    size_t p = 0;
    while (p <= s.size()) {
        const size_t q = s.find(',', p);
        const std::string tok = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
        char* end = nullptr;
        const long v = std::strtol(tok.c_str(), &end, 10);
        if (tok.empty() || !end || *end != '\0') return -1;  // not a number: not pinned
        list.push_back((int)v);
        if (q == std::string::npos) break;
        p = q + 1;
    }
    if (list.empty()) return -1;
    const int v = list[(size_t)(slot < (int)list.size() ? slot : (int)list.size() - 1)];
    return v < 0 ? -1 : v;
}

}  // namespace mp::pin
