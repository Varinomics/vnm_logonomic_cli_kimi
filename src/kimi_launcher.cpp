#include "kimi_launcher.h"
#include "kimi_json.h"
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <regex>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace kimi {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
std::string trim(std::string_view value)
{
    std::size_t first = value.size(), last = 0;
    for (std::size_t offset = 0; offset < value.size();) {
        const auto begin = offset;
        const auto lead = static_cast<unsigned char>(value[offset++]);
        std::uint32_t scalar = lead;
        const auto continuation = lead >= 0xF0 ? 3u : lead >= 0xE0 ? 2u : lead >= 0xC0 ? 1u : 0u;
        if (continuation != 0 && offset + continuation <= value.size()) {
            scalar = lead & ((1u << (6u - continuation)) - 1u);
            for (unsigned index = 0; index < continuation; ++index) {
                scalar = (scalar << 6) | (static_cast<unsigned char>(value[offset++]) & 0x3F);
            }
        }
        const bool whitespace = scalar == 0x20 || (scalar >= 9 && scalar <= 13) || scalar == 0x85 ||
            scalar == 0xA0 || scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200A) ||
            scalar == 0x2028 || scalar == 0x2029 || scalar == 0x202F || scalar == 0x205F || scalar == 0x3000;
        if (!whitespace) { first = std::min(first, begin); last = offset; }
    }
    return last == 0 ? std::string{} : std::string(value.substr(first, last - first));
}

std::string lower(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    return value;
}

std::string unquote(std::string_view value)
{
    std::string result = trim(value);
    if (result.size() >= 2 &&
        ((result.front() == '\'' && result.back() == '\'') ||
         (result.front() == '"'  && result.back() == '"')))
    {
        return result.substr(1, result.size() - 2);
    }
    return result;
}

std::string base_name(std::string_view value)
{
    std::string name = lower(unquote(value));
    const auto separator = name.find_last_of("/\\");
    if (separator != std::string::npos) name.erase(0, separator + 1);
    for (const std::string_view suffix : {".exe", ".cmd", ".ps1", ".bat"}) {
        if (name.ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    return name;
}

std::string path_string(const fs::path& path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

bool equal_name(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    return lower(std::string(left)) == lower(std::string(right));
#else
    return left == right;
#endif
}

std::string environment_value(const Environment& environment, std::string_view name)
{
    for (const auto& [key, value] : environment) if (equal_name(key, name)) return value;
    return {};
}

void set_environment(Environment& environment, std::string name, std::string value)
{
    std::erase_if(environment, [&](const auto& entry) { return equal_name(entry.first, name); });
    environment.emplace_back(std::move(name), std::move(value));
}

std::optional<fs::path> regular_file(const fs::path& path)
{
    std::error_code error;
    const auto canonical = fs::canonical(path, error);
    return !error && fs::is_regular_file(canonical, error) && !error
        ? std::optional(canonical) : std::nullopt;
}

std::optional<fs::path> resolve_program(
    std::string_view program, const Environment& environment, std::string_view working_directory)
{
    const auto path = addon::path_from_utf8(program);
    const bool has_working_directory = addon::path_from_utf8(working_directory).is_absolute();
    const auto usable_program = [](const fs::path& candidate) -> std::optional<fs::path> {
        const auto file = regular_file(candidate);
#ifndef _WIN32
        if (file && access(file->c_str(), X_OK) != 0) return std::nullopt;
#endif
        return file;
    };
    if (path.is_absolute()) return usable_program(path);
    if (path.has_parent_path()) {
        return has_working_directory ? usable_program(addon::path_from_utf8(working_directory) / path) : std::nullopt;
    }
    std::vector<fs::path> candidates;
    {
        const auto search_path = environment_value(environment, "PATH");
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::istringstream parts(search_path);
        for (std::string part; std::getline(parts, part, separator);) {
            if (part.empty()) continue;
            auto directory = addon::path_from_utf8(unquote(part));
            if (!directory.is_absolute()) {
                if (!has_working_directory) continue;
                directory = addon::path_from_utf8(working_directory) / directory;
            }
            candidates.push_back(directory / path);
        }
    }
    for (const auto& candidate : candidates) {
#ifdef _WIN32
        if (!candidate.has_extension()) {
            auto extensions = environment_value(environment, "PATHEXT");
            if (std::none_of(environment.begin(), environment.end(), [](const auto& entry) {
                    return equal_name(entry.first, "PATHEXT");
                })) extensions = ".COM;.EXE;.BAT;.CMD";
            std::istringstream parts(extensions);
            for (std::string extension; std::getline(parts, extension, ';');) {
                extension = trim(extension);
                if (extension.empty()) continue;
                if (!extension.starts_with('.')) extension.insert(extension.begin(), '.');
                if (const auto file = usable_program(addon::path_from_utf8(path_string(candidate) + extension))) return file;
            }
            continue;
        }
#endif
        if (const auto file = usable_program(candidate)) return file;
    }
    return std::nullopt;
}

bool native_image(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 4> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
#ifdef _WIN32
    return file.gcount() >= 2 && header[0] == 'M' && header[1] == 'Z';
#else
    if (file.gcount() != 4) return false;
    const auto magic = (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8) | header[3];
    return magic == 0x7f454c46 || magic == 0xfeedface || magic == 0xfeedfacf ||
        magic == 0xcefaedfe || magic == 0xcffaedfe || magic == 0xcafebabe || magic == 0xbebafeca;
#endif
}

#ifdef _WIN32
std::wstring wide(std::string_view text)
{
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (size == 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring quote_argument(std::wstring_view argument)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += character;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#endif

std::optional<Probe_output> probe(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, int timeout_ms, addon::Invocation_context& context)
{
    if (context.cancel_requested()) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    Probe_output result;
    bool valid = true;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output_read, output_write, errors_read, errors_write;
    if (!CreatePipe(&output_read.value, &output_write.value, &security, 0) ||
        !CreatePipe(&errors_read.value, &errors_write.value, &security, 0)) return std::nullopt;
    SetHandleInformation(output_read.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errors_read.value, HANDLE_FLAG_INHERIT, 0);
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE) return std::nullopt;
    std::wstring command;
    for (const auto& argument : argv) {
        if (!command.empty()) command += L' ';
        command += quote_argument(wide(argument));
    }
    std::vector<std::wstring> variables;
    for (const auto& [name, value] : environment) variables.push_back(wide(name + "=" + value));
    std::sort(variables.begin(), variables.end(), [](const auto& left, const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    std::wstring environment_block;
    for (const auto& variable : variables) { environment_block += variable; environment_block += L'\0'; }
    environment_block += L'\0';
    if (variables.empty()) environment_block += L'\0';
    // Restrict handle inheritance: concurrent adapter invokes must not retain
    // each other's pipe handles and keep a completed probe alive.
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attributes(attribute_bytes);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_bytes)) return std::nullopt;
    HANDLE handles[]{input.value, output_write.value, errors_write.value};
    const bool updated = UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof(handles), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput  = input.value;
    startup.StartupInfo.hStdOutput = output_write.value;
    startup.StartupInfo.hStdError  = errors_write.value;
    startup.lpAttributeList = attribute_list;
    PROCESS_INFORMATION information{};
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const auto directory = wide(working_directory);
    const bool created = updated && job.value &&
        SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
        CreateProcessW(wide(argv.front()).c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment_block.data(), directory.empty() ? nullptr : directory.c_str(),
            &startup.StartupInfo, &information);
    DeleteProcThreadAttributeList(attribute_list);
    if (!created) return std::nullopt;
    Handle process{information.hProcess}, thread{information.hThread};
    CloseHandle(output_write.value); output_write.value = nullptr;
    CloseHandle(errors_write.value); errors_write.value = nullptr;
    if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.value, 1);
        WaitForSingleObject(process.value, INFINITE);
        return std::nullopt;
    }
    const auto drain = [&](HANDLE pipe, std::string& output) {
        DWORD available = 0;
        while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            std::array<char, 4096> buffer{};
            DWORD count = 0;
            if (!ReadFile(pipe, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) break;
            if (output.size() + count > 65536) { valid = false; break; }
            output.append(buffer.data(), count);
        }
    };
    for (;;) {
        drain(output_read.value, result.output);
        drain(errors_read.value, result.errors);
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) break;
        if (!valid || context.cancel_requested() || std::chrono::steady_clock::now() >= deadline) {
            valid = false;
            TerminateJobObject(job.value, 1);
            WaitForSingleObject(process.value, INFINITE);
            break;
        }
        context.wait_for_cancel(10);
    }
    TerminateJobObject(job.value, 1);
    drain(output_read.value, result.output);
    drain(errors_read.value, result.errors);
    DWORD exit_code = 1;
    valid = GetExitCodeProcess(process.value, &exit_code) && exit_code == 0 && valid;
#else
    int output_pipe[2], errors_pipe[2];
    if (pipe2(output_pipe, O_CLOEXEC) != 0) return std::nullopt;
    if (pipe2(errors_pipe, O_CLOEXEC) != 0) {
        close(output_pipe[0]); close(output_pipe[1]); return std::nullopt;
    }
    const int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        for (int descriptor : {output_pipe[0], output_pipe[1], errors_pipe[0], errors_pipe[1]}) close(descriptor);
        return std::nullopt;
    }
    std::vector<std::string> environment_strings;
    for (const auto& [name, value] : environment) environment_strings.push_back(name + "=" + value);
    std::vector<char*> arguments, variables;
    for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
    for (auto& variable : environment_strings) variables.push_back(variable.data());
    arguments.push_back(nullptr);
    variables.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors_pipe[1], STDERR_FILENO);
    const std::string directory(working_directory);
    if (!directory.empty()) posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    pid_t process = 0;
    const int spawned = posix_spawn(&process, argv.front().c_str(), &actions, &attributes,
        arguments.data(), variables.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(input); close(output_pipe[1]); close(errors_pipe[1]);
    if (spawned != 0) { close(output_pipe[0]); close(errors_pipe[0]); return std::nullopt; }
    fcntl(output_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errors_pipe[0], F_SETFL, O_NONBLOCK);
    const auto drain = [&](int descriptor, std::string& output) {
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = read(descriptor, buffer.data(), buffer.size());
            if (count <= 0) break;
            if (output.size() + static_cast<std::size_t>(count) > 65536) { valid = false; break; }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
    };
    int status = 0;
    bool reaped = false;
    bool child_owned = true;
    for (;;) {
        drain(output_pipe[0], result.output);
        drain(errors_pipe[0], result.errors);
        const auto waited = waitpid(process, &status, WNOHANG);
        if (waited == process) { reaped = true; child_owned = false; break; }
        if (waited < 0 && errno != EINTR) {
            if (errno == ECHILD) child_owned = false;
            valid = false;
            break;
        }
        if (!valid || context.cancel_requested() || std::chrono::steady_clock::now() >= deadline) {
            valid = false;
            break;
        }
        pollfd descriptors[]{{output_pipe[0], POLLIN, 0}, {errors_pipe[0], POLLIN, 0}};
        poll(descriptors, 2, 10);
    }
    if (child_owned && !reaped) {
        // The unreaped leader still reserves its PID. Never signal after a
        // successful reap or ECHILD, when that number can identify a new task.
        // Descendants retain the custodian's group; creating a private group
        // here would violate the SDK's no-detach contract.
        kill(process, SIGKILL);
        pid_t waited;
        do { waited = waitpid(process, &status, 0); } while (waited < 0 && errno == EINTR);
        reaped = waited == process;
    }
    drain(output_pipe[0], result.output);
    drain(errors_pipe[0], result.errors);
    close(output_pipe[0]); close(errors_pipe[0]);
    valid = valid && reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    return valid && !context.cancel_requested() ? std::optional(std::move(result)) : std::nullopt;
}

Environment environment_pairs(const Json& value)
{
    Environment result;
    for (const auto& entry : value.as_object()) result.emplace_back(entry.name, entry.value.as_string());
    return result;
}

std::vector<std::string> literal_argv(std::string_view script)
{
    std::vector<std::string> result;
    std::string current;
    bool quoted = false;
    int quotes = 0;
    for (const char character : script) {
        if (character == '"') {
            if (++quotes == 3) { current += '"'; quotes = 0; }
            continue;
        }
        if (quotes == 1) quoted = !quoted;
        quotes = 0;
        if (!quoted && std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) { result.push_back(current); current.clear(); }
        }
        else current += character;
    }
    if (!current.empty()) result.push_back(current);
    return result;
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
        const auto nested = literal_argv(argv.back());
        return !nested.empty() && is_kimi_program(nested.front());
    }
    return false;
}

namespace {
const std::string& semver_pattern()
{
    static const std::string pattern =
        R"((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"
        R"((?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?)"
        R"((?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)";
    return pattern;
}

bool semver(std::string_view value)
{
    static const std::regex pattern("^" + semver_pattern() + "$", std::regex::ECMAScript);
    return std::regex_match(value.begin(), value.end(), pattern);
}

std::optional<std::string> stream_version(std::string_view value)
{
    if (value.size() > 65536 || value.find('\0') != std::string_view::npos ||
        !addon::is_valid_utf8(value)) return std::nullopt;
    static const std::regex labelled("^(?:kimi-code|kimi)(?:\\s+version)?(?:\\s*[:=]\\s*|\\s+)v?(" +
        semver_pattern() + ")(?:\\s+\\([^\\r\\n()]{1,128}\\))?\\s*$", std::regex::icase);
    std::vector<std::string> lines;
    std::string text(value);
    std::replace(text.begin(), text.end(), '\r', '\n');
    std::istringstream input(text);
    std::string version;
    for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.empty()) continue;
        lines.push_back(line);
        std::smatch match;
        if (std::regex_match(line, match, labelled)) {
            if (!version.empty() && version != match[1].str()) return std::nullopt;
            version = match[1].str();
        }
    }
    if (!version.empty()) return version;
    if (lines.size() == 1) {
        auto candidate = lines.front();
        if (candidate.starts_with('v') || candidate.starts_with('V')) candidate.erase(0, 1);
        if (semver(candidate)) return candidate;
    }
    return std::string{};
}

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

bool environment_name(std::string_view name)
{
    if (name.empty() || (name.front() != '_' &&
        !std::isalpha(static_cast<unsigned char>(name.front())))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return character == '_' || (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
    });
}

std::optional<std::pair<std::string, std::string>> assignment(std::string_view text)
{
    const std::string value = unquote(text);
    const auto separator = value.find('=');
    if (separator == std::string::npos) return std::nullopt;
    const std::string name = trim(value.substr(0, separator));
    if (!environment_name(name) || value.find('\0') != std::string::npos) return std::nullopt;
    return std::pair{name, unquote(value.substr(separator + 1))};
}

std::vector<std::string> tokens(std::string_view text)
{
    static const std::regex pattern(R"re("(?:\\.|[^"])*"|'[^']*'|\S+)re");
    const std::string input(text);
    std::vector<std::string> result;
    for (std::sregex_iterator match(input.begin(), input.end(), pattern), end;
         match != end; ++match)
    {
        result.push_back(match->str());
    }
    return result;
}

std::vector<std::string> segments(std::string_view line)
{
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '\\' && quote == '"' && index + 1 < line.size()) {
            current += character;
            current += line[++index];
            continue;
        }
        if (character == quote) quote = 0;
        else
            if (!quote && (character == '\'' || character == '"')) quote = character;
        else
            if (!quote && (character == ';' || character == '&' || character == '|')) {
            if (!trim(current).empty()) result.push_back(trim(current));
            current.clear();
            continue;
        }
        current += character;
    }
    if (!trim(current).empty()) result.push_back(trim(current));
    return result;
}

std::optional<Launcher_command> recognize(std::string_view script, int depth)
{
    if (depth == 0) return std::nullopt;
    Environment inherited;
    std::istringstream lines{std::string(script)};
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        if (line.empty() || line.starts_with('#') || line.starts_with("//") ||
            lower(line).starts_with("rem ")) continue;
        for (std::string segment : segments(line)) {
            const std::string normalized = lower(segment);
            std::optional<std::pair<std::string, std::string>> standalone;
            if (normalized.starts_with("set ")) standalone = assignment(segment.substr(4));
            else
            if (normalized.starts_with("export ")) standalone = assignment(segment.substr(7));
            else
            if (normalized.starts_with("$env:")) standalone = assignment(segment.substr(5));
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
            const auto words = tokens(segment);
            std::size_t index = 0;
            while (index < words.size()) {
                const auto value = assignment(words[index]);
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
                    auto command = recognize(unquote(tail), depth - 1);
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

bool recognize_launcher(std::string_view script)
{
    return recognize(script, 8).has_value();
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
    bool resume, addon::Invocation_context& context)
{
    const auto args = literal_argv(launcher.script_body);
    if (args.empty() || !is_kimi_program(args.front())) return std::nullopt;
    if (launcher.runner_key == "direct") {
        if (args != supplied) return std::nullopt;
    }
    else {
        if (launcher.runner_key != "powershell" || launcher.script_body.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \t-_. /\\:=") != std::string::npos) return std::nullopt;
        if (supplied != std::vector<std::string>{"powershell.exe", "-NoLogo", "-Command", launcher.script_body}) return std::nullopt;
    }
    if (!resume && !(args.size() == 1 || (args.size() == 2 && args[1] == "--yolo"))) return std::nullopt;
    if (resume) {
        for (std::size_t index = 1; index < args.size(); ++index) {
            const auto& option = args[index];
            if (option == "--yolo" || option == "-y" || option == "--auto" || option == "--plan") continue;
            if (option != "--model" && option != "-m" && option != "--skills-dir" && option != "--add-dir") return std::nullopt;
            if (++index == args.size() || args[index].empty() || args[index].starts_with('-')) return std::nullopt;
        }
    }
    const auto program = resolve_program(args.front(), environment, working_directory);
    if (!program) return std::nullopt;
    std::vector<fs::path> identities{*program};
    if (!native_image(*program)) {
#ifdef _WIN32
        return std::nullopt;
#else
        if (resume) return std::nullopt;
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
            result += path_string(target) + ':' + std::to_string(size) + ':' + std::to_string(modified.time_since_epoch().count()) + '\n';
        }
        return result;
    };
    const auto before = fingerprint();
    auto cache_context = object({{"identity", Json(before)}, {"working_directory", Json(working_directory)}, {"resume", Json(resume)}});
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
    const auto output = probe({path_string(*program), resume ? "--help" : "--version"}, environment, working_directory, 10000, context);
    if (!output || before.empty() || before != fingerprint()) return std::nullopt;
    if (resume) {
        static const std::regex selector(R"((^|\n)[^\n]*--session[ \t]+\[id\]([ \t]|$))");
        if (!std::regex_search(output->output, selector)) return std::nullopt;
    }
    else {
        const auto output_version = stream_version(output->output);
        const auto error_version = stream_version(output->errors);
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
