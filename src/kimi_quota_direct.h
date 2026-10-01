#pragma once

#include "kimi_json.h"
#include "kimi_launcher.h"

#include <optional>
#include <string>
#include <string_view>

namespace kimi {

struct Quota_route
{
    std::string host;
    std::string credential_name;
};

// These are the managed usage API's endpoint and file-slot rules. Unsupported
// configuration stays with the selected native CLI, which also owns renewal.
std::optional<Quota_route> direct_quota_route(std::string_view config, const Environment& environment);
std::optional<std::string> current_quota_access_token(const Json& record, double now, double remaining_seconds);
std::optional<Json> normalize_managed_quota(const Json& response);

} // namespace kimi
