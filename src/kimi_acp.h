#pragma once

#include <logonomic_cli_addon/session_contracts.h>

#include <optional>
#include <string>
#include <string_view>

namespace kimi {

// Kimi Code's dispatcher name for an MCP tool. Kimi skips a second tool
// whose name equals one it already registered.
std::string mcp_tool_name(std::string_view server, std::string_view tool);
// The first pair of required actions that Kimi would register under one name.
std::optional<std::string> required_action_collision(const logonomic::cli_addon::Delegated_prepare_request& request);
std::string initial_acp_input(const logonomic::cli_addon::Delegated_prepare_request& request);
std::string initial_acp_state(const logonomic::cli_addon::Delegated_prepare_request& request);
logonomic::cli_addon::Structured_inspect_result inspect_acp(
    const logonomic::cli_addon::Structured_inspection& inspection);

} // namespace kimi
