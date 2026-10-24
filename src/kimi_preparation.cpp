#include "kimi_preparation.h"
#include "kimi_acp.h"
#include "kimi_accounts.h"
#include "kimi_config.h"
#include "kimi_json.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <filesystem>
#include <regex>
#include <stdexcept>

namespace json_util = logonomic::cli_addon::json;

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

#ifdef _WIN32
constexpr const char* k_bridge = "helpers/kimi_launch_bridge.exe";
#else
constexpr const char* k_bridge = "helpers/kimi_launch_bridge";
#endif

void unavailable(addon::Invocation_context& context)
{
    context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
        context.cancel_requested() ? "cancelled" : "launcher_program_unavailable");
}

std::string bound_home(const Environment& environment, std::string_view working_directory,
    const std::optional<addon::Account_context>& account)
{
    const auto home = code_home(environment, working_directory);
    if (account && (!account->profile_path ||
        account_key(addon::path_from_utf8(*account->profile_path)) != account->adapter_account_key ||
        account_key(addon::path_from_utf8(home)) != account->adapter_account_key))
    {
        throw std::runtime_error("account_context_mismatch");
    }
    return home;
}

void prepare_launch(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_prepare_request(context.request().payload, &error, 3);
    if (!request) { context.fail(addon::Result_status::FAILED, error.reason_code, error.detail); return; }
    if (!is_kimi_invocation(request->launcher_argv)) {
        addon::Launch_patch patch;
        patch.profile_storage = request->profile_storage;
        patch.account_context = request->account_context;
        patch.disposition = addon::Launch_disposition{false, false, false};
        context.succeed(addon::encode_launch_patch(patch, 3));
        return;
    }
    const auto environment = environment_pairs(request->launch_environment->effective_environment);
    try { (void)bound_home(environment, request->working_directory, request->account_context); }
    catch (const std::runtime_error&) {
        context.fail(addon::Result_status::FAILED, "account_context_mismatch"); return;
    }
    if (context.cancel_requested()) { unavailable(context); return; }
    addon::Launch_patch patch;
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    patch.disposition = addon::Launch_disposition{true, true, true};
    patch.apply_managed_configuration = true;
    context.succeed(addon::encode_launch_patch(patch, 3));
}

void prepare_delegated(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_delegated_prepare_request(context.request().payload, &error);
    if (!request) { context.fail(addon::Result_status::FAILED, error.reason_code, error.detail); return; }
    if (request->purpose != addon::Structured_purpose::IMPLEMENTATION) {
        context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
        return;
    }
    if (request->prompt.empty() || !addon::is_valid_utf8(request->prompt) ||
        !addon::path_from_utf8(request->context.working_directory).is_absolute()) {
        context.fail(addon::Result_status::FAILED, "invalid_implementation_request");
        return;
    }
    if (!request->control.connection) {
        context.fail(addon::Result_status::UNAVAILABLE, "task_server_connection_unavailable");
        return;
    }
    if (const auto collision = required_action_collision(*request)) {
        context.fail(addon::Result_status::UNAVAILABLE, "required_control_name_collision",
            "Kimi registers the required actions " + *collision + " under one tool name.");
        return;
    }
    auto environment = environment_pairs(request->context.launch_environment->effective_environment);
    (void)bound_home(environment, request->context.working_directory, request->context.account_context);
    const auto program = verified_program(request->context.launcher, request->context.launcher_argv,
        environment, request->context.working_directory, false, context);
    if (!program) { unavailable(context); return; }
    addon::Delegated_prepare_result result;
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
    result.process.executable = k_bridge;
    result.process.argv = addon::literal_argv(request->context.launcher.script_body);
    result.process.argv.front() = *program;
    result.process.argv.push_back("acp");
    result.process.environment = std::move(environment);
    result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.process.stdin_payload = initial_acp_input(*request);
    result.acknowledgment.connection_version = addon::k_delegated_connection_version;
    result.inspection.incremental = true;
    result.inspection.keep_stdin_open = true;
    result.inspection.protocol_state = initial_acp_state(*request);
    for (const auto& server : request->control.servers) result.acknowledgment.server_ids.push_back(server.server_id);
    auto payload = addon::encode_delegated_prepare_result(result);
    if (!addon::validate_session_exchange_payload(payload, &error)) {
        context.fail(addon::Result_status::FAILED, error.reason_code, error.detail);
        return;
    }
    context.succeed(std::move(payload));
}

void prepare_resume(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_resume_prepare_request(context.request().payload, &error, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, error.reason_code, error.detail); return; }
    const auto session = json_util::text(request->resume_record, "provider_session_id");
    static const std::regex identity("[A-Za-z0-9][A-Za-z0-9_-]{0,255}");
    if (!json_util::exact_keys(request->resume_record, {"provider_session_id"}) || !std::regex_match(session, identity)) {
        context.fail(addon::Result_status::FAILED, "invalid_resume_record");
        return;
    }
    const auto workspace = addon::path_from_utf8(request->context.working_directory);
    if (!workspace.is_absolute() || !fs::is_directory(workspace) ||
        fs::canonical(workspace).generic_u8string() != workspace.generic_u8string()) {
        context.fail(addon::Result_status::FAILED, "invalid_resume_worktree");
        return;
    }
    auto environment = environment_pairs(request->context.launch_environment->effective_environment);
    const auto home = bound_home(environment, request->context.working_directory, request->context.account_context);
    const auto program = verified_program(request->context.launcher, request->context.launcher_argv,
        environment, request->context.working_directory, true, context);
    if (!program) { unavailable(context); return; }
    if (!has_start_hook(home)) {
        context.fail(addon::Result_status::UNAVAILABLE, "session_start_hook_unavailable");
        return;
    }
    auto argv = addon::literal_argv(request->context.launcher.script_body);
    argv.front() = *program;
    argv.insert(argv.end(), {"--session", session});
    addon::Resume_prepare_result result;
    result.process.executable = k_bridge;
    result.process.argv = std::move(argv);
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    // The terminal inherits the launch environment, including the host-bound
    // account selector. A package patch must not rewrite that selector.
    result.patch.profile_storage = request->context.profile_storage;
    result.patch.account_context = request->context.account_context;
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(request->resume_record);
    context.succeed(addon::encode_resume_prepare_result(result, 2));
}

} // namespace

void handle_preparation(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.capability.capability_id == "interactive-resume" && request.operation == "prepare_resume") {
        prepare_resume(context);
    }
    else
    if (request.operation != "prepare") context.reject_unknown_operation();
    else
    if (request.capability.capability_id == "launch-preparation") prepare_launch(context);
    else
    if (request.capability.capability_id == "delegated-session") prepare_delegated(context);
    else
    if (request.capability.capability_id == "structured-session") {
        // Kimi's native structured entry requires the delegated control block;
        // read-only review and control-free execution were never advertised.
        context.fail(addon::Result_status::UNAVAILABLE, "delegated_control_required");
    }
    else context.reject_unknown_operation();
}

} // namespace kimi
