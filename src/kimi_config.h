#pragma once

#include "kimi_launcher.h"

#include <string>
#include <string_view>

namespace kimi {

std::string hook_entry(std::string_view event, int timeout);
std::string updated_config(std::string config);
bool has_start_hook(const std::string& home);
bool authoring_profile_admitted(const std::filesystem::path& home, const std::filesystem::path& workspace);

} // namespace kimi
