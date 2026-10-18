#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/usage_session.h>

#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace kimi {

class Usage_ownership_unavailable final : public std::runtime_error
{
public:
    Usage_ownership_unavailable() : std::runtime_error("usage_ownership_unavailable") {}
};

// In-process collector used by package tests. The adapter streams disk-backed
// snapshots instead of retaining this vector across the history.
std::vector<logonomic::cli_addon::Usage_record> scan_usage(
    const std::filesystem::path& profile,
    const std::filesystem::path& storage,
    std::string_view mode,
    const std::optional<logonomic::cli_addon::Usage_native_file_cache>& seed,
    logonomic::cli_addon::Invocation_context& context,
    const logonomic::cli_addon::Usage_cache_selection* selection = nullptr);
void handle_usage(logonomic::cli_addon::Invocation_context& context);
void handle_usage_session(logonomic::cli_addon::Invocation_context& context);
std::optional<logonomic::cli_addon::Usage_session_fact> describe_usage_session(
    const logonomic::cli_addon::Usage_session_identity& identity);

} // namespace kimi
