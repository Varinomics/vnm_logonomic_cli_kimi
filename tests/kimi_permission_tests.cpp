#include "kimi_json.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/package_source.h>

#include <QByteArray>
#include <QChar>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
namespace json_util = addon::json;
namespace fs = std::filesystem;
using kimi::Json;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Workspace
{
public:
    Workspace()
    :
        m_path(fs::temp_directory_path() / ("kimi-permission-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_path);
        m_path = fs::canonical(m_path);
    }

    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    fs::path m_path;
};

int capture_delivery(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "request-title-instruction") {
        std::cout << R"({"accepted":true,"instruction_required":false,"instruction":""})" << '\n';
        return 0;
    }
    require(argc == 4 && std::string_view(argv[1]) == "provider-event", "unexpected helper command");
    if (std::string_view(argv[2]) == "acknowledge") {
        return 0;
    }
    require(std::string_view(argv[2]) == "deliver" && std::string_view(argv[3]) == "--args-stdin",
        "helper delivery must use stdin");
    const auto* path = std::getenv("KIMI_TEST_EVENT_CAPTURE");
    require(path != nullptr, "helper fixture capture path is missing");
    const std::string bytes{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    std::ofstream output(addon::path_from_utf8(path), std::ios::binary);
    output << bytes;
    output.close();
    require(output.good(), "helper fixture capture failed");
    std::cout << R"({"accepted":true,"epoch":1})" << '\n';
    return 0;
}

Json native_permission(std::string_view name, std::string_view id, std::string_view agent)
{
    // Oracle: Kimi toolApprovalService preserves this approval context on both
    // hooks; matchHooks converts top-level keys to snake_case before stdin.
    return json_util::object({
        {"hook_event_name", Json(name)},
        {"session_id",      Json("session-permission")},
        {"id",              Json(id)},
        {"agent_id",        Json(agent)},
        {"turn_id",         Json(7)},
        {"tool_call_id",    Json("call_deploy_preview")},
        {"tool_name",       Json("Shell")},
        {"action",          Json("execute")},
        {"display",         json_util::object({{"command", Json("private-command")}})},
        {"tool_input",      json_util::object({{"command", Json("private-command")}})},
        {"decision",        Json("approved")},
        {"feedback",        Json("private-feedback")},
    });
}

addon::Provider_event relay(
    const fs::path& package,
    const fs::path& executable,
    const fs::path& home,
    const addon::Host_simulator& host,
    const Json& payload)
{
    const auto capture = home / "captured-event.json";
    fs::remove(capture);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(addon::k_helper_env_vnmctl_path, QString::fromStdString(addon::path_to_utf8(executable)));
    environment.insert(addon::k_helper_env_session_capability, QString(64, QChar('a')));
    environment.insert(addon::k_helper_env_package_id, QStringLiteral("logonomic.cli.kimi"));
    environment.insert(addon::k_helper_env_generation_digest, QString::fromStdString(host.generation_digest()));
    environment.insert(addon::k_helper_env_generation_nonce, QString::fromStdString(host.generation_nonce()));
    environment.insert(addon::k_helper_env_helpers_dir,
        QString::fromStdString(addon::path_to_utf8(package / "helpers")));
    environment.insert(QStringLiteral("KIMI_CODE_HOME"), QString::fromStdString(addon::path_to_utf8(home)));
    environment.insert(QStringLiteral("KIMI_TEST_EVENT_CAPTURE"), QString::fromStdString(addon::path_to_utf8(capture)));

    auto helper = package / "helpers/kimi_helper";
#ifdef _WIN32
    helper += ".exe";
#endif
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setProgram(QString::fromStdString(addon::path_to_utf8(helper)));
    process.start();
    require(process.waitForStarted(5000), "packaged helper did not start");
    const auto bytes = QByteArray::fromStdString(addon::serialize_json(payload));
    require(process.write(bytes) == bytes.size(), "native hook input was not written");
    process.closeWriteChannel();
    require(process.waitForFinished(10000), "packaged helper did not finish");
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
        "packaged helper rejected a native hook");
    require(process.readAllStandardOutput().isEmpty(), "permission hooks must not print private hook data");
    std::ifstream input(capture, std::ios::binary);
    const std::string captured{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto envelope = json_util::parse(captured);
    require(envelope.has_value(), "native permission hook was dropped before provider-event delivery");
    const auto& event = json_util::member(*envelope, "event");
    require(event.is_object(), "helper must deliver a provider event");
    require(captured.find("private-command") == std::string::npos &&
        captured.find("private-feedback") == std::string::npos,
        "approval content must stay in the native process");
    return {json_util::text(event, "name"), json_util::member(event, "body")};
}

Json invoke(addon::Host_simulator& host, addon::Capability_pair capability,
    std::string operation, Json payload)
{
    const auto id = host.invoke(capability, std::move(operation), std::move(payload), 5000);
    const auto result = host.wait_outcome(id, 10000);
    require(result.has_value(), "packaged adapter did not answer");
    require(result->status == addon::Host_status::SUCCESS, result->reason_code);
    return result->payload;
}

addon::Attention_classification_result classify(
    addon::Host_simulator& host,
    const addon::Classify_event_request& request)
{
    const auto payload = invoke(host, {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
        "classify_event", addon::encode_attention_classification_request({request, std::nullopt}));
    addon::Contract_failure failure;
    const auto result = addon::decode_attention_classification_result(payload, &failure);
    require(result.has_value(), "packaged adapter returned invalid attention metadata: " + failure.reason_code);
    return *result;
}

void permission_pair(const fs::path& package, const fs::path& executable)
{
    Workspace workspace;
    addon::Simulator_options options;
    options.package_directory = package;
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), failure.reason_code + ": " + failure.detail);

    const auto nonce = addon::random_nonce_hex();
    invoke(host, {addon::k_lifecycle_correlation_lifetime_capability_id, 1}, "reserve",
        addon::encode_lifecycle_correlation_reserve_request({{nonce}}));
    addon::Classify_event_request request;
    request.transition_nonce = nonce;
    request.event = relay(package, executable, workspace.m_path, host,
        json_util::object({{"hook_event_name", Json("SessionStart")},
            {"session_id", Json("session-permission")}, {"source", Json("new")}}));
    const auto started = classify(host, request);
    require(started.base.classification == addon::Classification::TRANSITION && !started.attention_kind,
        "session admission remains the shared classification path");

    const std::string first_id  = "approval_2eb0b40f-75e5-4880-8b6b-c1305f6294f1";
    const std::string second_id = "approval_b590bd4c-78da-4d18-a319-b06c5a7f6453";
    request.event = relay(package, executable, workspace.m_path, host,
        native_permission("PermissionRequest", first_id, "main"));
    require(classify(host, request).base.classification == addon::Classification::IGNORED,
        "permission cannot open a host epoch");
    request.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, "session-permission", nonce}};

    for (const auto* event : {"PermissionRequest", "PermissionResult"}) {
        for (const auto& [id, agent] : {std::pair{first_id, "main"}, {second_id, "agent-review"}}) {
            request.event = relay(package, executable, workspace.m_path, host, native_permission(event, id, agent));
            require(json_util::text(request.event.body, "id") == id &&
                json_util::text(request.event.body, "agent_id") == agent &&
                json_util::text(request.event.body, "tool_call_id") == "call_deploy_preview",
                "helper must preserve native request, agent and invocation identities");
            const auto result = classify(host, request);
            require(result.base.classification == addon::Classification::EVENT && result.base.observations.size() == 1,
                "one permission hook must classify only its own request");
            const auto& observation = result.base.observations.front();
            require(observation.provider_event_name == event &&
                observation.provider_session_id == "session-permission" &&
                observation.attention_request_id == id && observation.agent_id == agent,
                "classifier must preserve native permission identity");
            require(observation.attention_transition == (std::string_view(event) == "PermissionRequest"
                    ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED) &&
                observation.attention_evidence == addon::Evidence::CONFIRMED,
                "native request and result must produce confirmed paired attention");
            require(result.attention_kind && result.attention_kind->kind == "permission" &&
                result.attention_kind->observation_index == 0,
                "the host must receive permission presentation metadata");
            require(observation.provider_turn_id.empty() &&
                observation.phase_transition == addon::Phase_transition::NONE &&
                observation.outcome_transition == addon::Outcome_transition::NONE,
                "agent approval cannot assert a root turn, phase or outcome");

            const auto base_payload = invoke(host, {"helper-ingress", 2}, "classify_event",
                addon::encode_classify_event_request(request));
            const auto base = addon::decode_classify_event_result(base_payload, &failure);
            require(base && !base_payload.find("attention_kind") && base->observations.size() == 1 &&
                addon::serialize_json(addon::encode_lifecycle_observation(base->observations.front())) ==
                    addon::serialize_json(addon::encode_lifecycle_observation(observation)),
                "the base and companion capability must share the classification producer");
        }
    }

    // Kimi starts each hook process independently. A result may arrive first;
    // the host's existing request tombstone owns suppression of a late request.
    request.event = relay(package, executable, workspace.m_path, host,
        native_permission("PermissionResult", "approval_86b6d226-b30e-41a2-af43-a1cc925d7cd7", "agent-test"));
    require(classify(host, request).base.observations.size() == 1,
        "an early native result must reach the host's per-request tombstone");
    const auto admitted = request;
    request.event.body.set("session_id", Json("session-other"));
    require(classify(host, request).base.classification == addon::Classification::IGNORED,
        "another native session cannot settle permission attention");
    request = admitted;
    request.event.body.set("kimi_code_home", Json(addon::path_to_utf8(workspace.m_path / "other-profile")));
    require(classify(host, request).base.classification == addon::Classification::IGNORED,
        "another native profile cannot settle permission attention");
    request = admitted;
    request.ledger.current_transition->nonce = addon::random_nonce_hex();
    require(classify(host, request).base.classification == addon::Classification::IGNORED,
        "an unbound correlation cannot settle permission attention");
    request = admitted;
    request.event.name = "UserPromptSubmit";
    require(classify(host, request).base.observations.empty(),
        "an unrelated prompt cannot settle permission attention");

    invoke(host, {addon::k_lifecycle_correlation_lifetime_capability_id, 1}, "retire",
        addon::encode_lifecycle_correlation_retire_request({{nonce},
            addon::Lifecycle_correlation_retire_disposition::ABANDONED, std::nullopt}));
    require(classify(host, admitted).base.classification == addon::Classification::IGNORED,
        "retired session correlation cannot publish late attention");
    int exit_code = -1;
    require(host.shutdown(5000, &exit_code) && exit_code == 0, "packaged adapter must stop cleanly");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc > 2) {
            return capture_delivery(argc, argv);
        }
        require(argc == 2, "usage: kimi_permission_tests <package directory>");
        permission_pair(fs::absolute(addon::path_from_utf8(argv[1])), fs::absolute(addon::path_from_utf8(argv[0])));
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
