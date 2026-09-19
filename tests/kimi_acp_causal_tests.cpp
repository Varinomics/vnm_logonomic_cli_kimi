#include "kimi_acp.h"
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
const std::string initialize=R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1}})" "\n";
const std::string new_session=R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"s"}})" "\n";
const std::string mode=R"({"jsonrpc":"2.0","id":3,"result":{"configOptions":[{"id":"mode","currentValue":"auto"}]}})" "\n";
const std::string completion=R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"end_turn"}})" "\n";
const std::string close_reply = R"({"jsonrpc":"2.0","id":5,"result":{}})" "\n";
const std::string model_text=R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"ANSWER"}}}})" "\n";
const std::string commands=R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"available_commands_update","availableCommands":[]}}})" "\n";
}
int main() {
    const auto setup=inspect(provider::initial_acp_state("TASK PAYLOAD"),initialize+new_session);
    require(setup.stdin_bytes.has_value() && !setup.failure_reason, "mode setup succeeds");
    // A whole frame and every strict prefix have the same causal meaning.
    for (const auto& forbidden : {model_text, completion}) {
        const auto whole=inspect(setup.protocol_state,mode+forbidden);
        require(whole.failure_reason.has_value() && !whole.stdin_bytes,"whole pre-request frame is rejected before task release");
        for (std::size_t split=1;split<forbidden.size();++split) {
            const auto first=inspect(setup.protocol_state,mode+forbidden.substr(0,split));
            require(!first.complete&&!first.failure_reason&&!first.stdin_bytes,"unclassified pre-request prefix holds task");
            const auto empty=inspect(first.protocol_state,"");
            require(!empty.complete&&!empty.failure_reason&&!empty.stdin_bytes,"empty inspection cannot release deferred task");
            const auto second=inspect(empty.protocol_state,forbidden.substr(split));
            require(second.failure_reason.has_value()&&!second.stdin_bytes,"forbidden tail retains pre-request provenance");
        }
    }
    const auto full_housekeeping=inspect(setup.protocol_state,mode+commands);
    require(!full_housekeeping.failure_reason&&full_housekeeping.stdin_bytes.has_value(),"complete housekeeping permits prompt");
    for (std::size_t split=1;split<commands.size();++split) {
        const auto first=inspect(setup.protocol_state,mode+commands.substr(0,split));
        require(!first.complete&&!first.failure_reason&&!first.stdin_bytes,"partial housekeeping holds task");
        const auto second=inspect(first.protocol_state,commands.substr(split));
        require(!second.failure_reason&&second.stdin_bytes.has_value(),"classified housekeeping releases task exactly once");
        const auto third=inspect(second.protocol_state,"");
        require(!third.complete&&!third.failure_reason&&!third.stdin_bytes,"deferred task is not replayed");
    }
    const auto prompt=inspect(setup.protocol_state,mode);
    require(!prompt.failure_reason&&prompt.stdin_bytes.has_value(),"normal prompt is released");
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
