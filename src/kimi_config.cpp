#include "kimi_config.h"

#include <logonomic_cli_addon/package_source.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace kimi {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace {
std::string toml_string(std::string_view value)
{
    std::string result = "\"";
    for (const char character : value) {
        if (character == '\\' || character == '"') result += '\\';
        if (character == '\r') result += "\\r";
        else
        if (character == '\n') result += "\\n";
        else result += character;
    }
    return result + '"';
}

std::pair<std::string, std::size_t> line_at(const std::string& value, std::size_t offset)
{
    const auto newline = value.find('\n', offset);
    const auto next = newline == std::string::npos ? value.size() : newline + 1;
    auto end = newline == std::string::npos ? value.size() : newline;
    if (end > offset && value[end - 1] == '\r') --end;
    return {value.substr(offset, end - offset), next};
}

bool hook_header(const std::string& value)
{
    const auto line = trim(value);
    const auto closing = line.find("]]");
    if (!line.starts_with("[[") || closing == std::string::npos) return false;
    const auto suffix = trim(line.substr(closing + 2));
    if (!suffix.empty() && !suffix.starts_with('#')) return false;
    const auto key = trim(line.substr(2, closing - 2));
    return key == "hooks" || key == "\"hooks\"" || key == "'hooks'";
}

struct Config_edit
{
    std::size_t begin;
    std::size_t end;
    std::string replacement;
};

// toml++ columns count Unicode codepoints, not UTF-8 bytes. Keep CR as a
// column and LF as a line break, matching its reader; the BOM is not a column.
class Source_offsets
{
public:
    explicit Source_offsets(const std::string& source) : m_source(source)
    {
        m_lines.push_back(source.starts_with("\xEF\xBB\xBF") ? 3 : 0);
        for (std::size_t index = 0; index < source.size(); ++index) {
            if (source[index] == '\n') m_lines.push_back(index + 1);
        }
    }

    std::size_t byte(toml::source_position position) const
    {
        if (!position || position.line > m_lines.size()) throw std::runtime_error("config_source_region_invalid");
        auto offset = m_lines[position.line - 1];
        for (std::size_t column = 1; column < position.column; ++column) {
            if (offset == m_source.size() || m_source[offset] == '\n') throw std::runtime_error("config_source_region_invalid");
            ++offset;
            while (offset < m_source.size() && (static_cast<unsigned char>(m_source[offset]) & 0xC0) == 0x80) ++offset;
        }
        return offset;
    }

private:
    const std::string& m_source;
    std::vector<std::size_t> m_lines;
};

void string_regions(const toml::node& node, const Source_offsets& offsets,
    std::vector<std::pair<std::size_t, std::size_t>>& regions)
{
    if (node.is_string()) {
        regions.emplace_back(offsets.byte(node.source().begin), offsets.byte(node.source().end));
    }
    else
    if (const auto* table = node.as_table()) {
        for (const auto& [key, value] : *table) {
            (void)key;
            string_regions(value, offsets, regions);
        }
    }
    else
    if (const auto* array = node.as_array()) {
        for (const auto& value : *array) string_regions(value, offsets, regions);
    }
}

std::string read_config(const fs::path& path)
{
    std::error_code error;
    if (!fs::exists(path, error) && !error) return {};
    if (error || fs::file_size(path, error) > 4 * 1024 * 1024 || error) throw std::runtime_error("config_read_failed");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("config_read_failed");
    std::string contents(4 * 1024 * 1024 + 1, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || contents.size() > 4 * 1024 * 1024) throw std::runtime_error("config_read_failed");
    return contents;
}

} // namespace

std::string code_home(const Environment& environment, std::string_view working_directory)
{
    auto home = trim(environment_value(environment, "KIMI_CODE_HOME"));
    if (home.empty()) {
#ifdef _WIN32
        home = environment_value(environment, "USERPROFILE");
#else
        home = environment_value(environment, "HOME");
#endif
        if (home.empty()) throw std::runtime_error("home_unavailable");
        home = path_string(addon::path_from_utf8(home) / ".kimi-code");
    }
    auto path = addon::path_from_utf8(home);
    if (!path.is_absolute()) path = addon::path_from_utf8(working_directory) / path;
    if (!path.is_absolute()) throw std::runtime_error("home_not_absolute");
    return path_string(path.lexically_normal());
}

std::string hook_entry(std::string_view event, int timeout)
{
#ifdef _WIN32
    const std::string command = "powershell.exe -NoLogo -NoProfile -NonInteractive -Command \""
        "if ($env:LOGONOMIC_PROVIDER_HELPERS_DIR) { "
        "& (Join-Path (Split-Path -LiteralPath $env:LOGONOMIC_PROVIDER_HELPERS_DIR) 'adapter/kimi_helper.exe') }\"";
#else
    const std::string command = "if [ -n \"$LOGONOMIC_PROVIDER_HELPERS_DIR\" ]; then "
        "\"${LOGONOMIC_PROVIDER_HELPERS_DIR%/*}/adapter/kimi_helper\"; fi";
#endif
    return "# Logonomic managed Kimi hook: " + std::string(event) + "\n[[hooks]]\nevent = " + toml_string(event) +
        "\ncommand = " + toml_string(command) + "\ntimeout = " + std::to_string(timeout) + "\n";
}

namespace {

std::string updated_hooks(std::string config, bool start_only)
{
    const auto document = toml::parse(config);
    const Source_offsets offsets(config);
    std::vector<std::pair<std::size_t, std::size_t>> strings;
    string_regions(document, offsets, strings);
    std::sort(strings.begin(), strings.end());
    std::map<std::size_t, const toml::table*> tables;
    if (const auto* hooks = document["hooks"].as_array()) {
        for (const auto& hook : *hooks) {
            const auto* table = hook.as_table();
            if (!table || table->is_inline()) continue;
            const auto begin = offsets.byte(table->source().begin);
            const auto previous = begin == 0 ? std::string::npos : config.rfind('\n', begin - 1);
            tables.emplace(previous == std::string::npos ? 0 : previous + 1, table);
        }
    }
    std::vector<Config_edit> edits;
    std::string appended;
    for (const auto& [event, timeout] : {
            std::pair{"SessionStart", 18}, {"SessionEnd", 13}, {"UserPromptSubmit", 23},
            {"PermissionRequest", 13}, {"PermissionResult", 13}})
    {
        if (start_only && std::string_view(event) != "SessionStart") {
            continue;
        }
        const auto marker = std::string("# Logonomic managed Kimi hook: ") + event;
        bool found = false;
        std::size_t cursor = config.starts_with("\xEF\xBB\xBF") ? 3 : 0;
        while (cursor < config.size()) {
            const auto [line, next] = line_at(config, cursor);
            if (line != marker) { cursor = next; continue; }
            const auto after = std::upper_bound(strings.begin(), strings.end(), cursor,
                [](std::size_t position, const auto& region) { return position < region.first; });
            if (after != strings.begin() && cursor < std::prev(after)->second) { cursor = next; continue; }
            auto header = next;
            while (header < config.size()) {
                const auto [candidate, following] = line_at(config, header);
                const auto stripped = trim(candidate);
                if (stripped.starts_with("# Logonomic managed Kimi hook:")) break;
                if (!stripped.empty() && !stripped.starts_with('#')) break;
                header = following;
            }
            const auto table_entry = tables.find(header);
            if (table_entry == tables.end() || !hook_header(line_at(config, header).first)) {
                cursor = next;
                continue; // An orphan comment does not authorize another table.
            }
            const auto& table = *table_entry->second;
            if (table.size() != 3 || table["event"].value<std::string>() != event ||
                !table["command"].is_string() || !table["timeout"].is_integer())
            {
                throw std::runtime_error("managed_hook_modified");
            }
            if (found) {
                // Delete owned syntax only. Inter-entry and inline comments,
                // including CRLF bytes, remain outside these source regions.
                edits.push_back({cursor, next, {}});
                const auto header_end = config.find("]]", offsets.byte(table.source().begin));
                edits.push_back({offsets.byte(table.source().begin), header_end + 2, {}});
                for (const auto& [key, value] : table) {
                    edits.push_back({offsets.byte(key.source().begin), offsets.byte(value.source().end), {}});
                }
            }
            else {
                const auto& command = *table.get("command");
                const auto& deadline = *table.get("timeout");
                const auto expected = toml::parse(hook_entry(event, timeout));
                const auto replacement = expected["hooks"][0]["command"].value<std::string>();
                edits.push_back({offsets.byte(command.source().begin), offsets.byte(command.source().end), toml_string(*replacement)});
                edits.push_back({offsets.byte(deadline.source().begin), offsets.byte(deadline.source().end), std::to_string(timeout)});
                found = true;
            }
            cursor = next;
        }
        if (!found) {
            appended += '\n' + hook_entry(event, timeout);
        }
    }
    std::sort(edits.begin(), edits.end(), [](const auto& left, const auto& right) { return left.begin > right.begin; });
    auto boundary = config.size();
    for (const auto& edit : edits) {
        if (edit.begin > edit.end || edit.end > boundary) throw std::runtime_error("config_edit_overlap");
        config.replace(edit.begin, edit.end - edit.begin, edit.replacement);
        boundary = edit.begin;
    }
    if (!appended.empty()) {
        if (!config.empty() && config.back() != '\n') config += '\n';
        config += appended;
    }
    (void)toml::parse(config);
    return config;
}

} // namespace

std::string updated_config(std::string config)
{
    return updated_hooks(std::move(config), false);
}

bool has_start_hook(const std::string& home)
{
    const auto config = read_config(addon::path_from_utf8(home) / "config.toml");
    return updated_hooks(config, true) == config;
}

} // namespace kimi
