#pragma once

#include "kimi_json.h"

#include <logonomic_cli_addon/adapter_runtime.h>

namespace kimi {

Json normalize_quota(const Json& response);
void handle_quota(logonomic::cli_addon::Invocation_context& context);

} // namespace kimi
