#include "kimi_authoring.h"
#include "kimi_accounts.h"
#include "kimi_config.h"
#include "kimi_json.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/authoring_availability.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
namespace json  = logonomic::cli_addon::json;

constexpr Authoring_restriction launcher_restriction{"authoring_launcher_unavailable",
    "Text-only authoring requires a direct Kimi command with only an optional model argument."};
constexpr Authoring_restriction account_restriction{"account_profile_mismatch",
    "The selected Kimi account does not match this launcher's profile."};
constexpr Authoring_restriction relative_profile{"authoring_profile_relative",
    "Set KIMI_CODE_HOME to an absolute profile path so text-only authoring can use it from its temporary directory."};

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
    if (!context.launch_environment || request->prompt.empty() || !addon::is_valid_utf8(request->prompt)) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "authoring_request_unavailable",
            "Kimi needs a launch environment and a valid text prompt for authoring.");
        return;
    }
    if (!authoring_launcher_supported(context.launcher, words)) {
        invocation.fail(addon::Result_status::UNAVAILABLE, launcher_restriction.code, launcher_restriction.detail);
        return;
    }
    const auto home = profile_for_environment(*context.launch_environment, context.working_directory);
    if (context.account_context && !account_matches_profile(*context.account_context, home)) {
        const auto& reason = profile_selector(*context.launch_environment).is_absolute() ? account_restriction : relative_profile;
        invocation.fail(addon::Result_status::FAILED, reason.code, reason.detail);
        return;
    }
    if (const auto restriction = authoring_profile_restriction(home, addon::path_from_utf8(context.working_directory))) {
        invocation.fail(addon::Result_status::UNAVAILABLE, restriction->code, restriction->detail);
        return;
    }
    auto environment = environment_pairs(context.launch_environment->effective_environment);
    set_environment(environment, "KIMI_CODE_HOME", path_string(home));
    const auto program = verified_program(context.launcher, context.launcher_argv, environment,
        context.working_directory, Native_entrypoint::AUTHORING, invocation);
    if (!program) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "authoring_launcher_unavailable",
            "Kimi could not verify this launcher's native text-only authoring mode.");
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

void check_authoring_availability(addon::Invocation_context& invocation)
{
    if (invocation.request().operation != addon::k_authoring_availability_check_operation) {
        invocation.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_authoring_availability_request(invocation.request().payload, &failure);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto& selection = request->launcher;
    std::optional<Authoring_restriction> restriction;
    if (!authoring_launcher_supported(selection.launcher, addon::literal_argv(selection.launcher.script_body))) {
        restriction = launcher_restriction;
    }
    else {
        try {
            // Discovery knows the launcher's original directory. It cannot
            // carry that base into the native turn's fresh working directory.
            const auto home = profile_selector(selection.launch_environment).lexically_normal();
            if (!home.is_absolute()) {
                restriction = relative_profile;
            }
            else if (request->account_context && !account_matches_profile(*request->account_context, home)) {
                restriction = account_restriction;
            }
            else {
                restriction = authoring_profile_restriction(home);
            }
        }
        catch (const std::exception&) {
            restriction = Authoring_restriction{"authoring_profile_unavailable",
                "Kimi could not locate the selected profile for text-only authoring."};
        }
    }
    invocation.succeed(addon::encode_authoring_availability_result(
        {!restriction, restriction ? restriction->detail : "", request->account_context}));
}

void handle_authoring(addon::Invocation_context& invocation)
{
    const auto& operation = invocation.request().operation;
    if (operation == "prepare") prepare_authoring(invocation);
    else if (operation == "inspect") inspect_authoring(invocation);
    else invocation.reject_unknown_operation();
}

} // namespace kimi
