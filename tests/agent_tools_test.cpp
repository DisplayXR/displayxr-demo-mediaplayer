// SPDX-License-Identifier: Apache-2.0
// Agent tools (#220): the player's and Pro's tables together must register cleanly with the
// runtime: unique names (a duplicate is refused, XR_ERROR_NAME_DUPLICATED), no more than
// the runtime's per-process budget (the rest are refused, XR_ERROR_LIMIT_REACHED), names
// that fit XR_MAX_MCP_TOOL_NAME_SIZE_DXR, and schemas that are JSON objects.
#include "AgentTools.h"
#include "pro/ProToolList.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <set>
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

struct Tool {
    std::string name, description, schema;
};

static std::vector<Tool> Collect(void (*fn)(const mp::AgentToolRegisterFn&)) {
    std::vector<Tool> out;
    fn([&](const char* n, const char* d, const char* s) { out.push_back({n, d ? d : "", s ? s : ""}); });
    return out;
}

static void CheckTable(const std::vector<Tool>& tools) {
    for (const Tool& t : tools) {
        CHECK(!t.name.empty() && t.name.size() < 64);  // XR_MAX_MCP_TOOL_NAME_SIZE_DXR incl. NUL
        CHECK(!t.description.empty());
        const nlohmann::json s = nlohmann::json::parse(t.schema, nullptr, false);
        if (s.is_discarded() || !s.is_object() || s.value("type", "") != "object")
            std::fprintf(stderr, "tool '%s': schema is not a JSON object schema\n", t.name.c_str()), ++g_fail;
    }
}

static bool Has(const std::vector<Tool>& tools, const char* name) {
    for (const Tool& t : tools)
        if (t.name == name) return true;
    return false;
}

int main() {
    const std::vector<Tool> player = Collect(mp::RegisterPlayerAgentTools);
    const std::vector<Tool> pro = Collect(mp::pro::RegisterProAgentTools);
    CheckTable(player);
    CheckTable(pro);

    // The base player alone, and Media Player Pro (Pro's tools + the player's) in the order
    // App::SetupAgentTools registers them.
    const std::vector<Tool> all = [&] {
        std::vector<Tool> v = pro;
        v.insert(v.end(), player.begin(), player.end());
        return v;
    }();
    for (const std::vector<Tool>* set : {&player, &all}) {
        std::set<std::string> seen;
        for (const Tool& t : *set)
            if (!seen.insert(t.name).second)
                std::fprintf(stderr, "duplicate agent tool '%s'\n", t.name.c_str()), ++g_fail;
        if (set->size() > mp::kAgentToolBudget)
            std::fprintf(stderr, "%zu agent tools, over the runtime's budget of %zu: the last %zu are refused\n",
                         set->size(), mp::kAgentToolBudget, set->size() - mp::kAgentToolBudget),
                ++g_fail;
    }

    // The two that #220 lost.
    CHECK(Has(pro, "open_decklink"));
    CHECK(Has(pro, "set_qc_view"));
    CHECK(Has(player, "set_view"));  // the VR180 / VR360 viewer's, unshadowed in Pro
    CHECK(!Has(pro, "set_view"));

    std::printf("agent tools: player %zu, pro %zu, Media Player Pro total %zu of %zu\n", player.size(), pro.size(),
                all.size(), mp::kAgentToolBudget);
    if (g_fail) std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return g_fail ? 1 : 0;
}
