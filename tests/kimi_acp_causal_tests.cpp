#include "kimi_acp.h"
#include "kimi_json.h"
#include <logonomic_cli_addon/session_contracts.h>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
namespace addon = logonomic::cli_addon;
namespace provider = kimi;
namespace {
int assertions=0, failures=0;
void require(bool ok, const char* message) {
    ++assertions;
    if (!ok && failures++ < 8) std::cerr << "FAIL: " << message << '\n';
}
addon::Structured_inspect_result inspect(std::string state, std::string bytes,
    bool final=false, int exit_code=0, bool timed_out=false) {
    addon::Structured_inspection r;
    r.protocol_state=std::move(state);r.stdout_bytes=std::move(bytes);
    r.final=final;r.exit_code=exit_code;r.timed_out=timed_out;
    auto result = provider::inspect_acp(r);
    addon::Contract_failure failure;
    try {
        require(addon::decode_structured_inspect_result(
            addon::encode_structured_inspect_result(result, 2), final, &failure, 2).has_value(),
            "inspection must round-trip through the structured-session@2 codec");
    }
    catch (const std::exception& error) {
        require(false, error.what());
    }
    return result;
}
addon::Delegated_prepare_request task_request() {
    addon::Delegated_prepare_request request;
    request.prompt="TASK PAYLOAD";
    request.control.task_id="t";
    request.control.servers.push_back({"control","/opt/logonomic/logonomic_coordination_mcp",{"task_status"},{}});
    return request;
}
std::string reply(int id, std::string_view result) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"result":)" + std::string(result) + "}\n";
}
std::string chunk(std::string_view text) {
    return R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":")" +
        std::string(text) + "\"}}}}\n";
}
std::string status(std::string_view report, int id) { return chunk(report) + reply(id, R"({"stopReason":"end_turn"})"); }
bool carries(const addon::Structured_inspect_result& result, std::string_view text) {
    return result.stdin_bytes && result.stdin_bytes->find(text) != std::string::npos;
}
const std::string initialize=R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1}})" "\n";
const std::string new_session=R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"s"}})" "\n";
const std::string commands=R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"available_commands_update","availableCommands":[{"name":"status","description":"Show current session status"},{"name":"mcp","description":"Show MCP server status"}]}}})" "\n";
const std::string commands_without_status=R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"available_commands_update","availableCommands":[{"name":"status","description":"Show current session status"}]}}})" "\n";
const std::string connected=R"(MCP servers (2):\n- user-notes (stdio): connected, 4 tools\n- logonomic-task-t-control (stdio): connected, 1 tools)";
const std::string pending=R"(MCP servers (1):\n- logonomic-task-t-control (stdio): pending, 0 tools)";
const std::string completion=reply(5, R"({"stopReason":"end_turn"})");
const std::string close_reply=reply(6, "{}");
const std::string model_text=chunk("ANSWER");
}

void launcher_permissions()
{
    using Json = provider::Json;
    addon::Delegated_prepare_request request;
    request.prompt = "Task";
    request.control.task_id = "2427cd8d-5ee6-4618-bbb3-cd5c8e68a682";
    request.control.private_artifact_root = "C:/task";
    request.control.servers.push_back({"delegation", "relay", {"submit"}, {}});
    auto state = *provider::parse(provider::initial_acp_state(request));
    state.set("stage", Json("await_prompt"));
    state.set("session_id", Json("s"));
    state.set("prompt_base64", Json(""));
    const auto pending = addon::serialize_json(state);
    const auto permission = [](std::string name, std::string session = "s", bool once = true) {
        auto call = provider::object({{"toolCallId", Json("call-1")}});
        call.set("title", Json(name));
        auto options = Json::array();
        options.push_back(provider::object({{"optionId", Json("always")}, {"kind", Json("allow_always")}}));
        options.push_back(provider::object({{"optionId", Json("proceed_once_and_switch_to_default")},
            {"kind", Json("allow_once")}}));
        if (once) options.push_back(provider::object({{"optionId", Json("approve_once")}, {"kind", Json("allow_once")}}));
        options.push_back(provider::object({{"optionId", Json("reject")}, {"kind", Json("reject_once")}}));
        return addon::serialize_json(provider::object({{"jsonrpc", Json("2.0")}, {"id", Json(17)},
            {"method", Json("session/request_permission")}, {"params", provider::object({
                {"sessionId", Json(session)}, {"toolCall", std::move(call)}, {"options", std::move(options)}})}})) + '\n';
    };
    const std::string own = "mcp__logonomic-task-2427cd8d-5ee6-4618-bbb3-cd5c8e68a68_78e47f18";
    for (const auto& name : {own, std::string("run_shell_command"), own + "_other",
            std::string("mcp__user_server__submit")}) {
        const auto result = inspect(pending, permission(name) + "{");
        const auto expected = name == own ? "approve_once" : "reject";
        require(!result.failure_reason && result.stdin_bytes &&
            result.stdin_bytes->find(std::string("\"optionId\":\"") + expected + "\"") != std::string::npos,
            "permission replies immediately: own Task tool once, everything else rejected once");
        const auto next = inspect(result.protocol_state, "");
        require(!next.stdin_bytes, "a permission reply is not replayed across a partial later record");
    }
    require(inspect(pending, permission(own, "foreign")).failure_reason.has_value(),
        "a foreign session cannot obtain an approval");
    require(inspect(pending, permission(own, "s", false)).failure_reason.has_value(),
        "missing plain allow-once refuses instead of selecting a persistent or mode-changing approval");
}

int main() {
    launcher_permissions();
    for (const auto& [flag, mode] : {std::pair{"--yolo", "auto"}, {"-y", "auto"},
            {"--auto", "yolo"}, {"--plan", "plan"}}) {
        auto request = task_request();
        request.context.launcher.script_body = std::string("kimi ") + flag;
        const auto selected = inspect(provider::initial_acp_state(request), initialize + new_session);
        require(!selected.failure_reason && carries(selected, "session/set_config_option") &&
            carries(selected, std::string("\"value\":\"") + mode + "\""),
            "only an explicit launcher flag selects its matching Kimi ACP permission mode");
        const auto acknowledged = inspect(selected.protocol_state, commands + reply(3,
            std::string("{\"configOptions\":[{\"id\":\"mode\",\"currentValue\":\"") + mode + "\"}]}"));
        require(!acknowledged.failure_reason && carries(acknowledged, "/mcp"),
            "matching launcher mode acknowledgment proceeds to Task-server admission");
    }
    const auto setup=inspect(provider::initial_acp_state(task_request()),initialize+new_session);
    require(!setup.stdin_bytes && !setup.failure_reason, "session keeps launcher permissions while awaiting the command catalog");
    // A whole frame and every strict prefix have the same causal meaning.
    for (const auto& forbidden : {model_text, completion}) {
        const auto whole=inspect(setup.protocol_state,commands+forbidden);
        require(whole.failure_reason.has_value() && !whole.stdin_bytes,"whole pre-request frame is rejected before release");
        for (std::size_t split=1;split<forbidden.size();++split) {
            const auto first=inspect(setup.protocol_state,commands+forbidden.substr(0,split));
            require(!first.complete&&!first.failure_reason&&!first.stdin_bytes,"unclassified pre-request prefix holds the status query");
            const auto empty=inspect(first.protocol_state,"");
            require(!empty.complete&&!empty.failure_reason&&!empty.stdin_bytes,"empty inspection cannot release a deferred query");
            const auto second=inspect(empty.protocol_state,forbidden.substr(split));
            require(second.failure_reason.has_value()&&!second.stdin_bytes,"forbidden tail retains pre-request provenance");
        }
    }
    // Kimi advertises its commands after the session reply; either order holds.
    const auto late_commands=inspect(setup.protocol_state,commands);
    require(!late_commands.failure_reason&&carries(late_commands,R"("text":"/mcp")")&&!carries(late_commands,"TASK PAYLOAD"),
        "the advertised commands release the status query, not the task");
    const auto waiting=inspect(setup.protocol_state,"");
    require(!waiting.complete&&!waiting.failure_reason&&!waiting.stdin_bytes,"no query is sent before /mcp is advertised");
    for (std::size_t split=1;split<commands.size();++split) {
        const auto first=inspect(waiting.protocol_state,commands.substr(0,split));
        require(!first.complete&&!first.failure_reason&&!first.stdin_bytes,"partial housekeeping holds the query");
        const auto second=inspect(first.protocol_state,commands.substr(split));
        require(!second.failure_reason&&carries(second,"/mcp"),"classified housekeeping releases the query exactly once");
        const auto third=inspect(second.protocol_state,"");
        require(!third.complete&&!third.failure_reason&&!third.stdin_bytes,"the query is not replayed");
    }
    {
        const auto& bytes = commands_without_status;
        const auto refused=inspect(setup.protocol_state,bytes);
        require(refused.failure_reason=="task_server_status_unavailable"&&!refused.stdin_bytes,
            "without the builtin status command nothing reaches the model");
    }
    const auto early_report=inspect(setup.protocol_state,chunk(connected));
    require(early_report.failure_reason.has_value()&&!early_report.stdin_bytes,"a report before its query is not evidence");

    const auto querying=inspect(setup.protocol_state,commands);
    require(!querying.failure_reason&&carries(querying,R"("id":4)")&&carries(querying,"/mcp")&&!carries(querying,"TASK PAYLOAD"),
        "the task waits for the status query");
    const auto still_pending=inspect(querying.protocol_state,status(pending,4));
    require(!still_pending.failure_reason&&carries(still_pending,R"("id":5)")&&carries(still_pending,"/mcp")&&
        !carries(still_pending,"TASK PAYLOAD"),"a pending task server is queried again");
    const auto after_pending=inspect(still_pending.protocol_state,status(connected,5));
    require(!after_pending.failure_reason&&carries(after_pending,R"("id":6)")&&carries(after_pending,"TASK PAYLOAD"),
        "the task follows the report that admits every server");
    for (const auto* report : {
            R"(MCP servers (1):\n- logonomic-task-t-control (stdio): failed, 0 tools)",
            R"(MCP servers (1):\n- logonomic-task-t-control (stdio): connected, 0 tools)",
            R"(MCP servers (1):\n- logonomic-task-t-control (stdio): connected, 2 tools)",
            R"(MCP servers (1):\n- logonomic-task-t-control (http): connected, 1 tools)",
            R"(MCP servers (1):\n- logonomic-task-t-control (stdio): connected, 1 tools and more)",
            R"(MCP servers (1):\n- user-notes (stdio): connected, 1 tools)",
            R"(No MCP servers configured for this session.)",
            R"(MCP servers (2):\n- x (stdio): failed, 0 tools\n- logonomic-task-t-control (stdio): connected, 1 tools\n- logonomic-task-t-control (stdio): pending, 0 tools)"})
    {
        const auto refused=inspect(querying.protocol_state,status(report,4));
        require(refused.failure_reason=="required_control_unavailable"&&!refused.stdin_bytes,
            "only one exact connected line with every required action admits a server");
    }
    const auto cancelled=inspect(querying.protocol_state,chunk(connected)+reply(4,R"({"stopReason":"cancelled"})"));
    require(cancelled.failure_reason=="invalid_acp_task_server_status","an unfinished status query admits nothing");
    const auto report=status(connected,4);
    for (std::size_t split=1;split<report.size();++split) {
        const auto first=inspect(querying.protocol_state,report.substr(0,split));
        require(!first.complete&&!first.failure_reason&&!carries(first,"TASK PAYLOAD"),"a partial report holds the task");
        const auto second=inspect(first.protocol_state,report.substr(split));
        require(!second.failure_reason&&carries(second,"TASK PAYLOAD"),"a fragmented report admits the task once");
    }

    const auto prompt=inspect(querying.protocol_state,report);
    require(!prompt.failure_reason&&carries(prompt,R"("id":5)")&&carries(prompt,"TASK PAYLOAD"),"normal prompt is released");
    auto wrong_session=model_text; wrong_session.replace(wrong_session.find("\"s\""),3,"\"other\"");
    require(inspect(prompt.protocol_state,wrong_session).failure_reason.has_value(),"foreign session cannot supply result");
    const std::string reverse=R"({"jsonrpc":"2.0","id":20,"method":"session/request_permission","params":{"sessionId":"s"}})" "\n";
    require(inspect(prompt.protocol_state,reverse).failure_reason.has_value(),"unsupported reverse request is not a notification");
    require(inspect(prompt.protocol_state,std::string(256*1024+1,'x')).failure_reason.has_value(),"unterminated record is bounded");
    require(inspect(prompt.protocol_state,"",true,0,true).failure_reason.has_value(),"timeout cannot complete a task");
    for (std::size_t split=1;split<model_text.size();++split) {
        const auto first=inspect(prompt.protocol_state,model_text.substr(0,split));
        const auto second=inspect(first.protocol_state,model_text.substr(split)+completion);
        require(!first.failure_reason&&!second.failure_reason,"ordinary fragmented post-request answer stays valid");
        require(second.stdin_bytes.has_value()&&!second.complete,"Kimi requests native session close");
        const auto completed = inspect(second.protocol_state, close_reply);
        require(completed.complete && !completed.failure_reason && completed.result_bytes == "ANSWER" &&
            completed.protocol_state.empty() && !completed.stdin_bytes,
            "authenticated close returns the exact answer and releases continuation");
    }
    const auto closing = inspect(prompt.protocol_state, model_text + completion);
    for (std::size_t split = 1; split < close_reply.size(); ++split) {
        const auto first = inspect(closing.protocol_state, close_reply.substr(0, split));
        require(!first.complete && !first.failure_reason && !first.stdin_bytes && !first.result_bytes,
            "fragmented close reply stays pending until its record is complete");
        const auto second = inspect(first.protocol_state, close_reply.substr(split));
        require(second.complete && !second.failure_reason && second.result_bytes == "ANSWER" &&
            second.protocol_state.empty() && !second.stdin_bytes,
            "every close split completes on the authenticated reply");
        const auto final_close = inspect(first.protocol_state, close_reply.substr(split), true);
        require(final_close.complete && !final_close.failure_reason && final_close.result_bytes == "ANSWER",
            "a close reply received with successful final process facts also completes");

        const auto early = inspect(prompt.protocol_state, model_text + completion + close_reply.substr(0, split));
        require(!early.complete && !early.failure_reason && !early.stdin_bytes && !early.result_bytes,
            "unclassified pre-request close prefix holds the close request");
        const auto late = inspect(early.protocol_state, close_reply.substr(split));
        require(late.complete && late.failure_reason && !late.stdin_bytes && !late.result_bytes,
            "fragmented close cannot authenticate a request that was never released");
    }
    const auto failed_exit = inspect(closing.protocol_state, close_reply, true, 7);
    const auto timed_out   = inspect(closing.protocol_state, close_reply, true, 0, true);
    const auto missing    = inspect(closing.protocol_state, "", true);
    const auto trailing   = inspect(closing.protocol_state, close_reply + "{");
    for (const auto* failed : {&failed_exit, &timed_out, &missing, &trailing}) {
        require(failed->complete && failed->failure_reason && !failed->result_bytes && !failed->stdin_bytes,
            "invalid close or unsuccessful final process facts cannot return an answer");
    }
    std::cout<<"kimi_causal_assertions="<<assertions<<" failures="<<failures<<'\n';
    return failures ? 1 : 0;
}
