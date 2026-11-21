// SPDX-License-Identifier: Apache-2.0
//
// RigBudgetSettings (#265): the rig check's budget kept in settings.txt as rig.budget.*, the
// names set_depth_budget takes. Header-only so the settings test covers it without the
// station.
#pragma once

#include "media/Settings.h"
#include "pro/RigCheck.h"

namespace mp::pro {

struct RigBudgetKey {
    const char* key;
    float rig::Budget::*field;
    float lo, hi;
};

inline constexpr int kRigBudgetKeys = 5;
inline const RigBudgetKey* RigBudgetKeys() {
    static const RigBudgetKey keys[kRigBudgetKeys] = {
        {"rig.budget.near_pct", &rig::Budget::nearPct, -20.0f, 20.0f},
        {"rig.budget.far_pct", &rig::Budget::farPct, -20.0f, 20.0f},
        {"rig.budget.vertical_px", &rig::Budget::maxVerticalPx, 0.0f, 100.0f},
        {"rig.budget.delta_e", &rig::Budget::maxDeltaE, 0.0f, 100.0f},
        {"rig.budget.edge_pct", &rig::Budget::edgePct, -20.0f, 20.0f},
    };
    return keys;
}

// The kept budget over `b`, except the fields whose bit is set in `keepMask` (the command
// line set them for this run). An inverted near/far pair leaves `b` as it was; false then.
inline bool LoadRigBudget(const Settings& store, rig::Budget& b, unsigned keepMask) {
    rig::Budget out = b;
    for (int i = 0; i < kRigBudgetKeys; ++i) {
        const RigBudgetKey& k = RigBudgetKeys()[i];
        if (!(keepMask & (1u << i))) out.*k.field = store.GetFloat(k.key, b.*k.field, k.lo, k.hi);
    }
    if (!(out.nearPct < out.farPct)) return false;
    b = out;
    return true;
}

// Writes the fields of `b` that differ from `seen` (and updates `seen`): only what was
// changed after the start is kept, so a command-line value is never saved by itself.
inline void SaveRigBudgetChanges(Settings& store, const rig::Budget& b, float seen[kRigBudgetKeys]) {
    for (int i = 0; i < kRigBudgetKeys; ++i) {
        const RigBudgetKey& k = RigBudgetKeys()[i];
        const float v = b.*k.field;
        if (v == seen[i]) continue;
        seen[i] = v;
        store.SetFloat(k.key, v);
    }
}

inline void SeenRigBudget(const rig::Budget& b, float seen[kRigBudgetKeys]) {
    for (int i = 0; i < kRigBudgetKeys; ++i) seen[i] = b.*RigBudgetKeys()[i].field;
}

}  // namespace mp::pro
