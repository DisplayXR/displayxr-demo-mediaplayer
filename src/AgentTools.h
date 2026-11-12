// SPDX-License-Identifier: Apache-2.0
//
// The player's agent tools (XR_DXR_mcp_tools) as a table: name, agent-facing description
// and JSON Schema per tool. App::SetupAgentTools registers them with the runtime and
// DispatchAgentTool runs them. They live apart from App.cpp so tests/agent_tools_test.cpp
// can check the whole set (with Pro's, pro/ProToolList.h) without the app (#220).
#pragma once

#include <cstddef>
#include <functional>

namespace mp {

using AgentToolRegisterFn = std::function<void(const char* name, const char* description, const char* schema)>;

// The runtime's per-process MCP tool budget: xrRegisterMCPToolDXR returns
// XR_ERROR_LIMIT_REACHED past it (runtime 2.66). Not queryable through the extension, so it
// is pinned here and the test keeps the player + Pro set within it (#220).
constexpr std::size_t kAgentToolBudget = 32;

// The base player's tools, in registration order.
void RegisterPlayerAgentTools(const AgentToolRegisterFn& reg);

}  // namespace mp
