#include "kimi_guidance.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/sha256.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <fstream>
#include <stdexcept>

namespace kimi {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

fs::path session_guidance_path(const fs::path& home,
    std::string_view generation_nonce, std::string_view session_id)
{
    if (session_id.empty()) throw std::runtime_error("guidance_session_missing");
    // Neither the session identity nor host authority becomes a pathname.
    addon::Sha256 identity;
    identity.update(generation_nonce);
    identity.update("\n");
    identity.update(session_id);
    return home / "scratch" / "logonomic" / addon::to_hex(identity.finish()) / "guidance.md";
}

std::string publish_session_guidance(const fs::path& path, std::string_view instruction)
{
    if (!addon::is_valid_utf8(instruction)) throw std::runtime_error("guidance_utf8_invalid");
    fs::create_directories(path.parent_path());
    fs::permissions(path.parent_path(), fs::perms::owner_all, fs::perm_options::replace);
    const auto temporary = path.parent_path() / (addon::random_nonce_hex() + ".tmp");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(instruction.data(), static_cast<std::streamsize>(instruction.size()));
        output.close();
        if (!output) throw std::runtime_error("guidance_write_failed");
        fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("guidance_replace_failed");
        }
#else
        fs::rename(temporary, path);
#endif
    }
    catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
    return "Read " + addon::serialize_json(addon::Json_value(path_string(path))) +
        " for Logonomic session guidance using an available file-reading tool (prefer native Read). "
        "Apply it without quoting it, then continue the user's request. "
        "If no file-reading tool is available, continue normally.";
}

void remove_session_guidance(const fs::path& path)
{
    // Remove only this helper's fixed file and its empty session directory.
    fs::remove(path);
    fs::remove(path.parent_path());
}

} // namespace kimi
