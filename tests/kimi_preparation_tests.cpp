#include "kimi_preparation.h"
#include "kimi_acp.h"
#include "kimi_accounts.h"
#include "kimi_config.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

namespace addon = logonomic::cli_addon;

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value result) override { m_result = std::move(result); }
    void fail(addon::Result_status status, std::string reason, std::string) override { m_failure_status = status; throw std::runtime_error(reason); }
    bool finished() const override { return m_result.has_value(); }

    addon::Invoke_request m_request;
    std::optional<addon::Json_value> m_result;
    std::optional<addon::Result_status> m_failure_status;
    bool m_cancelled = false;
};

void require(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
}

addon::Launch_environment native_environment(const std::string& home)
{
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    environment.effective_environment.set("KIMI_CODE_HOME", addon::Json_value(home));
    for (const auto* name : {"SystemRoot", "PATH"}) {
        if (const auto* value = std::getenv(name)) {
            environment.effective_environment.set(name, addon::Json_value(value));
        }
    }
    return environment;
}

void required_control_reaches_native_admission(const char* executable)
{
    // Regression: preparation refused every request that named required
    // actions, and the host always names them. The real preparation owner now
    // proposes the native ACP process; its inspection admits the actions.
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("kimi-delegated-" + addon::random_nonce_hex());
    fs::create_directories(scratch / "selected");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{scratch};
    const auto root = fs::canonical(scratch);
    const auto home = addon::path_to_utf8(root / "selected");
#ifdef _WIN32
    const auto fake = root / "kimi.exe";
#else
    const auto fake = root / "kimi";
#endif
    fs::copy_file(fs::absolute(executable), fake);
    addon::Delegated_prepare_request request;
    request.context.launcher.runner_key  = addon::k_runner_direct;
    request.context.launcher.script_body = '"' + addon::path_to_utf8(fake) + '"';
    request.context.launcher_argv        = {addon::path_to_utf8(fake)};
    request.context.working_directory    = addon::path_to_utf8(root);
    request.context.account_context = addon::Account_context{2, kimi::account_key(root / "selected"), home};
    request.context.launch_environment = native_environment(home);
    request.context.launch_environment->effective_environment.set("KIMI_TEST_VERSION", addon::Json_value("2.0.2"));
    request.prompt = "Submit the review task to the delegation target.";
    request.control.task_id = "task-7f3a";
    request.control.private_artifact_root = addon::path_to_utf8(root);
    request.control.connection = addon::Delegated_connection{
        addon::path_to_utf8(root / "host relay"),
        addon::path_to_utf8(root / ".logonomic/task-servers.json")};
    const auto server_program = addon::path_to_utf8(root / "logonomic_coordination_mcp");
    request.control.servers.push_back({"coordination", server_program, {"status", "notice"},
        {{"VNM_CONTROL_TOKEN", "kimi-host-authority-canary"}}});
    request.control.servers.push_back({"delegation", server_program, {"targets", "submit"}, {}});
    Context context;
    context.m_request = {"delegated", {"delegated-session", 2}, "prepare", addon::encode_delegated_prepare_request(request, 2)};
    kimi::handle_preparation(context);
    addon::Contract_failure failure;
    const auto result = context.m_result ? addon::decode_delegated_prepare_result(*context.m_result, &failure, 2) : std::nullopt;
    require(result && result->acknowledgment.connection_version == addon::k_delegated_connection_version &&
        result->acknowledgment.server_ids == std::vector<std::string>{"coordination", "delegation"},
        "required actions reach a relay-acknowledged native proposal");
    require(result->process.argv == std::vector<std::string>{addon::path_to_utf8(fake), "acp"} &&
        result->inspection.incremental && result->inspection.keep_stdin_open,
        "the proposal runs the verified launcher's native ACP server");
    const auto& input = result->process.stdin_payload;
    const auto session = addon::parse_json(input.substr(input.find('\n') + 1), {}, nullptr);
    require(session.has_value(), "native session request remains JSON");
    const auto& server = session->find("params")->find("mcpServers")->as_array().front();
    const auto& args = server.find("args")->as_array();
    require(server.find("name")->as_string() == "logonomic-task-task-7f3a-coordination" &&
        server.find("command")->as_string() == request.control.connection->relay_program &&
        server.find("env")->as_array().empty() && args.size() == 4 &&
        args[0].as_string() == "--descriptor" && args[1].as_string() == request.control.connection->descriptor_path &&
        args[2].as_string() == "--server" && args[3].as_string() == "coordination",
        "ACP uses exact nonsecret relay arguments");
    require(input.find("kimi-host-authority-canary") == std::string::npos && input.find(request.prompt) == std::string::npos,
        "neither host credentials nor the task enter ACP stdin before admission");

    // The prepared state requires both task servers with all of their actions.
    const std::string session_setup =
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1}})" "\n"
        R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"s"}})" "\n";
    const std::string mode_setup =
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"available_commands_update","availableCommands":[{"name":"mcp","description":"Show MCP server status"}]}}})" "\n"
        R"({"jsonrpc":"2.0","id":3,"result":{"configOptions":[{"id":"mode","currentValue":"auto"}]}})" "\n";
    const auto status = [](std::string report) {
        auto chunk = addon::Json_value::object();
        chunk.set("type", addon::Json_value("text"));
        chunk.set("text", addon::Json_value(std::move(report)));
        return R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"agent_message_chunk","content":)" +
            addon::serialize_json(chunk) + "}}}\n" + R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"end_turn"}})" "\n";
    };
    const auto inspect = [](std::string state, std::string bytes) {
        addon::Structured_inspection inspection;
        inspection.protocol_state = std::move(state);
        inspection.stdout_bytes   = std::move(bytes);
        return kimi::inspect_acp(inspection);
    };
    const auto querying = inspect(inspect(result->inspection.protocol_state, session_setup).protocol_state, mode_setup);
    require(!querying.failure_reason && querying.stdin_bytes && querying.stdin_bytes->find(request.prompt) == std::string::npos,
        "the prepared session queries its task servers before the task");
    const auto one_missing = inspect(querying.protocol_state, status("MCP servers (2):\n"
        "- logonomic-task-task-7f3a-coordination (stdio): connected, 2 tools\n"
        "- logonomic-task-task-7f3a-delegation (stdio): connected, 1 tools"));
    require(one_missing.failure_reason == "required_control_unavailable",
        "a task server without every required action is not admitted");
    const auto admitted = inspect(querying.protocol_state, status("MCP servers (2):\n"
        "- logonomic-task-task-7f3a-coordination (stdio): connected, 2 tools\n"
        "- logonomic-task-task-7f3a-delegation (stdio): connected, 2 tools"));
    require(!admitted.failure_reason && admitted.stdin_bytes &&
        admitted.stdin_bytes->find(request.prompt) != std::string::npos,
        "the task is released once every task server holds its required actions");

    Context cancelled;
    cancelled.m_request = context.m_request;
    cancelled.m_cancelled = true;
    bool cancellation_preserved = false;
    try { kimi::handle_preparation(cancelled); }
    catch (const std::runtime_error& error) {
        cancellation_preserved = std::string(error.what()) == "cancelled";
    }
    require(cancellation_preserved && cancelled.m_failure_status == addon::Result_status::CANCELLED && !cancelled.m_result,
        "a cancelled preparation reports cancellation and proposes no process");

    // Kimi registers only the first of two tools with one dispatcher name, so
    // its tool count would still match; such actions are refused up front.
    request.control.servers.back().actions = {"status.a", "status_a"};
    Context colliding;
    colliding.m_request = {"delegated", {"delegated-session", 2}, "prepare", addon::encode_delegated_prepare_request(request, 2)};
    bool collision_refused = false;
    try { kimi::handle_preparation(colliding); }
    catch (const std::runtime_error& error) {
        collision_refused = std::string(error.what()) == "required_control_name_collision";
    }
    require(collision_refused && colliding.m_failure_status == addon::Result_status::UNAVAILABLE && !colliding.m_result,
        "required actions that share a Kimi tool name are refused before a process is proposed");
}

void kimi_tool_names()
{
    // Oracle: values computed with Kimi Code 2.0.2's qualifyMcpToolName
    // (mcpCore/tool-naming.ts), including negative hashes printed with '-'.
    const std::string task = "logonomic-task-0123456789abcdef0123456789abcdef-coordination";
    require(kimi::mcp_tool_name("logonomic-task-t-control", "task_status") == "mcp__logonomic-task-t-control__task_status" &&
        kimi::mcp_tool_name(task, "status") == "mcp__logonomic-task-0123456789abcdef0123456789abcdef-co_-ff22951" &&
        kimi::mcp_tool_name(task, "notice") == "mcp__logonomic-task-0123456789abcdef0123456789abcdef-c_-2f62928f" &&
        kimi::mcp_tool_name("logonomic-task-9f8e7d6c5b4a39281706f5e4d3c2b1a0-delegation", "submit") ==
            "mcp__logonomic-task-9f8e7d6c5b4a39281706f5e4d3c2b1a0-de_495ef1f0",
        "Kimi tool names are mirrored exactly, including shortened names");
    require(kimi::mcp_tool_name("task server", "a.b") == kimi::mcp_tool_name("task.server", "a__b"),
        "sanitized parts collapse separator runs as Kimi does");
}

void launch_disposition()
{
    // Oracle: the launch-preparation@3 decoder requires explicit feature
    // admission. Exercise the actual provider producer without running a CLI.
    addon::Prepare_request request;
    request.working_directory = addon::path_to_utf8(std::filesystem::current_path());
    request.launcher = {"kimi", "bash", ""};
    request.launcher_argv = {"bash", "-c", "kimi"};
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    const auto home = addon::path_to_utf8(std::filesystem::current_path() / ".kimi-profile");
    environment.effective_environment.set("KIMI_CODE_HOME", addon::Json_value(home));
    request.launch_environment = environment;
    request.account_context = addon::Account_context{2, kimi::account_key(addon::path_from_utf8(home)), home};
    for (const bool prepared : {true, false}) {
        if (!prepared) {
            request.launcher.script_body = "echo unrelated";
            request.launcher_argv = {"bash", "-c", "echo unrelated"};
        }
        Context context;
        context.m_request.capability = {"launch-preparation", 3};
        context.m_request.operation = "prepare";
        context.m_request.payload = addon::encode_prepare_request(request, 3);
        kimi::handle_preparation(context);
        require(context.m_result.has_value(), "provider must return a terminal preparation");
        addon::Contract_failure failure;
        const auto patch = addon::decode_launch_patch(*context.m_result, &failure, 3);
        require(patch && patch->disposition && patch->disposition->prepared == prepared &&
            patch->disposition->lifecycle_events == prepared &&
            patch->disposition->coordination_delivery == prepared &&
            patch->apply_managed_configuration == prepared,
            "actual provider success must round-trip explicit admission and managed intent");
        require(patch->account_context && patch->account_context->adapter_account_key == request.account_context->adapter_account_key &&
            patch->account_context->profile_path == request.account_context->profile_path, "preparation retains the admitted account");
        require(addon::validate_launch_patch(*patch, {}, {}, &failure), "returned patch must satisfy host policy");
        if (!prepared) require(patch->set.empty() && patch->unset.empty() && !patch->argv,
            "unmanaged preparation must preserve the native launch");
    }
    request.launcher = {"kimi", "bash", ""};
    request.launcher_argv = {"bash", "-c", "kimi"};
    const auto sibling = home + "-work";
    request.account_context = addon::Account_context{2, kimi::account_key(addon::path_from_utf8(sibling)), sibling};
    Context mismatched;
    mismatched.m_request.capability = {"launch-preparation", 3};
    mismatched.m_request.operation = "prepare";
    mismatched.m_request.payload = addon::encode_prepare_request(request, 3);
    bool refused = false;
    try { kimi::handle_preparation(mismatched); }
    catch (const std::runtime_error& error) { refused = std::string(error.what()) == "account_context_mismatch"; }
    require(refused && !mismatched.m_result, "selected B against frozen A must fail before requesting a transform");
    request.launch_environment->effective_environment.set("KIMI_CODE_HOME", addon::Json_value(sibling));
    Context bound;
    bound.m_request = mismatched.m_request;
    bound.m_request.payload = addon::encode_prepare_request(request, 3);
    kimi::handle_preparation(bound);
    addon::Contract_failure failure;
    const auto patch = bound.m_result ? addon::decode_launch_patch(*bound.m_result, &failure, 3) : std::nullopt;
    require(patch && patch->apply_managed_configuration && patch->set.empty(),
        "host-bound B prepares hooks without a package-selected environment target");
    // The original shell assignment would override the host's parent B.
    // This provider does not reconstruct such scripts; the host must refuse
    // a selected-profile switch when it receives this unmanaged disposition.
    request.launcher.script_body = "KIMI_CODE_HOME=/profiles/account-a kimi";
    request.launcher_argv = {"bash", "-c", request.launcher.script_body};
    Context wrapper;
    wrapper.m_request = bound.m_request;
    wrapper.m_request.payload = addon::encode_prepare_request(request, 3);
    kimi::handle_preparation(wrapper);
    const auto unmanaged = wrapper.m_result ? addon::decode_launch_patch(*wrapper.m_result, &failure, 3) : std::nullopt;
    require(unmanaged && unmanaged->disposition && !unmanaged->disposition->prepared &&
        !unmanaged->apply_managed_configuration && !unmanaged->argv && unmanaged->set.empty(),
        "an inline profile assignment remains unchanged and cannot claim an integrated account switch");
}

void resume_policy(const char* executable, const char* help, bool admitted = true)
{
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("kimi-resume-" + addon::random_nonce_hex());
    fs::create_directories(scratch / "selected");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{scratch};
    const auto root = fs::canonical(scratch);
    const auto home = addon::path_to_utf8(root / "selected");
#ifdef _WIN32
    const auto fake = root / "kimi.exe";
#else
    const auto fake = root / "kimi";
#endif
    fs::copy_file(fs::absolute(executable), fake);
    {
        std::ofstream config(root / "selected" / "config.toml", std::ios::binary);
        config << kimi::updated_config("");
        require(bool(config), "resume fixture configuration must be written");
    }
    addon::Resume_prepare_request request;
    request.context.launcher.runner_key  = addon::k_runner_direct;
    request.context.launcher.script_body = '"' + addon::path_to_utf8(fake) + '"';
    request.context.launcher_argv        = {addon::path_to_utf8(fake)};
    request.context.working_directory    = addon::path_to_utf8(root);
    request.context.account_context = addon::Account_context{2, kimi::account_key(root / "selected"), home};
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    environment.effective_environment.set("KIMI_CODE_HOME", addon::Json_value(home));
    environment.effective_environment.set("KIMI_TEST_HELP", addon::Json_value(help));
    for (const auto* name : {"SystemRoot", "PATH"}) {
        if (const auto* value = std::getenv(name)) {
            environment.effective_environment.set(name, addon::Json_value(value));
        }
    }
    request.context.launch_environment = environment;
    request.resume_record.set("provider_session_id", addon::Json_value("session-selected"));
    Context context;
    context.m_request = {"resume", {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(request, 2)};
    if (!admitted) {
        bool rejected = false;
        try { kimi::handle_preparation(context); }
        catch (const std::runtime_error& error) {
            rejected = std::string(error.what()) == "launcher_program_unavailable";
        }
        require(rejected, "resume must reject help without the required optional session selector");
        return;
    }
    kimi::handle_preparation(context);
    addon::Contract_failure failure;
    const auto result = context.m_result ? addon::decode_resume_prepare_result(*context.m_result, &failure, 2) : std::nullopt;
    require(result && addon::validate_resume_process_proposal(result->process, &failure) &&
        addon::validate_launch_patch(result->patch, {}, {}, &failure), "resume producer must satisfy terminal policy");
    // The host installs the admitted selector and forbids even same-value
    // rewrites of it. The provider merely acknowledges the same account.
    require(result->patch.set.empty() && result->patch.unset.empty() && result->patch.account_context &&
        result->patch.account_context->profile_path == home, "resume must retain the host-bound account selector");
    require(result->process.argv.back() == "session-selected" &&
        addon::validate_resume_acknowledgment(request.resume_record, result->acknowledgment, &failure),
        "resume must preserve the selected native session identity");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "--version")) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        const auto* text = std::getenv(std::string(argv[1]) == "--help" ? "KIMI_TEST_HELP" : "KIMI_TEST_VERSION");
        if (!text) return 1;
        std::cout << text;
        return 0;
    }
    try {
        required_control_reaches_native_admission(argv[0]);
        kimi_tool_names();
        launch_disposition();
        for (const auto* help : {"  --session [id]\n", "  --session [id]\r\n",
                "  --session [id]", "  --session [id]  Resume a session\n"})
        {
            resume_policy(argv[0], help);
        }
        for (const auto* help : {"  --session id\n", "  --session [id]suffix\n", "  --help\n"}) {
            resume_policy(argv[0], help, false);
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
