#include "kimi_authoring.h"
#include "kimi_accounts.h"
#include "kimi_config.h"
#include "kimi_json.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
namespace json  = logonomic::cli_addon::json;

void prepare_authoring(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_authoring_prepare_request(invocation.request().payload, &failure);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto& context = request->context;
    const auto words = addon::literal_argv(context.launcher.script_body);
    if (!context.launch_environment || request->prompt.empty() || !addon::is_valid_utf8(request->prompt) ||
        (words.size() != 1 && (words.size() != 3 || (words[1] != "-m" && words[1] != "--model"))))
    {
        invocation.fail(addon::Result_status::UNAVAILABLE, "authoring_launcher_unavailable");
        return;
    }
    const auto home = profile_for_environment(*context.launch_environment, context.working_directory);
    if (context.account_context && !account_matches_profile(*context.account_context, home)) {
        invocation.fail(addon::Result_status::FAILED, "account_profile_mismatch");
        return;
    }
    if (!authoring_profile_admitted(home, addon::path_from_utf8(context.working_directory))) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "authoring_callbacks_configured");
        return;
    }
    auto environment = environment_pairs(context.launch_environment->effective_environment);
    set_environment(environment, "KIMI_CODE_HOME", path_string(home));
    const auto program = verified_program(context.launcher, context.launcher_argv, environment,
        context.working_directory, Native_entrypoint::AUTHORING, invocation);
    if (!program) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "authoring_launcher_unavailable");
        return;
    }
    addon::Structured_prepare_result result;
    result.profile_storage = context.profile_storage;
    result.account_context = context.account_context;
#ifdef _WIN32
    result.process.executable = "adapter/kimi_helper.exe";
#else
    result.process.executable = "adapter/kimi_helper";
#endif
    result.process.argv = {"--authoring-print", *program};
    result.process.argv.insert(result.process.argv.end(), words.begin() + 1, words.end());
    result.process.environment = std::move(environment);
    result.process.working_directory = addon::Working_directory_intent::SCRATCH_DIRECTORY;
    result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.process.stdin_payload = request->prompt;
    invocation.succeed(addon::encode_structured_prepare_result(result));
}

std::optional<std::string> assistant_reply(std::string_view input)
{
    std::optional<std::string> answer;
    bool initialized = false;
    bool resumed = false;
    while (!input.empty()) {
        const auto newline = input.find('\n');
        const auto line = input.substr(0, newline);
        input = newline == std::string_view::npos ? std::string_view{} : input.substr(newline + 1);
        if (line.empty() || line == "\r") continue;
        if (resumed) return std::nullopt;
        const auto frame = json::parse(line);
        if (!frame) return std::nullopt;
        const auto role = json::text(*frame, "role");
        const auto type = json::text(*frame, "type");
        if (!initialized) {
            if (role != "meta" || type != "system.version" || json::text(*frame, "version").empty()) return std::nullopt;
            initialized = true;
            continue;
        }
        if (role == "assistant" && !answer && json::exact_keys(*frame, {"role", "content"}) &&
            json::bounded_string(json::member(*frame, "content"), addon::limits::k_max_structured_stream_bytes)) {
            answer = json::text(*frame, "content");
            continue;
        }
        if (role != "meta") return std::nullopt;
        if (type == "turn.step.retrying" && !answer) continue;
        if (type != "session.resume_hint" || !answer || json::text(*frame, "session_id").empty()) return std::nullopt;
        resumed = true;
    }
    return resumed ? answer : std::nullopt;
}

void inspect_authoring(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_authoring_inspect_request(invocation.request().payload, &failure);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto& inspection = request->inspection;
    addon::Structured_inspect_result result;
    result.complete = true;
    if (inspection.final && !inspection.timed_out && inspection.exit_code == 0) {
        result.result_bytes = assistant_reply(inspection.stdout_bytes);
    }
    if (!result.result_bytes) result.failure_reason = "authoring_result_unavailable";
    invocation.succeed(addon::encode_structured_inspect_result(result));
}

} // namespace

void handle_authoring(addon::Invocation_context& invocation)
{
    const auto& operation = invocation.request().operation;
    if (operation == "prepare") prepare_authoring(invocation);
    else if (operation == "inspect") inspect_authoring(invocation);
    else invocation.reject_unknown_operation();
}

} // namespace kimi
