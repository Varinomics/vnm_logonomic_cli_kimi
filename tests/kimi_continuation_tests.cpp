#include "kimi_acp.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

addon::Structured_inspect_result inspect(std::string state, std::string bytes)
{
    addon::Structured_inspection request;
    request.protocol_state = std::move(state);
    request.stdout_bytes   = std::move(bytes);
    return kimi::inspect_acp(request);
}

std::string reply(int id, std::string_view result)
{
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"result":)" +
        std::string(result) + "}\n";
}

std::string chunk(std::string_view text)
{
    return R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":)" +
        addon::serialize_json(addon::Json_value(std::string(text))) + "}}}}\n";
}

std::string prompt_state()
{
    addon::Delegated_prepare_request request;
    request.prompt = "Task";
    request.control.task_id = "t";
    request.control.servers.push_back({"control", "/opt/logonomic/logonomic_coordination_mcp",
        {"task_status"}, {}});
    const auto session = inspect(kimi::initial_acp_state(request),
        reply(1, R"({"protocolVersion":1})") + reply(2, R"({"sessionId":"s"})"));
    require(!session.failure_reason && !session.stdin_bytes, "session setup must await the command catalog");
    const auto querying = inspect(session.protocol_state,
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"available_commands_update","availableCommands":[{"name":"status","description":"Show current session status"},{"name":"mcp","description":"Show MCP server status"}]}}})" "\n");
    require(!querying.failure_reason && querying.stdin_bytes &&
        querying.stdin_bytes->find("/mcp") != std::string::npos, "the command catalog must release the server query");
    const auto prompt = inspect(querying.protocol_state,
        chunk("MCP servers (1):\n- logonomic-task-t-control (stdio): connected, 1 tools") +
        reply(4, R"({"stopReason":"end_turn"})"));
    require(!prompt.failure_reason && prompt.stdin_bytes &&
        prompt.stdin_bytes->find("Task") != std::string::npos, "the matching server report must release the task");
    return prompt.protocol_state;
}

void accumulated_result_survives_inspections()
{
    const std::string answer(800 * 1024, 'x');
    // Each native record remains below Kimi's 256 KiB record limit.
    const auto notification = chunk(std::string(100 * 1024, 'x'));
    for (const int batch_size : {8, 4, 1}) {
        auto state = prompt_state();
        for (int received = 0; received < 8; received += batch_size) {
            std::string bytes;
            for (int index = 0; index < batch_size; ++index) {
                bytes += notification;
            }
            const auto first = inspect(std::move(state), std::move(bytes));
            require(!first.complete && !first.failure_reason &&
                addon::exchange_protocol_state_valid(first.protocol_state), "model chunks must yield an accepted continuation");
            const auto idle = inspect(first.protocol_state, "");
            require(!idle.complete && !idle.failure_reason && !idle.result_bytes,
                "accepted accumulated text must survive an empty inspection");
            state = idle.protocol_state;
        }
        const auto closing = inspect(std::move(state), reply(5, R"({"stopReason":"end_turn"})"));
        require(!closing.complete && !closing.failure_reason && closing.stdin_bytes &&
            closing.stdin_bytes->find("session/close") != std::string::npos,
            "prompt completion must request native session close");
        const auto settled = inspect(closing.protocol_state, reply(6, "{}"));
        require(settled.complete && !settled.failure_reason && settled.result_bytes == answer &&
            settled.protocol_state.empty(), "authenticated close must return the exact large answer");
    }
}

} // namespace

int main()
{
    try {
        accumulated_result_survives_inspections();
        std::cout << "kimi_continuation_tests batches=3 failures=0\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "kimi_continuation_tests: " << error.what() << '\n';
        return 1;
    }
}
