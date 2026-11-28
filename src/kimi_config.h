#pragma once

#include "kimi_launcher.h"

#include <optional>
#include <string>
#include <string_view>

namespace kimi {

std::string hook_entry(std::string_view event, int timeout);
std::string updated_config(std::string config);
bool has_start_hook(const std::string& home);
struct Authoring_restriction
{
    const char* code;
    const char* detail;
};

std::optional<Authoring_restriction> authoring_profile_restriction(const std::filesystem::path& home,
    const std::optional<std::filesystem::path>& workspace = std::nullopt);

} // namespace kimi
