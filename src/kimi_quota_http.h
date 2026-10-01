#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace kimi {

// The helper and its native child remain in the host's supervised invocation.
// A transport failure may use the native route only before this deadline.
std::optional<std::string> get_managed_quota(
    std::string_view host,
    std::string_view bearer,
    std::chrono::steady_clock::time_point deadline);

} // namespace kimi
