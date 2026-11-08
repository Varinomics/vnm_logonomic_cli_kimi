#include "kimi_preparation.h"
#include "kimi_acp.h"
#include "kimi_accounts.h"
#include "kimi_config.h"

#include <logonomic_cli_addon_test/invocation_context.h>

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

using Context = addon::test::Throwing_invocation_context;

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
    context.m_request = {"delegated", {"delegated-session", 3}, "prepare", addon::encode_delegated_prepare_request(request)};
    kimi::handle_preparation(context);
    addon::Contract_failure failure;
    const auto result = context.m_result ? addon::decode_delegated_prepare_result(*context.m_result, &failure) : std::nullopt;
    require(result && result->acknowledgment.connection_version == addon::k_delegated_connection_version &&
        result->acknowledgment.server_ids == std::vector<std::string>{"coordination", "delegation"},
        "required actions reach a relay-acknowledged native proposal");
    require(result->process.argv == std::vector<std::string>{addon::path_to_utf8(fake), "acp"} &&
        result->inspection.incremental && result->inspection.keep_stdin_open,
        "the proposal runs the verified launcher's native ACP server");
    for (const auto& options : std::vector<std::vector<std::string>>{{"--yolo"}, {"-y"}, {"--auto"}, {"--plan"}}) {
        auto selected = request;
        for (const auto& option : options) selected.context.launcher.script_body += " " + option;
        selected.context.launcher_argv.insert(selected.context.launcher_argv.end(), options.begin(), options.end());
        Context configured;
        configured.m_request = {"delegated", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(selected)};
        kimi::handle_preparation(configured);
        const auto proposal = configured.m_result
            ? addon::decode_delegated_prepare_result(*configured.m_result, &failure) : std::nullopt;
        auto expected = selected.context.launcher_argv;
        expected.push_back("acp");
        require(proposal && proposal->process.argv == expected, "launcher permission options are preserved in delegated ACP");
    }
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
    const std::string command_catalog =
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"available_commands_update","availableCommands":[{"name":"mcp","description":"Show MCP server status"}]}}})" "\n"
        ;
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
    const auto querying = inspect(inspect(result->inspection.protocol_state, session_setup).protocol_state, command_catalog);
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
    colliding.m_request = {"delegated", {"delegated-session", 3}, "prepare", addon::encode_delegated_prepare_request(request)};
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

void profile_selector_consistency()
{
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("kimi-profiles-" + addon::random_nonce_hex());
    fs::create_directories(scratch / ".kimi-code");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{scratch};
    const auto root = fs::canonical(scratch);
    const auto selected = root / ".kimi-code";
    const auto sibling = root / ".kimi-code work";
    fs::create_directories(sibling / "credentials");
    std::ofstream(sibling / "credentials/kimi-code.json") << "{}";
#ifndef _WIN32
    const auto unselectable = root / ".kimi-code ";
    fs::create_directories(unselectable / "credentials");
    std::ofstream(unselectable / "credentials/kimi-code.json") << "{}";
#endif
    const auto root_text = addon::path_to_utf8(root);
    const auto selected_text = addon::path_to_utf8(selected);
#ifdef _WIN32
    const char* home_name = "USERPROFILE";
#else
    const char* home_name = "HOME";
#endif
    int selector_case = 0;
    const auto check = [&](addon::Launch_environment environment, const std::string& fallback,
                           const fs::path& expected, bool prepare_original_selector) {
        ++selector_case;
        addon::Accounts_launcher_request accounts_request;
        accounts_request.launcher = {"kimi", addon::k_runner_direct, ""};
        accounts_request.launch_environment = environment;
        accounts_request.default_home_fallback = fallback;
        accounts_request.normalization_directory = root_text;
        Context discovery;
        discovery.m_request = {"discover", {"accounts", 2}, "discover",
            addon::encode_accounts_launcher_request(accounts_request)};
        kimi::handle_accounts(discovery);
        addon::Contract_failure failure;
        const auto accounts = discovery.m_result ? addon::decode_accounts_result(*discovery.m_result, &failure) : std::nullopt;
        const auto expected_text = addon::path_to_utf8(expected);
        const auto expected_key  = kimi::account_key(expected);
        const bool matches =
            accounts && !accounts->empty()                  &&
            accounts->front().profile_path == expected_text &&
            accounts->front().adapter_account_key == expected_key;
        if (!matches) {
            std::cerr
                << "selector_case=" << selector_case
                << "\nexpected_profile_path=" << addon::serialize_json(addon::Json_value(expected_text))
                << "\nnormalized_expected_profile_path="
                << addon::serialize_json(addon::Json_value(addon::path_to_utf8(expected.lexically_normal())))
                << "\nexpected_account_key=" << expected_key
                << "\ndecode_failure=" << failure.reason_code << ": " << failure.detail
                << "\ndiscovery="
                << (discovery.m_result ? addon::serialize_json(*discovery.m_result) : "no result") << '\n';
        }
        require(matches, "discovery must resolve the managed-configuration v1 selector");
        if (expected == selected) {
            require(accounts->size() == 2 && accounts->at(1).profile_path == addon::path_to_utf8(sibling),
                "discovery must keep internal spaces and omit siblings that v1 would trim into another path");
        }
        Context resolution;
        resolution.m_request = discovery.m_request;
        resolution.m_request.operation = "resolve";
        kimi::handle_accounts(resolution);
        const auto resolved = resolution.m_result
            ? addon::decode_account_resolution_result(*resolution.m_result, &failure) : std::nullopt;
        require(resolved && resolved->adapter_account_key == accounts->front().adapter_account_key,
            "resolve must identify the same profile as discovery");
        addon::Prepare_request prepare;
        prepare.launcher = accounts_request.launcher;
        prepare.launcher_argv = {"kimi"};
        prepare.working_directory = root_text;
        prepare.account_context = addon::Account_context{2,
            accounts->front().adapter_account_key, accounts->front().profile_path};
        // The host binds the discovered path before preparing a selected
        // account. Also exercise the inherited selector where it is complete.
        for (const bool bind : {false, true}) {
            if (!bind && !prepare_original_selector) continue;
            if (bind) environment.effective_environment.set("KIMI_CODE_HOME", addon::Json_value(expected_text));
            prepare.launch_environment = environment;
            Context preparation;
            preparation.m_request = {"prepare", {"launch-preparation", 3}, "prepare",
                addon::encode_prepare_request(prepare, 3)};
            kimi::handle_preparation(preparation);
            const auto patch = preparation.m_result
                ? addon::decode_launch_patch(*preparation.m_result, &failure, 3) : std::nullopt;
            require(patch && patch->apply_managed_configuration && patch->account_context &&
                patch->account_context->adapter_account_key == accounts->front().adapter_account_key &&
                patch->account_context->profile_path == accounts->front().profile_path,
                "preparation must retain and accept the resolved account");
        }
    };
    for (const auto& selector : {" \t" + selected_text + " \n", std::string(" .kimi-code\t"),
            std::string("\xc2\xa0") + selected_text + "\xe2\x80\x83"})
    {
        check(native_environment(selector), {}, selected, true);
    }
    auto environment = native_environment(" \t\n");
    environment.effective_environment.set(home_name, addon::Json_value(root_text));
    check(environment, {}, selected, true);
    environment.effective_environment.erase("KIMI_CODE_HOME");
    check(environment, {}, selected, true);
    environment.effective_environment.set(home_name, addon::Json_value(""));
    check(environment, addon::path_to_utf8(root / "fallback"), selected, true);
    environment.effective_environment.erase(home_name);
    check(environment, addon::path_to_utf8(root / "fallback"), root / "fallback" / ".kimi-code", false);
#ifndef _WIN32
    require(fs::is_directory(unselectable), "discovery must not rename or remove an unselectable sibling");
#endif
}

std::string three_hook_configuration()
{
    // Frozen profile syntax preceding the permission-hook addition, from
    // 7bc7f64b245a57d9f394ec36262b722c66300ab9. updated_config would repair the
    // exact upgrade condition under test before preparation could inspect it.
#ifdef _WIN32
    const std::string command = R"toml(command = "powershell.exe -NoLogo -NoProfile -NonInteractive -Command \"if ($env:LOGONOMIC_PROVIDER_HELPERS_DIR) { & (Join-Path (Split-Path -LiteralPath $env:LOGONOMIC_PROVIDER_HELPERS_DIR) 'adapter/kimi_helper.exe') }\"")toml";
#else
    const std::string command = R"toml(command = "if [ -n \"$LOGONOMIC_PROVIDER_HELPERS_DIR\" ]; then \"${LOGONOMIC_PROVIDER_HELPERS_DIR%/*}/adapter/kimi_helper\"; fi")toml";
#endif
    return "\n# Logonomic managed Kimi hook: SessionStart\n[[hooks]]\nevent = \"SessionStart\"\n" +
        command + "\ntimeout = 18\n" +
        "\n# Logonomic managed Kimi hook: SessionEnd\n[[hooks]]\nevent = \"SessionEnd\"\n" +
        command + "\ntimeout = 13\n" +
        "\n# Logonomic managed Kimi hook: UserPromptSubmit\n[[hooks]]\nevent = \"UserPromptSubmit\"\n" +
        command + "\ntimeout = 23\n";
}

void start_hook_validation()
{
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("kimi-start-hook-" + addon::random_nonce_hex());
    fs::create_directories(scratch);
    class Cleanup
    {
    public:
        fs::path path;
        ~Cleanup()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{scratch};
    const auto home = addon::path_to_utf8(scratch);
    const auto check = [
            &scratch, &home
        ](
            const std::string& config, bool expected, const char* description)
        {
            {
                std::ofstream output(scratch / "config.toml", std::ios::binary);
                output << config;
                output.close();
                require(output.good(), "start-hook fixture must be completely written");
            }
            require(kimi::has_start_hook(home) == expected, description);
        };

    const auto three_hooks = three_hook_configuration();
    const auto start = three_hooks.substr(0, three_hooks.find("\n# Logonomic managed Kimi hook: SessionEnd"));
    check({}, false, "a missing SessionStart hook must remain unavailable");
    check(start, true, "SessionStart admission does not depend on unrelated lifecycle hooks");
    check(three_hooks, true, "the installed three-hook profile must remain resumable");
    check(kimi::updated_config(three_hooks), true, "complete managed installation must remain resumable");
    std::string crlf;
    for (const char character : start) {
        if (character == '\n') {
            crlf += '\r';
        }
        crlf += character;
    }
    check("description = '作業設定'\r\n" + crlf, true,
        "CRLF and user Unicode text must not change installed hook admission");
    check(three_hooks + kimi::hook_entry("PermissionRequest", 13) + "matcher = 'Shell'\n", true,
        "unrelated permission-hook modifications must not veto an intact SessionStart route");
    check(start + start, false, "duplicate managed SessionStart hooks still require repair");
    check("description = '''\n" + start + "'''\n", false,
        "a marker inside a string cannot establish an installed hook");

    auto wrong_command = start;
    wrong_command.replace(wrong_command.find("kimi_helper"), std::string("kimi_helper").size(), "other_helper");
    check(wrong_command, false, "a different helper command must not establish the required route");
    auto wrong_timeout = start;
    wrong_timeout.replace(wrong_timeout.find("timeout = 18"), std::string("timeout = 18").size(), "timeout = 1");
    check(wrong_timeout, false, "the SessionStart delivery budget must remain validated");
    bool modified_refused = false;
    try {
        check(start + "matcher = 'Shell'\n", false, "modified SessionStart must not be admitted");
    }
    catch (const std::runtime_error& error) {
        modified_refused = std::string(error.what()) == "managed_hook_modified";
    }
    require(modified_refused, "extra fields on the required managed hook must retain explicit refusal");
}

void resume_policy(
    const char* executable, const char* help, bool admitted = true,
    const std::optional<std::string>& configuration = std::nullopt, bool scoped_checks = false)
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
    const auto original_configuration = configuration ? *configuration : kimi::updated_config("");
    {
        std::ofstream config(root / "selected" / "config.toml", std::ios::binary);
        config << original_configuration;
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
    std::ifstream persisted(root / "selected" / "config.toml", std::ios::binary);
    const std::string unchanged{std::istreambuf_iterator<char>(persisted), std::istreambuf_iterator<char>()};
    require(unchanged == original_configuration, "resume admission must not rewrite the selected profile");
    if (!scoped_checks) {
        return;
    }

    // Native metadata and --session address the same ID inside the original
    // KIMI_CODE_HOME. Explicit preparation uses the inspection lookup afresh.
    const auto session = root / "selected/sessions/workspace/session-selected";
    fs::create_directories(session);
    const auto write_state = [&](const fs::path& directory) {
        fs::create_directories(directory);
        std::ofstream state(directory / "state.json", std::ios::binary | std::ios::trunc);
        state << R"({"id":"session-selected","version":2,"createdAt":1,"updatedAt":2,"archived":false,"agents":{},"custom":{}})";
        state.close();
        require(state.good(), "scoped resume metadata fixture is complete");
    };
    write_state(session);
    auto native_store = addon::Json_value::object();
    native_store.set("version", addon::Json_value(1));
    native_store.set("kimi_code_home", addon::Json_value(home));
    request.resume_record.set("native_store", native_store);
    const auto scoped_record = request.resume_record;
    const auto prepare = [&](const addon::Resume_prepare_request& selected, const std::string& reason = {}) {
        Context invocation;
        invocation.m_request = {"scoped-resume", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(selected, 2)};
        if (!reason.empty()) {
            bool refused = false;
            try {
                kimi::handle_preparation(invocation);
            }
            catch (const std::runtime_error& error) {
                refused = error.what() == reason;
            }
            require(refused && !invocation.m_result, "unverified or absent native target is not prepared");
            return;
        }
        kimi::handle_preparation(invocation);
        const auto prepared = invocation.m_result
            ? addon::decode_resume_prepare_result(*invocation.m_result, &failure, 2) : std::nullopt;
        require(prepared && addon::validate_resume_process_proposal(prepared->process, &failure) &&
            addon::validate_resume_acknowledgment(selected.resume_record, prepared->acknowledgment, &failure),
            "scoped preparation acknowledges the exact original native store and selector");
        require(prepared->process.argv == std::vector<std::string>{addon::path_to_utf8(fake), "--session", "session-selected"} &&
            prepared->patch.set.empty() && prepared->patch.unset.empty(),
            "scoped resume preserves the account selector and native argument semantics");
    };
    prepare(request);
    fs::create_directories(root / "destination/nested");
    request.context.working_directory = addon::path_to_utf8(root / "destination/nested");
    prepare(request);
    require(request.resume_record == scoped_record, "the host-selected launch directory does not rewrite conversation identity");

    auto foreign = request;
    foreign.resume_record.find("native_store")->set("kimi_code_home", addon::Json_value(addon::path_to_utf8(root / "foreign")));
    prepare(foreign, "resume_scope_mismatch");
    auto foreign_environment = request;
    foreign_environment.context.launch_environment->effective_environment.set("KIMI_CODE_HOME",
        addon::Json_value(addon::path_to_utf8(root / "foreign")));
    prepare(foreign_environment, "resume_scope_mismatch");
    write_state(root / "selected/sessions/second-workspace/session-selected");
    prepare(request, "native_session_ambiguous");
    fs::remove_all(root / "selected/sessions/second-workspace");
    fs::remove(session / "state.json");
    prepare(request);
    fs::remove_all(session);
    prepare(request, "native_session_absent");
    write_state(session);
    prepare(request);
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
        profile_selector_consistency();
        for (const auto* help : {"  --session [id]\n", "  --session [id]\r\n",
                "  --session [id]", "  --session [id]  Resume a session\n"})
        {
            resume_policy(argv[0], help);
        }
        for (const auto* help : {"  --session id\n", "  --session [id]suffix\n", "  --help\n"}) {
            resume_policy(argv[0], help, false);
        }
        resume_policy(argv[0], "  --session [id]\n", true, three_hook_configuration());
        resume_policy(argv[0], "  --session [id]\n", true, std::nullopt, true);
        start_hook_validation();
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
