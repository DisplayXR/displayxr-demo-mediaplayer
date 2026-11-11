// SPDX-License-Identifier: Apache-2.0
//
// Media Player Pro's agent tools as a table (name, description, JSON Schema), apart from
// ProController so tests/agent_tools_test.cpp can check them with the player's (#220).
// ProController::RegisterTools registers them; ProController::DispatchTool runs them.
#pragma once

#include "AgentTools.h"

namespace mp::pro {

// Pro's tools, in registration order. App::SetupAgentTools registers them BEFORE the
// player's, so an overrun budget never cuts a station tool.
void RegisterProAgentTools(const AgentToolRegisterFn& reg);

}  // namespace mp::pro
