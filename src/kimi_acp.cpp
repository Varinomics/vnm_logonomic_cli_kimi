#include "kimi_acp.h"
#include "kimi_json.h"
#include "kimi_launcher.h"
#include <logonomic_cli_addon/launcher_utilities.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
constexpr std::size_t k_record_limit = 256 * 1024;
constexpr std::size_t k_result_limit = 1024 * 1024;
constexpr std::size_t k_status_limit = 64 * 1024;
// Kimi's ACP builtin `/mcp` renders the session's MCP connection state
// without starting a model turn. It reports a snapshot, and Kimi signals no
// connection change over ACP, so a pending server is queried again.
constexpr std::string_view k_status_command = "mcp";
constexpr auto k_status_interval = std::chrono::milliseconds(100);
// Requests 1-3 initialize the session; status queries, the task and close follow.
constexpr int k_first_status_request_id = 4;
constexpr double k_max_request_id = 2147483647.0;

enum class Task_server_status { ADMITTED, PENDING, UNAVAILABLE };

std::string rpc(int id, const char* method, Json params)
{
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json(id)},
        {"method", Json(method)}, {"params", std::move(params)}})) + '\n';
}

std::string prompt_request(int id, const Json& session, std::string prompt)
{
    auto content = Json::array();
    content.push_back(json_util::object({{"type", Json("text")}, {"text", Json(std::move(prompt))}}));
    return rpc(id, "session/prompt", json_util::object({{"sessionId", session}, {"prompt", std::move(content)}}));
}

std::string task_server_name(const addon::Delegated_prepare_request& request, const addon::Delegated_task_server& server)
{
    return "logonomic-task-" + request.control.task_id + "-" + server.server_id;
}

// `/mcp` prints "- <name> (<transport>): <status>, <count> tools" for every
// configured server. Kimi counts the tools it enabled from the catalog, and
// the host relay limits that catalog to the required actions. Preparation
// refuses actions that Kimi would register under one name, so the count
// equals the action count only when every required action was registered.
// Names and error text are printed unescaped: a second line for a name is
// refused rather than trusted.
Task_server_status task_server_status(std::string_view report, const Json& servers)
{
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start <= report.size();) {
        const auto end = std::min(report.find('\n', start), report.size());
        lines.push_back(report.substr(start, end - start));
        start = end + 1;
    }
    bool pending = false;
    for (const auto& server : servers.as_array()) {
        const auto prefix = "- " + json_util::text(server, "name") + " (";
        const std::string_view* line = nullptr;
        for (const auto& candidate : lines) {
            if (!candidate.starts_with(prefix)) continue;
            if (line) return Task_server_status::UNAVAILABLE;
            line = &candidate;
        }
        const auto head = prefix + "stdio): ";
        const auto actions = static_cast<std::uint64_t>(json_util::member(server, "actions").as_number());
        if (line && *line == head + "connected, " + std::to_string(actions) + " tools") continue;
        if (line && *line == head + "pending, 0 tools") {
            pending = true;
            continue;
        }
        return Task_server_status::UNAVAILABLE;
    }
    return pending ? Task_server_status::PENDING : Task_server_status::ADMITTED;
}

addon::Structured_inspect_result failure(const char* reason)
{
    addon::Structured_inspect_result result;
    result.complete       = true;
    result.failure_reason = reason;
    return result;
}

bool mode_matches(const Json& value, std::string_view mode)
{
    const auto& options = json_util::member(value, "configOptions");
    if (!options.is_array()) return false;
    for (const auto& option : options.as_array()) {
        if (!option.is_object()) return false;
        if (json_util::text(option, "id") == "mode" && json_util::text(option, "currentValue") == mode) return true;
    }
    return false;
}

// Approve only the tools injected for this Task. Never select a persistent
// approval or an option that also changes the launcher's permission mode.
std::string permission_reply(const Json& request, const Json& state)
{
    const auto& params = json_util::member(request, "params");
    const auto& id = json_util::member(request, "id");
    const auto& options = json_util::member(params, "options");
    if (!json_util::exact_keys(request, {"jsonrpc", "id", "method", "params"}) || json_util::text(request, "jsonrpc") != "2.0" ||
        !(id.is_integer() || json_util::bounded_string(id, 512)) || json_util::text(params, "sessionId") != json_util::text(state, "session_id") ||
        !json_util::bounded_string(json_util::member(json_util::member(params, "toolCall"), "toolCallId"), 512) || !options.is_array()) return {};
    const auto name = json_util::text(json_util::member(params, "toolCall"), "title");
    const auto& tools = json_util::member(state, "task_tools").as_array();
    const bool allowed = std::any_of(tools.begin(), tools.end(), [&](const Json& tool) { return tool.as_string() == name; });
    const std::string chosen = allowed ? "approve_once" : "reject";
    int matches = 0;
    for (const auto& option : options.as_array()) {
        if (json_util::text(option, "optionId") != chosen) continue;
        if (json_util::text(option, "kind") != (allowed ? "allow_once" : "reject_once")) return {};
        ++matches;
    }
    if (matches != 1) return {};
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", id},
        {"result", json_util::object({{"outcome", json_util::object({{"outcome", Json("selected")}, {"optionId", Json(chosen)}})}})}})) + '\n';
}

bool nonnegative_integer(const Json& value)
{
    return value.is_number() && value.as_number() >= 0 && value.as_number() <= 9007199254740991.0 &&
        std::floor(value.as_number()) == value.as_number();
}

bool valid_text(const Json& value)
{
    const auto& content = json_util::member(value, "content");
    return json_util::exact_keys(value, {"sessionUpdate", "content"}) &&
        json_util::exact_keys(content, {"type", "text"}) && json_util::text(content, "type") == "text" &&
        json_util::member(content, "text").is_string();
}

bool valid_commands(const Json& value)
{
    const auto& commands = json_util::member(value, "availableCommands");
    if (!json_util::exact_keys(value, {"sessionUpdate", "availableCommands"}) || !commands.is_array() ||
        commands.as_array().size() > 1024) return false;
    for (const auto& command : commands.as_array()) {
        if (!json_util::only_keys(command, {"name", "description", "input"}) ||
            !json_util::bounded_string(json_util::member(command, "name"), 512) ||
            !json_util::bounded_string(json_util::member(command, "description"), 4096) ||
            (command.find("input") && !json_util::member(command, "input").is_object())) return false;
    }
    return true;
}

bool valid_tool(const Json& value)
{
    if (!json_util::only_keys(value, {"sessionUpdate", "toolCallId", "title", "kind", "status",
            "content", "locations", "rawInput", "rawOutput"}) ||
        !json_util::bounded_string(json_util::member(value, "toolCallId"), 512)) return false;
    if (json_util::text(value, "sessionUpdate") == "tool_call" &&
        (!json_util::bounded_string(json_util::member(value, "title"), 4096) ||
         !json_util::bounded_string(json_util::member(value, "status"), 64))) return false;
    for (const auto key : {"kind", "status"}) {
        if (value.find(key) && !json_util::bounded_string(json_util::member(value, key), 64)) return false;
    }
    if (value.find("title") && !json_util::bounded_string(json_util::member(value, "title"), 4096, true)) return false;
    for (const auto key : {"content", "locations"}) {
        if (value.find(key) && !json_util::member(value, key).is_array()) return false;
    }
    return true;
}

bool valid_plan(const Json& value)
{
    const auto& entries = json_util::member(value, "entries");
    if (!json_util::exact_keys(value, {"sessionUpdate", "entries"}) || !entries.is_array() ||
        entries.as_array().empty() || entries.as_array().size() > 1024) return false;
    for (const auto& entry : entries.as_array()) {
        const auto priority = json_util::text(entry, "priority");
        const auto status   = json_util::text(entry, "status");
        if (!json_util::exact_keys(entry, {"content", "priority", "status"}) ||
            !json_util::bounded_string(json_util::member(entry, "content"), 16 * 1024) ||
            (priority != "low" && priority != "medium" && priority != "high") ||
            (status != "pending" && status != "in_progress" && status != "completed")) return false;
    }
    return true;
}

bool notification(const Json& value, std::string_view stage, std::string_view session, bool status_outstanding,
    std::string& output, std::string& status, bool& status_command, std::string_view mode)
{
    const auto& params = json_util::member(value, "params");
    if (!json_util::exact_keys(value, {"jsonrpc", "method", "params"}) || json_util::text(value, "jsonrpc") != "2.0" ||
        json_util::text(value, "method") != "session/update" || !json_util::exact_keys(params, {"sessionId", "update"}) ||
        session.empty() || json_util::text(params, "sessionId") != session) return false;
    const auto& update = json_util::member(params, "update");
    const auto type = json_util::text(update, "sessionUpdate");
    if (type == "available_commands_update") {
        if (stage == "await_initialize" || stage == "await_new" || !valid_commands(update)) return false;
        const auto& commands = json_util::member(update, "availableCommands").as_array();
        status_command = std::any_of(commands.begin(), commands.end(),
            [](const Json& command) { return json_util::text(command, "name") == k_status_command; });
        return true;
    }
    if (type == "agent_message_chunk" && stage == "await_admission") {
        if (!status_outstanding || !valid_text(update)) return false;
        status += json_util::text(json_util::member(update, "content"), "text");
        return status.size() <= k_status_limit;
    }
    if (type == "current_mode_update") {
        return stage == "await_mode" && json_util::exact_keys(update, {"sessionUpdate", "currentModeId"}) &&
            json_util::text(update, "currentModeId") == mode;
    }
    if (type == "config_option_update") {
        return stage == "await_mode" && json_util::exact_keys(update, {"sessionUpdate", "configOptions"}) && mode_matches(update, mode);
    }
    if (type == "agent_message_chunk" || type == "agent_thought_chunk") {
        if (stage != "await_prompt" || !valid_text(update)) return false;
        if (type == "agent_message_chunk") output += json_util::text(json_util::member(update, "content"), "text");
        return output.size() <= k_result_limit;
    }
    if (type == "tool_call" || type == "tool_call_update") return stage == "await_prompt" && valid_tool(update);
    if (type == "plan") return stage == "await_prompt" && valid_plan(update);
    if (type == "usage_update") {
        return (stage == "await_prompt" || stage == "await_close") &&
            json_util::exact_keys(update, {"sessionUpdate", "used", "size"}) &&
            nonnegative_integer(json_util::member(update, "used")) && nonnegative_integer(json_util::member(update, "size")) &&
            json_util::member(update, "size").as_number() > 0;
    }
    if (type == "session_info_update") {
        return (stage == "await_prompt" || stage == "await_close") &&
            json_util::exact_keys(update, {"sessionUpdate", "title"}) &&
            (json_util::member(update, "title").is_null() || json_util::bounded_string(json_util::member(update, "title"), 4096, true));
    }
    return false;
}

bool response(const Json& value, int expected)
{
    return json_util::exact_keys(value, {"jsonrpc", "id", "result"}) && json_util::text(value, "jsonrpc") == "2.0" &&
        json_util::member(value, "id").is_number() && json_util::member(value, "id").as_number() == expected &&
        json_util::member(value, "result").is_object();
}

std::string sanitized_name_part(std::string_view part)
{
    std::string result;
    for (const char c : part) {
        const bool kept = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-';
        const char mapped = kept ? c : '_';
        if (mapped == '_' && !result.empty() && result.back() == '_') continue;
        result.push_back(mapped);
    }
    return result;
}

} // namespace

// Mirrors qualifyMcpToolName in Kimi Code's mcpCore/tool-naming.ts: every
// character outside [A-Za-z0-9_-] becomes '_', runs of '_' collapse, and a
// name over 64 characters keeps its start plus '_' and a 32-bit FNV-1a hash of
// the whole name, printed as JavaScript prints a signed integer in base 16.
std::string mcp_tool_name(std::string_view server, std::string_view tool)
{
    constexpr std::size_t k_max_length = 64;
    auto full = "mcp__" + sanitized_name_part(server) + "__" + sanitized_name_part(tool);
    if (full.size() <= k_max_length) return full;
    std::uint32_t hash = 2166136261u;
    for (const char c : full) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    const auto value = static_cast<std::int32_t>(hash);
    auto magnitude = static_cast<std::uint64_t>(value < 0 ? -static_cast<std::int64_t>(value) : value);
    std::string digits;
    do {
        digits.insert(digits.begin(), "0123456789abcdef"[magnitude % 16]);
        magnitude /= 16;
    } while (magnitude != 0);
    if (value < 0) digits.insert(digits.begin(), '-');
    if (digits.size() < 8) digits.insert(0, 8 - digits.size(), '0');
    return full.substr(0, k_max_length - digits.size() - 1) + "_" + digits;
}

std::optional<std::string> required_action_collision(const addon::Delegated_prepare_request& request)
{
    std::map<std::string, std::string> registered;
    for (const auto& server : request.control.servers) {
        for (const auto& action : server.actions) {
            const auto name = mcp_tool_name(task_server_name(request, server), action);
            const auto action_label = server.server_id + "/" + action;
            const auto [existing, inserted] = registered.emplace(name, action_label);
            if (!inserted) return existing->second + " and " + action_label;
        }
    }
    return std::nullopt;
}

std::string initial_acp_input(const addon::Delegated_prepare_request& request)
{
    auto servers = Json::array();
    for (const auto& server : request.control.servers) {
        const auto& connection = request.control.connection.value();
        auto arguments = Json::array();
        for (const auto& argument : {std::string("--descriptor"), connection.descriptor_path,
                 std::string("--server"), server.server_id})
        {
            arguments.push_back(Json(argument));
        }
        servers.push_back(json_util::object({{"name", Json(task_server_name(request, server))},
            {"command", Json(connection.relay_program)}, {"args", std::move(arguments)}, {"env", Json::array()}}));
    }
    return rpc(1, "initialize", json_util::object({{"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()}})) +
        rpc(2, "session/new", json_util::object({{"cwd", Json(request.context.working_directory)}, {"mcpServers", std::move(servers)}}));
}

std::string initial_acp_state(const addon::Delegated_prepare_request& request)
{
    auto servers = Json::array();
    auto tools = Json::array();
    // Kimi's ACP entry ignores top-level mode flags. Mirror their CLI meaning
    // only when the launcher explicitly supplies one (plan takes precedence).
    const auto args = addon::literal_argv(request.context.launcher.script_body);
    const auto has = [&](std::string_view flag) { return std::find(args.begin(), args.end(), flag) != args.end(); };
    const std::string mode = has("--plan") ? "plan" : has("--auto") ? "yolo" :
        (has("--yolo") || has("-y")) ? "auto" : "";
    for (const auto& server : request.control.servers) {
        for (const auto& action : server.actions) tools.push_back(Json(mcp_tool_name(task_server_name(request, server), action)));
        servers.push_back(json_util::object({{"name", Json(task_server_name(request, server))},
            {"actions", Json(static_cast<double>(server.actions.size()))}}));
    }
    return addon::serialize_json(json_util::object({{"stage", Json("await_initialize")}, {"pending_base64", Json("")},
        {"deferred_input_base64", Json("")},
        {"prompt_base64", Json(addon::encode_base64(request.prompt))}, {"session_id", Json("")}, {"result_base64", Json("")},
        {"task_tools", std::move(tools)}, {"launcher_mode", Json(mode)}, {"servers", std::move(servers)}, {"status_command", Json(false)}, {"request_id", Json(0)},
        {"status_base64", Json("")}}));
}

addon::Structured_inspect_result inspect_acp(const addon::Structured_inspection& inspection)
{
    const auto parsed = addon::parse_exchange_protocol_state(inspection.protocol_state);
    if (!parsed || !json_util::exact_keys(*parsed, {"stage", "pending_base64", "deferred_input_base64", "prompt_base64", "session_id",
            "result_base64", "task_tools", "launcher_mode", "servers", "status_command", "request_id", "status_base64"}) ||
        !json_util::member(*parsed, "stage").is_string() || !json_util::bounded_string(json_util::member(*parsed, "session_id"), 512, true) ||
        !json_util::member(*parsed, "servers").is_array() || !json_util::member(*parsed, "status_command").is_boolean() ||
        !nonnegative_integer(json_util::member(*parsed, "request_id")) || json_util::member(*parsed, "request_id").as_number() > k_max_request_id)
    {
        return failure("malformed_acp_state");
    }
    if (!json_util::member(*parsed, "task_tools").is_array()) return failure("malformed_acp_state");
    for (const auto& tool : json_util::member(*parsed, "task_tools").as_array()) {
        if (!json_util::bounded_string(tool, 64)) return failure("malformed_acp_state");
    }
    if (!json_util::member(*parsed, "launcher_mode").is_string()) return failure("malformed_acp_state");
    auto state = *parsed;
    for (const auto key : {"pending_base64", "deferred_input_base64", "prompt_base64", "result_base64", "status_base64"}) {
        if (!json_util::member(state, key).is_string()) return failure("malformed_acp_state");
    }
    for (const auto& server : json_util::member(state, "servers").as_array()) {
        if (!json_util::exact_keys(server, {"name", "actions"}) || !json_util::bounded_string(json_util::member(server, "name"), 512) ||
            !nonnegative_integer(json_util::member(server, "actions"))) return failure("malformed_acp_state");
    }
    auto pending = addon::decode_base64(json_util::text(state, "pending_base64"));
    auto deferred_input = addon::decode_base64(json_util::text(state, "deferred_input_base64"));
    auto prompt  = addon::decode_base64(json_util::text(state, "prompt_base64"));
    auto output  = addon::decode_base64(json_util::text(state, "result_base64"));
    auto status  = addon::decode_base64(json_util::text(state, "status_base64"));
    if (!pending || !deferred_input || !prompt || !output || !status || output->size() > k_result_limit ||
        status->size() > k_status_limit) return failure("malformed_acp_state");
    bool status_command = json_util::member(state, "status_command").as_boolean();
    auto request_id = static_cast<int>(json_util::member(state, "request_id").as_number());
    const auto status_request = [&state, &request_id]() {
        return prompt_request(request_id, json_util::member(state, "session_id"), "/" + std::string(k_status_command));
    };
    *pending += inspection.stdout_bytes;
    // A reply proposed while a native record is incomplete has not been
    // written yet. Retain that causal boundary across inspection calls.
    std::string input = std::move(*deferred_input);
    std::string replies;
    for (;;) {
        const auto end = pending->find('\n');
        if (end == std::string::npos) break;
        if (end == 0 || end > k_record_limit) return failure("invalid_acp_record");
        const auto value = json_util::parse(std::string_view(*pending).substr(0, end));
        pending->erase(0, end + 1);
        if (!value || !value->is_object()) return failure("invalid_acp_record");
        const auto stage = json_util::text(state, "stage");
        if (value->find("method")) {
            if (value->find("id")) {
                if (!input.empty() || stage != "await_prompt" || json_util::text(*value, "method") != "session/request_permission")
                    return failure("unsupported_acp_reverse_request");
                const auto reply = permission_reply(*value, state);
                if (reply.empty()) return failure("task_permission_response_unavailable");
                replies += reply;
                continue;
            }
            const auto type = json_util::text(json_util::member(json_util::member(*value, "params"), "update"), "sessionUpdate");
            if (!input.empty()) {
                const bool housekeeping = type == "available_commands_update" ||
                    type == "current_mode_update" || type == "config_option_update";
                const bool closing_advisory = stage == "await_close" &&
                    (type == "usage_update" || type == "session_info_update");
                if (!housekeeping && !closing_advisory) {
                    return failure("out_of_order_acp_notification");
                }
            }
            if (!notification(*value, stage, json_util::text(state, "session_id"), stage == "await_admission" && request_id != 0,
                    *output, *status, status_command, json_util::text(state, "launcher_mode")))
            {
                return failure("unauthenticated_acp_notification");
            }
            if (type == "available_commands_update" && (stage == "await_mode" || stage == "await_admission")) {
                // Without the builtin, "/mcp" would reach the model as a prompt.
                if (!status_command) return failure("task_server_status_unavailable");
                if (stage == "await_admission" && request_id == 0) {
                    request_id = k_first_status_request_id;
                    input = status_request();
                }
            }
            continue;
        }
        if (!input.empty()) return failure("out_of_order_acp_response");
        const auto& result = json_util::member(*value, "result");
        if (stage == "await_initialize") {
            if (!response(*value, 1) || !json_util::member(result, "protocolVersion").is_number() ||
                json_util::member(result, "protocolVersion").as_number() != 1) return failure("invalid_acp_initialize");
            state.set("stage", Json("await_new"));
        }
        else
        if (stage == "await_new") {
            if (!response(*value, 2) || !json_util::bounded_string(json_util::member(result, "sessionId"), 512)) return failure("invalid_acp_session");
            state.set("session_id", json_util::member(result, "sessionId"));
            const auto mode = json_util::text(state, "launcher_mode");
            state.set("stage", Json(mode.empty() ? "await_admission" : "await_mode"));
            if (!mode.empty()) {
                input = rpc(3, "session/set_config_option", json_util::object({{"sessionId", json_util::member(state, "session_id")},
                    {"configId", Json("mode")}, {"value", Json(mode)}}));
            }
        }
        else
        if (stage == "await_mode") {
            if (!response(*value, 3) || !mode_matches(result, json_util::text(state, "launcher_mode")) || prompt->empty() || !addon::is_valid_utf8(*prompt)) {
                return failure("invalid_acp_mode");
            }
            state.set("stage", Json("await_admission"));
            // The command list follows the session reply and may still be in flight.
            if (status_command) {
                request_id = k_first_status_request_id;
                input = status_request();
            }
        }
        else
        if (stage == "await_admission") {
            if (request_id == 0 || !response(*value, request_id) || json_util::text(result, "stopReason") != "end_turn") {
                return failure("invalid_acp_task_server_status");
            }
            const auto admission = task_server_status(*status, json_util::member(state, "servers"));
            status->clear();
            if (admission == Task_server_status::UNAVAILABLE) return failure("required_control_unavailable");
            ++request_id;
            if (admission == Task_server_status::PENDING) {
                // Inspection runs in the add-on process. Pace the native status
                // queries; Kimi's own startup timeout bounds a pending server.
                std::this_thread::sleep_for(k_status_interval);
                input = status_request();
            }
            else {
                state.set("stage", Json("await_prompt"));
                state.set("prompt_base64", Json(""));
                input = prompt_request(request_id, json_util::member(state, "session_id"), std::move(*prompt));
            }
        }
        else
        if (stage == "await_prompt") {
            if (!response(*value, request_id) || json_util::text(result, "stopReason") != "end_turn" || output->empty()) {
                return failure("invalid_acp_completion");
            }
            state.set("stage", Json("await_close"));
            ++request_id;
            input = rpc(request_id, "session/close", json_util::object({{"sessionId", json_util::member(state, "session_id")}}));
        }
        else
        if (stage == "await_close") {
            if (!response(*value, request_id) || !result.as_object().empty() || !pending->empty() ||
                (inspection.final && (inspection.timed_out || inspection.exit_code != 0))) return failure("invalid_acp_close");
            addon::Structured_inspect_result completed;
            completed.complete     = true;
            completed.result_bytes = std::move(*output);
            return completed;
        }
        else return failure("out_of_order_acp_response");
    }
    if (pending->size() > k_record_limit) return failure("acp_record_limit");
    if (inspection.final) return failure(inspection.timed_out ? "acp_timeout" : "acp_incomplete");
    // Never release a native request over an already observed, unclassified
    // output prefix. Complete housekeeping may be accepted, but a task reply
    // produced before its request must still be rejected on the next call.
    const bool defer_input = !pending->empty() && !input.empty();
    state.set("deferred_input_base64", Json(defer_input ? addon::encode_base64(input) : ""));
    state.set("pending_base64", Json(addon::encode_base64(*pending)));
    state.set("result_base64", Json(addon::encode_base64(*output)));
    state.set("status_base64", Json(addon::encode_base64(*status)));
    state.set("status_command", Json(status_command));
    state.set("request_id", Json(request_id));
    addon::Structured_inspect_result result;
    result.protocol_state = addon::serialize_json(state);
    if (!addon::exchange_protocol_state_valid(result.protocol_state)) return failure("acp_state_limit");
    if (!defer_input) replies += input;
    if (!replies.empty()) result.stdin_bytes = std::move(replies);
    return result;
}

} // namespace kimi
