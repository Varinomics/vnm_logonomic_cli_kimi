#include "kimi_acp.h"
#include "kimi_json.h"

#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
constexpr std::size_t k_record_limit = 256 * 1024;
constexpr std::size_t k_result_limit = 1024 * 1024;

std::string rpc(int id, const char* method, Json params)
{
    return addon::serialize_json(object({{"jsonrpc", Json("2.0")}, {"id", Json(id)},
        {"method", Json(method)}, {"params", std::move(params)}})) + '\n';
}

addon::Structured_inspect_result failure(const char* reason)
{
    addon::Structured_inspect_result result;
    result.complete       = true;
    result.failure_reason = reason;
    return result;
}

bool mode_is_auto(const Json& value)
{
    const auto& options = member(value, "configOptions");
    if (!options.is_array()) return false;
    for (const auto& option : options.as_array()) {
        if (!option.is_object()) return false;
        if (text(option, "id") == "mode" && text(option, "currentValue") == "auto") return true;
    }
    return false;
}

bool nonnegative_integer(const Json& value)
{
    return value.is_number() && value.as_number() >= 0 && value.as_number() <= 9007199254740991.0 &&
        std::floor(value.as_number()) == value.as_number();
}

bool valid_text(const Json& value)
{
    const auto& content = member(value, "content");
    return exact_keys(value, {"sessionUpdate", "content"}) &&
        exact_keys(content, {"type", "text"}) && text(content, "type") == "text" &&
        member(content, "text").is_string();
}

bool valid_commands(const Json& value)
{
    const auto& commands = member(value, "availableCommands");
    if (!exact_keys(value, {"sessionUpdate", "availableCommands"}) || !commands.is_array() ||
        commands.as_array().size() > 1024) return false;
    for (const auto& command : commands.as_array()) {
        if (!only_keys(command, {"name", "description", "input"}) ||
            !bounded_string(member(command, "name"), 512) ||
            !bounded_string(member(command, "description"), 4096) ||
            (command.find("input") && !member(command, "input").is_object())) return false;
    }
    return true;
}

bool valid_tool(const Json& value)
{
    if (!only_keys(value, {"sessionUpdate", "toolCallId", "title", "kind", "status",
            "content", "locations", "rawInput", "rawOutput"}) ||
        !bounded_string(member(value, "toolCallId"), 512)) return false;
    if (text(value, "sessionUpdate") == "tool_call" &&
        (!bounded_string(member(value, "title"), 4096) ||
         !bounded_string(member(value, "status"), 64))) return false;
    for (const auto key : {"kind", "status"}) {
        if (value.find(key) && !bounded_string(member(value, key), 64)) return false;
    }
    if (value.find("title") && !bounded_string(member(value, "title"), 4096, true)) return false;
    for (const auto key : {"content", "locations"}) {
        if (value.find(key) && !member(value, key).is_array()) return false;
    }
    return true;
}

bool valid_plan(const Json& value)
{
    const auto& entries = member(value, "entries");
    if (!exact_keys(value, {"sessionUpdate", "entries"}) || !entries.is_array() ||
        entries.as_array().empty() || entries.as_array().size() > 1024) return false;
    for (const auto& entry : entries.as_array()) {
        const auto priority = text(entry, "priority");
        const auto status   = text(entry, "status");
        if (!exact_keys(entry, {"content", "priority", "status"}) ||
            !bounded_string(member(entry, "content"), 16 * 1024) ||
            (priority != "low" && priority != "medium" && priority != "high") ||
            (status != "pending" && status != "in_progress" && status != "completed")) return false;
    }
    return true;
}

bool notification(const Json& value, std::string_view stage, std::string_view session, std::string& output)
{
    const auto& params = member(value, "params");
    if (!exact_keys(value, {"jsonrpc", "method", "params"}) || text(value, "jsonrpc") != "2.0" ||
        text(value, "method") != "session/update" || !exact_keys(params, {"sessionId", "update"}) ||
        session.empty() || text(params, "sessionId") != session) return false;
    const auto& update = member(params, "update");
    const auto type = text(update, "sessionUpdate");
    if (type == "available_commands_update") {
        return stage != "await_initialize" && stage != "await_new" && valid_commands(update);
    }
    if (type == "current_mode_update") {
        return stage == "await_mode" && exact_keys(update, {"sessionUpdate", "currentModeId"}) &&
            text(update, "currentModeId") == "auto";
    }
    if (type == "config_option_update") {
        return stage == "await_mode" && exact_keys(update, {"sessionUpdate", "configOptions"}) && mode_is_auto(update);
    }
    if (type == "agent_message_chunk" || type == "agent_thought_chunk") {
        if (stage != "await_prompt" || !valid_text(update)) return false;
        if (type == "agent_message_chunk") output += text(member(update, "content"), "text");
        return output.size() <= k_result_limit;
    }
    if (type == "tool_call" || type == "tool_call_update") return stage == "await_prompt" && valid_tool(update);
    if (type == "plan") return stage == "await_prompt" && valid_plan(update);
    if (type == "usage_update") {
        return (stage == "await_prompt" || stage == "await_close") &&
            exact_keys(update, {"sessionUpdate", "used", "size"}) &&
            nonnegative_integer(member(update, "used")) && nonnegative_integer(member(update, "size")) &&
            member(update, "size").as_number() > 0;
    }
    if (type == "session_info_update") {
        return (stage == "await_prompt" || stage == "await_close") &&
            exact_keys(update, {"sessionUpdate", "title"}) &&
            (member(update, "title").is_null() || bounded_string(member(update, "title"), 4096, true));
    }
    return false;
}

bool response(const Json& value, int expected)
{
    return exact_keys(value, {"jsonrpc", "id", "result"}) && text(value, "jsonrpc") == "2.0" &&
        member(value, "id").is_number() && member(value, "id").as_number() == expected &&
        member(value, "result").is_object();
}

} // namespace

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
        servers.push_back(object({{"name", Json("logonomic-task-" + request.control.task_id + "-" + server.server_id)},
            {"command", Json(connection.relay_program)}, {"args", std::move(arguments)}, {"env", Json::array()}}));
    }
    return rpc(1, "initialize", object({{"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()}})) +
        rpc(2, "session/new", object({{"cwd", Json(request.context.working_directory)}, {"mcpServers", std::move(servers)}}));
}

std::string initial_acp_state(std::string_view prompt)
{
    return addon::serialize_json(object({{"stage", Json("await_initialize")}, {"pending_base64", Json("")},
        {"deferred_input_base64", Json("")},
        {"prompt_base64", Json(addon::encode_base64(prompt))}, {"session_id", Json("")}, {"result_base64", Json("")}}));
}

addon::Structured_inspect_result inspect_acp(const addon::Structured_inspection& inspection)
{
    const auto parsed = parse(inspection.protocol_state);
    if (!parsed || !exact_keys(*parsed, {"stage", "pending_base64", "deferred_input_base64", "prompt_base64", "session_id", "result_base64"}) ||
        !member(*parsed, "stage").is_string() || !bounded_string(member(*parsed, "session_id"), 512, true)) {
        return failure("malformed_acp_state");
    }
    auto state = *parsed;
    for (const auto key : {"pending_base64", "deferred_input_base64", "prompt_base64", "result_base64"}) {
        if (!member(state, key).is_string()) return failure("malformed_acp_state");
    }
    auto pending = addon::decode_base64(text(state, "pending_base64"));
    auto deferred_input = addon::decode_base64(text(state, "deferred_input_base64"));
    auto prompt  = addon::decode_base64(text(state, "prompt_base64"));
    auto output  = addon::decode_base64(text(state, "result_base64"));
    if (!pending || !deferred_input || !prompt || !output || output->size() > k_result_limit) return failure("malformed_acp_state");
    *pending += inspection.stdout_bytes;
    // A reply proposed while a native record is incomplete has not been
    // written yet. Retain that causal boundary across inspection calls.
    std::string input = std::move(*deferred_input);
    for (;;) {
        const auto end = pending->find('\n');
        if (end == std::string::npos) break;
        if (end == 0 || end > k_record_limit) return failure("invalid_acp_record");
        const auto value = parse(std::string_view(*pending).substr(0, end));
        pending->erase(0, end + 1);
        if (!value || !value->is_object()) return failure("invalid_acp_record");
        const auto stage = text(state, "stage");
        if (value->find("method")) {
            if (!input.empty()) {
                const auto type = text(member(member(*value, "params"), "update"), "sessionUpdate");
                const bool housekeeping = type == "available_commands_update" ||
                    type == "current_mode_update" || type == "config_option_update";
                const bool closing_advisory = stage == "await_close" &&
                    (type == "usage_update" || type == "session_info_update");
                if (!housekeeping && !closing_advisory) {
                    return failure("out_of_order_acp_notification");
                }
            }
            if (!notification(*value, stage, text(state, "session_id"), *output)) return failure("unauthenticated_acp_notification");
            continue;
        }
        if (!input.empty()) return failure("out_of_order_acp_response");
        const auto& result = member(*value, "result");
        if (stage == "await_initialize") {
            if (!response(*value, 1) || !member(result, "protocolVersion").is_number() ||
                member(result, "protocolVersion").as_number() != 1) return failure("invalid_acp_initialize");
            state.set("stage", Json("await_new"));
        }
        else
        if (stage == "await_new") {
            if (!response(*value, 2) || !bounded_string(member(result, "sessionId"), 512)) return failure("invalid_acp_session");
            state.set("session_id", member(result, "sessionId"));
            state.set("stage", Json("await_mode"));
            input = rpc(3, "session/set_config_option", object({{"sessionId", member(state, "session_id")},
                {"configId", Json("mode")}, {"value", Json("auto")}}));
        }
        else
        if (stage == "await_mode") {
            if (!response(*value, 3) || !mode_is_auto(result) || prompt->empty() || !addon::is_valid_utf8(*prompt)) {
                return failure("invalid_acp_mode");
            }
            state.set("stage", Json("await_prompt"));
            state.set("prompt_base64", Json(""));
            auto content = Json::array();
            content.push_back(object({{"type", Json("text")}, {"text", Json(*prompt)}}));
            input = rpc(4, "session/prompt", object({{"sessionId", member(state, "session_id")}, {"prompt", std::move(content)}}));
        }
        else
        if (stage == "await_prompt") {
            if (!response(*value, 4) || text(result, "stopReason") != "end_turn" || output->empty()) {
                return failure("invalid_acp_completion");
            }
            state.set("stage", Json("await_close"));
            input = rpc(5, "session/close", object({{"sessionId", member(state, "session_id")}}));
        }
        else
        if (stage == "await_close") {
            if (!response(*value, 5) || !result.as_object().empty() || !pending->empty() ||
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
    addon::Structured_inspect_result result;
    result.protocol_state = addon::serialize_json(state);
    if (!addon::exchange_protocol_state_valid(result.protocol_state)) return failure("acp_state_limit");
    if (!defer_input && !input.empty()) result.stdin_bytes = std::move(input);
    return result;
}

} // namespace kimi
