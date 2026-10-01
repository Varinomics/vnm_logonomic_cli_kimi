#pragma once

#include "kimi_json.h"

#include <logonomic_cli_addon/adapter_runtime.h>

#include <string_view>

namespace kimi {

Json normalize_quota(const Json& response);
Json quota_probe_failure(std::string_view reason);
void handle_quota(logonomic::cli_addon::Invocation_context& context);

} // namespace kimi
