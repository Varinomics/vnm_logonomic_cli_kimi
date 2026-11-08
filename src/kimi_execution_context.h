#pragma once

#include <logonomic_cli_addon/execution_context.h>
#include <logonomic_cli_addon/helper_ingress.h>

#include <filesystem>
#include <string_view>

namespace kimi {

// Capture at the authenticated SessionStart source, before asynchronous helper
// delivery. A persisted path alone never confirms this activation's default cwd.
logonomic::cli_addon::Execution_context_update capture_session_start_context(
    const std::filesystem::path& home, std::string_view session,
    std::string_view transition_nonce, std::string_view native_cwd);

void classify_session_start_context(
    const logonomic::cli_addon::Classify_event_request& request,
    logonomic::cli_addon::Classify_event_result& result);

} // namespace kimi
