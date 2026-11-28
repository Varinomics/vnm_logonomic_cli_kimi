#include "kimi_launcher.h"
#include "kimi_json.h"
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <regex>
#include <system_error>
#include <utility>
namespace json_util = logonomic::cli_addon::json;

namespace kimi {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
std::string path_string(const fs::path& path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

bool is_kimi_program(std::string_view value)
{
    const auto name = base_name(value);
    return name == "kimi" || name == "kimi-cli" || name == "kimi-code";
}

bool is_kimi_invocation(const std::vector<std::string>& argv)
{
    if (argv.empty()) return false;
    if (is_kimi_program(argv.front())) return true;
    const auto executable = base_name(argv.front());
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto argument = lower(argv[index]);
        bool command = false;
        if (executable == "powershell" || executable == "pwsh") command = argument == "-command" || argument == "-c";
        else
        if (executable == "cmd") {
            if (argument == "/k") return false;
            command = argument == "/c";
        }
        else
        if (executable == "sh" || executable == "bash") {
            if (!argv[index].starts_with('-')) continue;
            if (argv[index].find('i') != std::string::npos) return false;
            command = argv[index].find('c') != std::string::npos;
        }
        else return false;
        if (!command) continue;
        if (index + 2 != argv.size() || argv.back().find_first_of(";&|<>$`%\r\n") != std::string::npos) return false;
        const auto nested = addon::literal_argv(argv.back());
        return !nested.empty() && is_kimi_program(nested.front());
    }
    return false;
}

namespace {
struct Launcher_command
{
    std::string program;
    std::vector<std::string> arguments;
    Environment environment;
};

bool package_flag(std::string_view value)
{
    return value == "-y" || value == "--yes" || value == "--quiet" ||
        value == "--silent" || value == "--no-install" || value == "--bun";
}

std::optional<Launcher_command> recognize(std::string_view script, std::string_view runner, int depth)
{
    if (depth == 0) return std::nullopt;
    Environment inherited;
    std::istringstream lines{std::string(script)};
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        if (line.empty() || line.starts_with('#') || line.starts_with("//") ||
            lower(line).starts_with("rem ")) continue;
        for (std::string segment : addon::launcher_segments(line, runner, addon::Launcher_whitespace::UTF8)) {
            const std::string normalized = lower(segment);
            std::optional<std::pair<std::string, std::string>> standalone;
            if (normalized.starts_with("set ")) standalone = addon::environment_assignment(
                segment.substr(4), addon::Launcher_whitespace::UTF8);
            else
            if (normalized.starts_with("export ")) standalone = addon::environment_assignment(
                segment.substr(7), addon::Launcher_whitespace::UTF8);
            else
            if (normalized.starts_with("$env:")) standalone = addon::environment_assignment(
                segment.substr(5), addon::Launcher_whitespace::UTF8);
            if (standalone) {
                inherited.push_back(*standalone);
                continue;
            }
            Environment environment = inherited;
            // Leading assignment values may contain quoted whitespace, unlike
            // the command-token grammar used for the remainder of the line.
            static const std::regex leading(R"re(^([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|'[^']*'|[^\s]*)\s*)re");
            std::smatch match;
            while (std::regex_search(segment, match, leading)) {
                environment.emplace_back(match[1].str(), unquote(match[2].str()));
                segment.erase(0, match.length());
            }
            const auto words = addon::launcher_tokens(segment, runner);
            std::size_t index = 0;
            while (index < words.size()) {
                const auto value = addon::environment_assignment(words[index], addon::Launcher_whitespace::UTF8);
                const auto prefix = lower(unquote(words[index]));
                if (value) environment.push_back(*value);
                else
            if (prefix != "call" && prefix != "command" && prefix != "exec" && prefix != "&") break;
                ++index;
            }
            if (index == words.size()) continue;
            const std::string program = unquote(words[index]);
            const std::string base = base_name(program);
            if (is_kimi_program(program) || base.starts_with("kimi-code@") || lower(program) == "@moonshot-ai/kimi-code" || lower(program).starts_with("@moonshot-ai/kimi-code@")) return Launcher_command{program, {}, environment};
            if (base == "npx" || base == "bunx") {
                std::vector<std::string> arguments;
                for (++index; index < words.size(); ++index) {
                    const auto argument = unquote(words[index]);
                    arguments.push_back(argument);
                    if (argument.starts_with('-')) {
                        if (!package_flag(argument)) break;
                    }
                    else {
                        const auto package = lower(argument);
                        if (package == "@moonshot-ai/kimi-code" || package.starts_with("@moonshot-ai/kimi-code@") || package == "kimi-code" || package.starts_with("kimi-code@")) {
                            return Launcher_command{program, arguments, environment};
                        }
                        break;
                    }
                }
            }
            else
            if (base == "cmd" || base == "powershell" || base == "pwsh" ||
                     base == "bash" || base == "sh")
            {
                for (++index; index + 1 < words.size(); ++index) {
                    const auto argument = lower(unquote(words[index]));
                    if (argument != "/c" && argument != "-c" && argument != "-command") continue;
                    std::string tail;
                    for (++index; index < words.size(); ++index) {
                        if (!tail.empty()) tail += ' ';
                        tail += words[index];
                    }
                    auto command = recognize(unquote(tail),
                        base == "cmd" ? "cmd" : base == "bash" || base == "sh" ? "bash" : "powershell", depth - 1);
                    if (command) {
                        command->environment.insert(command->environment.begin(), environment.begin(), environment.end());
                        return command;
                    }
                    break;
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace

bool recognize_launcher(std::string_view script, std::string_view runner)
{
    return recognize(script, runner, 8).has_value();
}

std::optional<addon::Launcher_candidate> discover_launcher(const addon::Discover_request& request)
{
    const auto environment = environment_pairs(request.ambient_environment);
    for (const auto name : {"kimi", "kimi-code", "kimi-cli"}) {
        if (const auto program = resolve_program(name, environment, {})) {
            addon::Launcher_candidate result;
            result.candidate_id = "kimi";
            result.display_name = "Kimi";
            result.script_body = "\"" + path_string(*program) + "\"";
            result.runner_key = "direct";
            result.is_default = true;
            return result;
        }
    }
    return std::nullopt;
}

std::optional<std::string> verified_program(const addon::Launcher_request& launcher,
    const std::vector<std::string>& supplied, const Environment& environment, std::string_view working_directory,
    Native_entrypoint entrypoint, addon::Invocation_context& context)
{
    const bool resume = entrypoint == Native_entrypoint::RESUME;
    const bool authoring = entrypoint == Native_entrypoint::AUTHORING;
    const auto args = addon::literal_argv(launcher.script_body);
    if (args.empty() || !is_kimi_program(args.front())) return std::nullopt;
    if (launcher.runner_key == "direct") {
        if (args != supplied) return std::nullopt;
    }
    else {
        if (launcher.runner_key != "powershell" || launcher.script_body.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \t-_. /\\:=") != std::string::npos) return std::nullopt;
        if (supplied != std::vector<std::string>{"powershell.exe", "-NoLogo", "-Command", launcher.script_body}) return std::nullopt;
    }
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto& option = args[index];
        if (option == "--yolo" || option == "-y" || option == "--auto" || option == "--plan") {
            continue;
        }
        if (option != "--model" && option != "-m" &&
            (!resume || (option != "--skills-dir" && option != "--add-dir")))
        {
            return std::nullopt;
        }
        if (++index == args.size() || args[index].empty() || args[index].starts_with('-')) {
            return std::nullopt;
        }
    }
    const auto program = resolve_program(args.front(), environment, working_directory);
    if (!program) return std::nullopt;
    std::vector<fs::path> identities{*program};
    if (!native_image(*program)) {
#ifdef _WIN32
        return std::nullopt;
#else
        if (resume || authoring) return std::nullopt;
        std::ifstream file(*program, std::ios::binary);
        std::array<char, 257> bytes{};
        file.read(bytes.data(), bytes.size());
        const std::string header(bytes.data(), static_cast<std::size_t>(file.gcount()));
        const auto end = header.find('\n');
        if (!header.starts_with("#!") || end < 3 || end >= 256) return std::nullopt;
        const auto interpreter = header.substr(2, end - 2);
        if (std::any_of(interpreter.begin(), interpreter.end(), [](unsigned char byte) { return byte <= 0x20 || byte == 0x7f; }) ||
            interpreter.find_first_of(";&|`$<>()") != std::string::npos) return std::nullopt;
        const auto raw = addon::path_from_utf8(interpreter);
        const auto target = regular_file(raw);
        if (!raw.is_absolute() || raw.filename() == "env" || !target || !native_image(*target) || access(target->c_str(), X_OK) != 0) {
            return std::nullopt;
        }
        identities.push_back(raw);
        identities.push_back(*target);
#endif
    }
    const auto fingerprint = [&]() {
        std::string result;
        for (const auto& path : identities) {
            std::error_code error;
            const auto target = fs::canonical(path, error);
            if (error) return std::string{};
            const auto size = fs::file_size(path, error);
            if (error) return std::string{};
            const auto modified = fs::last_write_time(path, error);
            if (error) return std::string{};
            result += path_string(target) + ':' + std::to_string(size) + ':' + addon::file_time_ticks_text(modified) + '\n';
        }
        return result;
    };
    const auto before = fingerprint();
    auto cache_context = json_util::object({{"identity", Json(before)}, {"working_directory", Json(working_directory)},
        {"entrypoint", Json(static_cast<int>(entrypoint))}});
    auto cache_environment = Json::object();
    for (const auto& [name, value] : environment) cache_environment.set(name, Json(value));
    cache_context.set("environment", std::move(cache_environment));
    const auto cache_key = addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(cache_context)));
    static std::mutex cache_mutex;
    static std::map<std::string, std::string> cache;
    {
        std::lock_guard lock(cache_mutex);
        if (!context.cancel_requested()) {
            if (const auto found = cache.find(cache_key); found != cache.end()) return found->second;
        }
    }
    const auto deadline = std::min<std::int64_t>(context.request().deadline_ms,
        resume ? addon::limits::k_resume_prepare_deadline_ms : addon::limits::k_delegated_prepare_deadline_ms);
    const auto output = probe({path_string(*program), resume || authoring ? "--help" : "--version"},
        environment, working_directory, static_cast<int>(deadline), context);
    if (!output || before.empty() || before != fingerprint()) return std::nullopt;
    if (authoring) {
        const auto& help = output->output;
        if (help.find("--agent-file <") == std::string::npos || help.find("--skills-dir <") == std::string::npos ||
            help.find("--output-format <") == std::string::npos || help.find("stream-json") == std::string::npos)
        {
            return std::nullopt;
        }
    }
    else
    if (resume) {
        static const std::regex selector(R"((^|\n)[^\n]*--session[ \t]+\[id\]([ \t\r\n]|$))");
        if (!std::regex_search(output->output, selector)) return std::nullopt;
    }
    else {
        constexpr std::string_view labels[] = {"kimi-code", "kimi"};
        const auto output_version = addon::parse_version_stream(output->output, labels);
        const auto error_version  = addon::parse_version_stream(output->errors, labels);
        if (!output_version || !error_version || (!output_version->empty() && !error_version->empty() &&
            *output_version != *error_version)) return std::nullopt;
        const auto version = output_version->empty() ? *error_version : *output_version;
        if (version.empty()) return std::nullopt;
        auto remaining = std::string_view(version).substr(0, version.find_first_of("-+"));
        bool greater = false;
        for (const std::string_view minimum : {"0", "39", "1"}) {
            const auto dot = remaining.find('.');
            const auto component = remaining.substr(0, dot);
            if (component.size() != minimum.size()) {
                if (component.size() < minimum.size()) return std::nullopt;
                greater = true; break;
            }
            if (component != minimum) {
                if (component < minimum) return std::nullopt;
                greater = true; break;
            }
            if (dot != std::string_view::npos) remaining.remove_prefix(dot + 1);
        }
        const auto suffix = version.find_first_of("-+");
        if (!greater && suffix != std::string::npos && version[suffix] == '-') return std::nullopt;
    }
    const auto result = path_string(*program);
    {
        std::lock_guard lock(cache_mutex);
        if (cache.size() >= 256) cache.erase(cache.begin());
        cache.emplace(cache_key, result);
    }
    return result;
}

} // namespace kimi
