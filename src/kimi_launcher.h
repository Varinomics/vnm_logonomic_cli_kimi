#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kimi {

using logonomic::cli_addon::Environment;
using logonomic::cli_addon::Probe_output;
using logonomic::cli_addon::trim;
using logonomic::cli_addon::lower;
using logonomic::cli_addon::unquote;
using logonomic::cli_addon::base_name;
using logonomic::cli_addon::environment_value;
using logonomic::cli_addon::set_environment;
using logonomic::cli_addon::environment_pairs;
using logonomic::cli_addon::regular_file;
using logonomic::cli_addon::resolve_program;
using logonomic::cli_addon::native_image;
using logonomic::cli_addon::probe;

std::string path_string(const std::filesystem::path& path);
bool is_kimi_program(std::string_view value);
bool is_kimi_invocation(const std::vector<std::string>& argv);
bool recognize_launcher(std::string_view script, std::string_view runner);
std::optional<logonomic::cli_addon::Launcher_candidate> discover_launcher(
    const logonomic::cli_addon::Discover_request& request);
std::optional<std::string> verified_program(const logonomic::cli_addon::Launcher_request& launcher,
    const std::vector<std::string>& argv, const Environment& environment, std::string_view working_directory,
    bool resume, logonomic::cli_addon::Invocation_context& context);

} // namespace kimi
