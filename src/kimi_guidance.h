#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace kimi {

std::filesystem::path session_guidance_path(const std::filesystem::path& home,
    std::string_view generation_nonce, std::string_view session_id);
// Publishes the complete UTF-8 instruction before returning a short native Read reminder.
std::string publish_session_guidance(const std::filesystem::path& path, std::string_view instruction);
void remove_session_guidance(const std::filesystem::path& path);

} // namespace kimi
