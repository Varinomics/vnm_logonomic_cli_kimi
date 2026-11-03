#include "kimi_accounts.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

fs::path normalize(const fs::path& profile)
{
    if (profile.empty()) throw std::runtime_error("profile_unavailable");
    return fs::absolute(profile).lexically_normal();
}

addon::Account_record account(const fs::path& profile)
{
    addon::Account_record result;
    result.adapter_account_key = account_key(profile);
    result.display_name = addon::account_display_name(path_string(profile));
    result.profile_path = path_string(profile);
    // A profile that cannot be checked, such as one on an unreachable share, is
    // an unavailable account: it must not fail the discovery of every account.
    std::error_code error;
    result.status = fs::is_directory(profile, error)
        ? addon::Account_status::AVAILABLE
        : addon::Account_status::UNAVAILABLE;
    return result;
}

} // namespace

fs::path profile_for_environment(const addon::Launch_environment& launch,
    std::string_view normalization_directory, std::string_view default_home_fallback)
{
    const bool windows = launch.platform_id.starts_with("windows-");
    const auto value = [&](std::string_view name) -> const addon::Json_value* {
        for (const auto& entry : launch.effective_environment.as_object()) {
            if (windows ? lower(entry.name) == lower(std::string(name)) : entry.name == name) {
                return &entry.value;
            }
        }
        return nullptr;
    };
    // Match the declared managed-configuration@1 selector, including the
    // distinction between an absent platform home and a present empty home.
    const auto* configured = value("KIMI_CODE_HOME");
    auto profile = configured ? trim(configured->as_string()) : std::string{};
    if (profile.empty()) {
        const auto* home = value(windows ? "USERPROFILE" : "HOME");
        profile = path_string(addon::path_from_utf8(home ? home->as_string() : default_home_fallback) / ".kimi-code");
    }
    const auto path = addon::path_from_utf8(profile);
    const auto resolved = path.is_absolute() ? path : addon::path_from_utf8(normalization_directory) / path;
    if (!resolved.is_absolute()) throw std::runtime_error("profile_normalization_unavailable");
    return normalize(resolved);
}

std::string account_key(const fs::path& profile)
{
    auto normalized = normalize(profile);
#ifdef _WIN32
    const auto& source = normalized.native();
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source.data(),
        static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0) throw std::runtime_error("profile_normalization_failed");
    std::wstring folded(static_cast<std::size_t>(count), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source.data(), static_cast<int>(source.size()),
            folded.data(), count, nullptr, nullptr, 0) != count)
    {
        throw std::runtime_error("profile_normalization_failed");
    }
    normalized = fs::path(folded);
#endif
    return "profile-" + addon::to_hex(addon::Sha256::digest(path_string(normalized)));
}

fs::path profile_for_context(const addon::Session_launch_context& context)
{
    if (context.account_context) {
        if (!context.account_context->profile_path) throw std::runtime_error("account_unavailable");
        const auto profile = normalize(addon::path_from_utf8(*context.account_context->profile_path));
        if (account_key(profile) != context.account_context->adapter_account_key) throw std::runtime_error("account_profile_mismatch");
        return profile;
    }
    if (!context.launch_environment) throw std::runtime_error("launch_environment_unavailable");
    return profile_for_environment(*context.launch_environment, context.working_directory);
}

void handle_accounts(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto input = addon::decode_accounts_launcher_request(context.request().payload, &failure);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    if (!recognize_launcher(input->launcher.script_body)) {
        context.succeed(context.request().operation == "discover" ? addon::encode_accounts({})
            : addon::encode_account_resolution({std::nullopt}));
        return;
    }
    const auto selected = profile_for_environment(input->launch_environment,
        input->normalization_directory, input->default_home_fallback);
    if (context.request().operation == "resolve") {
        context.succeed(addon::encode_account_resolution({account_key(selected)}));
        return;
    }
    if (context.request().operation != "discover") {
        context.reject_unknown_operation();
        return;
    }
    std::vector<addon::Account_record> accounts{account(selected)};
    const auto parent = selected.parent_path();
    std::error_code error;
    if (fs::is_directory(parent, error)) {
        std::vector<fs::path> candidates;
        for (fs::directory_iterator entry(parent, error), end; !error && entry != end; entry.increment(error)) {
            if (context.cancel_requested()) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
            std::error_code entry_error;
            if (entry->is_directory(entry_error) && path_string(entry->path().filename()).starts_with(".kimi-code") &&
                fs::is_regular_file(entry->path() / "credentials/kimi-code.json", entry_error))
            {
                candidates.push_back(normalize(entry->path()));
            }
        }
        std::sort(candidates.begin(), candidates.end());
        for (const auto& candidate : candidates) {
            // The host overlays this exact path as the v1 selector. A sibling
            // whose path changes under trimming cannot be selected faithfully.
            const auto selector = path_string(candidate);
            if (trim(selector) != selector) continue;
            if (account_key(candidate) == account_key(selected)) continue;
            if (accounts.size() == 64) throw std::runtime_error("account_inventory_capacity");
            accounts.push_back(account(candidate));
        }
    }
    context.succeed(addon::encode_accounts(accounts));
}

std::string canonical_usage_session_id(std::string_view identity)
{
    if (!identity.starts_with("kimi:") || identity.size() == 5) return {};
    const auto end = identity.find(':', 5);
    const auto result = identity.substr(0, end);
    return result.size() > 5 ? std::string(result) : std::string{};
}

} // namespace kimi
