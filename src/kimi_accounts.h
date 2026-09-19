#pragma once

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace kimi {

std::string account_key(const std::filesystem::path& profile);
std::filesystem::path profile_for_context(const logonomic::cli_addon::Session_launch_context& context);
void handle_accounts(logonomic::cli_addon::Invocation_context& context);
void handle_legacy_data(logonomic::cli_addon::Invocation_context& context);
std::string canonical_usage_session_id(std::string_view identity);

} // namespace kimi
