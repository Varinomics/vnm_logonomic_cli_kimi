#include "kimi_preparation.h"
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
#include <utility>

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

void required_control_is_explicitly_unavailable()
{
    // Regression: invoke the real preparation owner with a decoder-valid envelope.
    // A missing native executable is deliberate: the action-admission boundary
    // must answer first, without running a native probe or creating a proposal.
    const auto workspace = addon::path_to_utf8(std::filesystem::current_path());
    const auto home = addon::path_to_utf8(std::filesystem::current_path() / ".kimi-profile");
    const auto program = addon::path_to_utf8(std::filesystem::current_path() / "absent-required-control-native");
    addon::Delegated_prepare_request request;
    request.context.launcher = {"kimi", addon::k_runner_direct, program};
    request.context.launcher_argv = {program};
    request.context.working_directory = workspace;
    request.context.account_context = addon::Account_context{2,
        kimi::account_key(addon::path_from_utf8(home)), home};
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    environment.effective_environment.set("KIMI_CODE_HOME", addon::Json_value(home));
    request.context.launch_environment = environment;
    request.prompt = "A task that must not be released without its required control action.";
    request.control.task_id = "required-control-regression";
    request.control.private_artifact_root = workspace;
    request.control.servers.push_back({"control", program, {"task_status"},
        {{"LOGONOMIC_TASK_ID", request.control.task_id}}});
    addon::Contract_failure failure;
    const auto payload = addon::encode_delegated_prepare_request(request, 2);
    require(addon::decode_delegated_prepare_request(payload, &failure, 2).has_value(),
        "containment fixture must satisfy the actual public decoder");
    Context context;
    context.m_request = {"required-control", {"delegated-session", 2}, "prepare", payload};
    bool refused = false;
    try { kimi::handle_preparation(context); }
    catch (const std::runtime_error& error) {
        refused = std::string(error.what()) == "required_control_admission_unavailable";
    }
    require(refused && context.m_failure_status == addon::Result_status::UNAVAILABLE && !context.m_result,
        "unsupported required actions must produce no process proposal or task bytes");
    Context cancelled;
    cancelled.m_request = context.m_request;
    cancelled.m_cancelled = true;
    bool cancellation_preserved = false;
    try { kimi::handle_preparation(cancelled); }
    catch (const std::runtime_error& error) {
        cancellation_preserved = std::string(error.what()) == "cancelled";
    }
    require(cancellation_preserved && cancelled.m_failure_status == addon::Result_status::CANCELLED && !cancelled.m_result,
        "the fail-closed boundary must not relabel an already cancelled request");
    // The SDK accepts empty action lists, but the host requires nonempty ones.
    // Decoder acceptance here cannot establish a working host delegation path.
    request.control.servers.front().actions.clear();
    require(addon::decode_delegated_prepare_request(
        addon::encode_delegated_prepare_request(request, 2), &failure, 2).has_value(),
        "empty-action control servers remain decoder-valid, not host-valid");
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
    if (argc == 2 && std::string(argv[1]) == "--help") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        const auto* help = std::getenv("KIMI_TEST_HELP");
        if (!help) return 1;
        std::cout << help;
        return 0;
    }
    try {
        required_control_is_explicitly_unavailable();
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
