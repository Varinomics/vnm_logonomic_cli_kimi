#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>

namespace kimi {

void handle_authoring(logonomic::cli_addon::Invocation_context& context);
int run_authoring_print(int argc, char** argv);

} // namespace kimi
