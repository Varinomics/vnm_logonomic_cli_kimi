#pragma once

#include "kimi_json.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace kimi {

struct Resume_record
{
    std::string provider_session_id;
    std::optional<std::filesystem::path> kimi_code_home;
};

struct Native_session_lookup
{
    logonomic::cli_addon::Resume_availability availability =
        logonomic::cli_addon::Resume_availability::UNKNOWN;
    std::filesystem::path session_directory;
    std::optional<Json> metadata;
    bool selector_ambiguous = false;
};

// The native selector is stable within its original KIMI_CODE_HOME. Runtime
// activation, cwd and transcript timestamps are deliberately not identity.
Json make_resume_record(const std::filesystem::path& home, std::string_view session);
std::optional<Resume_record> decode_kimi_resume_record(const Json& record);
Native_session_lookup lookup_native_session(const std::filesystem::path& home, std::string_view session);
bool resume_scope_matches(const Resume_record& record,
    const logonomic::cli_addon::Session_launch_context& context);
void handle_resume_inspection(logonomic::cli_addon::Invocation_context& context);

} // namespace kimi
