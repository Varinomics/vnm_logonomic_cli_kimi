#include "kimi_json.h"
#include "kimi_interactions.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>

#include <QByteArray>
#include <QChar>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    const Json& payload,
    std::string* diagnostic = nullptr)
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
    if (diagnostic) {
        *diagnostic = process.readAllStandardError().replace("\r\n", "\n").toStdString();
    }
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

void append_wire(const fs::path& path, const Json& record)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output << addon::serialize_json(record) << '\n';
    output.close();
    require(output.good(), "native interaction fixture was not written");
}

Json interaction(std::string_view id, std::string_view agent, bool resolved = false,
    std::string_view kind = "question")
{
    // Serialized InteractionRequestEvent / InteractionResolvedEvent from Kimi
    // 18bea3d: agent/interaction/interactionOps.ts and interactionWiring.ts.
    auto result = json_util::object({
        {"type",    Json(resolved ? "interaction.resolved" : "interaction.request")},
        {"agentId", Json(agent)},
        {"id",      Json(id)},
        {"time",    Json(std::int64_t{1793199600000LL})},
    });
    if (resolved) {
        result.set("response", json_util::object({{"cancelled", Json(true)}, {"reason", Json("turn_ended")}}));
    }
    else {
        result.set("kind", Json(kind));
        result.set("toolCallId", Json("call_native_interaction"));
        if (kind == "question") {
            auto questions = Json::array();
            auto choices = Json::array();
            choices.as_array().push_back(json_util::object({{"label", Json("Continue")},
                {"description", Json("private-choice")}}));
            questions.as_array().push_back(json_util::object({{"question", Json("private-question")},
                {"header", Json("Deploy")}, {"options", std::move(choices)}, {"multiSelect", Json(false)}}));
            result.set("request", json_util::object({{"turnId", Json(7)},
                {"toolCallId", Json("call_native_interaction")}, {"questions", std::move(questions)}}));
        }
        else {
            result.set("request", json_util::object({{"id", Json(id)},
                {"sessionId", Json("session-permission")}, {"agentId", Json(agent)},
                {"turnId", Json(7)}, {"toolCallId", Json("call_native_interaction")},
                {"toolName", Json("Shell")}, {"action", Json("execute")},
                {"display", json_util::object({{"kind", Json("shell")}, {"command", Json("private-command")}})}}));
        }
    }
    return result;
}

void append_wire_bytes(const fs::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(output.good(), "native record bytes must be completely written");
}

std::string irrelevant_tools_snapshot(std::size_t description_size, std::size_t depth = 0)
{
    // Native llm.tools_snapshot descriptions and parameter values have no
    // provider-reader DOM bound. Put the escaped discriminator last, after
    // payload text containing strings that must never classify a record.
    return R"({"agentId":"main","tools":[{"name":"probe","description":")" +
        std::string(description_size, 'x') +
        R"( interaction.request \"type\":\"interaction.resolved\" \\ } ] ","parameters":{"nested":)" +
        std::string(depth, '[') + "0" + std::string(depth, ']') +
        R"(}}],"hash":"tools","time":1789200000500,"\u0074ype":"llm.tools_\u0073napshot"})";
}

void interaction_record_isolation()
{
    Workspace workspace;
    const auto agents = workspace.m_path / "sessions/workspace/session-permission/agents";
    const auto main_wire = agents / "main/wire.jsonl";
    const auto child_wire = agents / "reviewer/wire.jsonl";
    append_wire(main_wire, Json::object());
    append_wire(child_wire, Json::object());
    kimi::Interaction_reader reader("session-permission",
        kimi::capture_interaction_frontier(addon::path_to_utf8(workspace.m_path), "session-permission"));
    const auto inventory = [&] {
        return kimi::discover_interaction_wires(addon::path_to_utf8(workspace.m_path), "session-permission");
    };
    std::vector<addon::Lifecycle_observation> observations;
    const auto acquire = [&] {
        require(reader.acquire(inventory(), observations, [] { return false; }),
            "an irrelevant partial record must not make wire acquisition fail");
    };
    append_wire(main_wire, interaction("question_before_large", "main"));
    append_wire(child_wire, interaction("question_interleaved", "reviewer"));
    const auto large = irrelevant_tools_snapshot(4 * 1024 * 1024 + 73);
    const auto split = large.find("\\u0074ype") + 3;
    require(split > 4 * 1024 * 1024, "type-last fixture must exceed the raw record bound");
    append_wire_bytes(main_wire, std::string_view(large).substr(0, 65535));
    acquire();
    require(observations.size() == 2, "requests before an unfinished irrelevant record must survive");
    append_wire_bytes(main_wire, std::string_view(large).substr(65535, split - 65535));
    append_wire(child_wire, interaction("question_interleaved", "reviewer", true));
    acquire();
    require(observations.size() == 3 && observations.back().agent_id == "reviewer" &&
        observations.back().attention_transition == addon::Attention_transition::RESOLVED,
        "another wire must progress while a giant type-last record is unfinished");
    append_wire_bytes(main_wire, std::string_view(large).substr(split));
    append_wire_bytes(main_wire, "\r\n" + irrelevant_tools_snapshot(19, 80) + "\n");
    append_wire(main_wire, interaction("question_before_large", "main", true));
    acquire();
    require(observations.size() == 4 && observations.back().attention_request_id == "question_before_large" &&
        observations.back().attention_transition == addon::Attention_transition::RESOLVED,
        "oversized and deep irrelevant records must preserve adjacent request/resolution order");
    acquire();
    require(observations.size() == 4, "physical offsets must not replay the skipped record or accepted attention");
    auto escaped_request = addon::serialize_json(interaction("question_after_large", "main"));
    escaped_request.replace(escaped_request.find("\"type\""), 6, R"("\u0074ype")");
    escaped_request.replace(escaped_request.find("interaction.request"), std::string("interaction.request").size(),
        R"(interaction.\u0072equest)");
    append_wire_bytes(main_wire, escaped_request + '\n');
    acquire();
    require(observations.size() == 5 && observations.back().attention_request_id == "question_after_large",
        "escaped required types after ignored bytes retain their original physical position and semantics");

    // Rewrite only ignored payload inside the actual last-4096-byte anchor.
    // A filtered/re-encoded anchor would miss this same-size history change.
    const auto size = fs::file_size(main_wire);
    {
        std::fstream rewrite(main_wire, std::ios::binary | std::ios::in | std::ios::out);
        rewrite.seekp(static_cast<std::streamoff>(size - 3000));
        rewrite.put('y');
        rewrite.close();
        require(rewrite.good(), "ignored raw payload rewrite must complete");
    }
    require(fs::file_size(main_wire) == size, "history rewrite preserves the physical file size");
    bool refused = false;
    try { acquire(); }
    catch (const std::runtime_error& error) { refused = std::string_view(error.what()) == "interaction_wire_changed"; }
    require(refused && observations.size() == 5,
        "ignored bytes remain in the raw anchor and cannot establish current-process attention after rewrite");
}

void interaction_required_records_remain_strict()
{
    auto oversized = addon::serialize_json(interaction("question_bad", "main"));
    oversized.pop_back();
    oversized += ",\"extra\":\"" + std::string(4 * 1024 * 1024, 'x') + "\"}";
    auto deep = addon::serialize_json(interaction("question_bad", "main"));
    deep.pop_back();
    deep += ",\"extra\":" + std::string(40, '[') + "0" + std::string(40, ']') + "}";
    auto duplicate_after_large = irrelevant_tools_snapshot(4 * 1024 * 1024 + 1);
    duplicate_after_large.pop_back();
    duplicate_after_large += R"(,"type":"interaction.request"})";
    const auto oversized_type_last = "{\"extra\":\"" + std::string(4 * 1024 * 1024, 'x') +
        R"(","type":"interaction.request","agentId":"main","id":"question_bad","kind":"question","toolCallId":"call","request":{}})";
    const std::vector<std::pair<std::string, std::string>> cases{
        {R"({"type":"interaction.request","id":"question_bad")", "invalid_interaction_wire_record"},
        {R"({"type":"llm.tools_snapshot","type":"interaction.request"})", "invalid_interaction_wire_record"},
        {R"({"type":"interaction.request","\u0074ype":"llm.tools_snapshot"})", "invalid_interaction_wire_record"},
        {R"({"type":"interaction.request","request":{"x":1,"x":2}})", "invalid_interaction_wire_record"},
        {R"({"type":"interaction.request","id":"\uD800"})", "invalid_interaction_wire_record"},
        {R"({"type":"interaction.request","time":1e+})", "invalid_interaction_wire_record"},
        {std::move(deep), "invalid_interaction_wire_record"},
        {std::move(duplicate_after_large), "invalid_interaction_wire_record"},
        {std::move(oversized), "interaction_record_capacity_exhausted"},
        {oversized_type_last, "interaction_record_capacity_exhausted"},
    };
    for (const auto& [record, expected] : cases) {
        Workspace workspace;
        const auto wire = workspace.m_path / "sessions/workspace/session-permission/agents/main/wire.jsonl";
        append_wire(wire, Json::object());
        kimi::Interaction_reader reader("session-permission",
            kimi::capture_interaction_frontier(addon::path_to_utf8(workspace.m_path), "session-permission"));
        append_wire(wire, interaction("question_accepted", "main"));
        append_wire_bytes(wire, record + '\n');
        append_wire(wire, interaction("question_accepted", "main", true));
        std::vector<addon::Lifecycle_observation> observations;
        std::string failure;
        try {
            reader.acquire(kimi::discover_interaction_wires(addon::path_to_utf8(workspace.m_path),
                "session-permission"), observations, [] { return false; });
        }
        catch (const std::runtime_error& error) { failure = error.what(); }
        require(failure == expected && observations.size() == 1 &&
            observations.front().attention_request_id == "question_accepted",
            "malformed/oversized required records remain strict without losing previously accepted attention: " + failure);
    }
}

std::string observe(addon::Host_simulator& host, const fs::path& home, const std::string& nonce)
{
    addon::Observe_request request;
    request.session = {addon::path_to_utf8(home), {"kimi"}, "session-permission"};
    request.correlation = addon::Session_correlation{nonce};
    request.window_ms = addon::limits::k_observe_window_ms;
    return host.invoke({"lifecycle-events", 2}, "observe", addon::encode_observe_request(request), 25000);
}

addon::Poll_observations_result poll_attention(addon::Host_simulator& host, const std::string& nonce)
{
    addon::Poll_observations_request request;
    request.correlation = addon::Session_correlation{nonce};
    request.limit = addon::limits::k_max_observations_per_result;
    const auto payload = invoke(host, {"lifecycle-events", 2}, "poll_observations",
        addon::encode_poll_observations_request(request));
    addon::Contract_failure failure;
    const auto result = addon::decode_poll_observations_result(payload, request.limit, &failure);
    require(result.has_value(), "continuous interaction result must obey the host schema");
    return *result;
}

void wait_for_progress(addon::Host_simulator& host, const std::string& window, std::uint64_t previous)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (host.progress_count(window) <= previous && std::chrono::steady_clock::now() < deadline) {
        host.pump(20);
        require(!host.outcome(window), "interaction observation stopped before acquiring native evidence");
    }
    require(host.progress_count(window) > previous,
        "native interaction wire did not publish its pending question");
}

void native_interactions(const fs::path& package, const fs::path& executable)
{
    Workspace workspace;
    const auto agents = workspace.m_path / "sessions/workspace/session-permission/agents";
    const auto main_wire = agents / "main/wire.jsonl";
    const auto child_wire = agents / "reviewer/wire.jsonl";
    const auto partial_wire = agents / "partial/wire.jsonl";
    append_wire(main_wire, interaction("question_old_process", "main"));
    append_wire(child_wire, interaction("question_old_child", "reviewer"));
    const auto old_partial = addon::serialize_json(interaction("question_old_partial", "partial"));
    fs::create_directories(partial_wire.parent_path());
    {
        std::ofstream output(partial_wire, std::ios::binary);
        output << old_partial.substr(0, old_partial.size() / 2);
        require(output.good(), "old native partial record must reach the captured frontier");
    }

    addon::Simulator_options options;
    options.package_directory = package;
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), failure.reason_code + ": " + failure.detail);
    auto nonce = addon::random_nonce_hex();
    invoke(host, {addon::k_lifecycle_correlation_lifetime_capability_id, 1}, "reserve",
        addon::encode_lifecycle_correlation_reserve_request({{nonce}}));
    addon::Classify_event_request start;
    start.transition_nonce = nonce;
    start.event = relay(package, executable, workspace.m_path, host,
        json_util::object({{"hook_event_name", Json("SessionStart")},
            {"session_id", Json("session-permission")}, {"source", Json("resume")},
            {"interaction_frontier", Json("untrusted native input")}}));

    // The helper has returned, but adapter admission has not run. A native first
    // prompt in this interval must remain after the helper's frozen frontier.
    {
        std::ofstream output(partial_wire, std::ios::binary | std::ios::app);
        output << old_partial.substr(old_partial.size() / 2) << '\n';
        require(output.good(), "the pre-launch partial record must complete after capture");
    }
    append_wire(main_wire, interaction("question_first_prompt", "main"));
    append_wire(child_wire, interaction("question_background", "reviewer"));
    require(classify(host, start).base.classification == addon::Classification::TRANSITION,
        "admit the captured native launch before observing its wires");
    auto window = observe(host, workspace.m_path, nonce);
    wait_for_progress(host, window, 0);
    auto page = poll_attention(host, nonce);
    require(page.observations.size() == 2, "only current-process root and child questions are pending");
    std::map<std::string, std::string> pending;
    for (const auto& observation : page.observations) {
        require(observation.attention_transition == addon::Attention_transition::REQUESTED &&
            observation.attention_evidence == addon::Evidence::CONFIRMED &&
            observation.provider_turn_id.empty() && observation.phase_transition == addon::Phase_transition::NONE,
            "native interaction requests must retain exact attention without inventing a root phase");
        pending.emplace(observation.attention_request_id, observation.agent_id);
    }
    require(pending == std::map<std::string, std::string>{{"question_first_prompt", "main"},
        {"question_background", "reviewer"}}, "old process interactions must not be replayed");
    require(json_util::member(start.event.body, "interaction_frontier").is_object(),
        "the helper must replace native input with its own admission frontier");
    const auto frontier = addon::serialize_json(json_util::member(start.event.body, "interaction_frontier"));
    require(frontier.find(addon::path_to_utf8(workspace.m_path)) == std::string::npos &&
        frontier.find("private-question") == std::string::npos,
        "frontier metadata must not contain paths or native question text");

    auto progress = host.progress_count(window);
    append_wire(child_wire, json_util::object({{"type", Json("context.append_loop_event")},
        {"event", json_util::object({{"type", Json("tool.result")},
            {"toolCallId", Json("call_native_interaction")}, {"result", json_util::object({
                {"output", Json("task_id: background-question\nstatus: running\n")}})}})}}));
    append_wire(main_wire, interaction("question_background", "reviewer", true));
    append_wire(agents / "new-child/wire.jsonl", interaction("approval_cancelled", "new-child", false, "approval"));
    wait_for_progress(host, window, progress);
    page = poll_attention(host, nonce);
    require(page.observations.size() == 3 &&
        page.observations.back().attention_request_id == "approval_cancelled",
        "a new child wire must be acquired without clearing either existing question");

    progress = host.progress_count(window);
    append_wire(agents / "new-child/wire.jsonl", interaction("approval_cancelled", "new-child", true));
    wait_for_progress(host, window, progress);
    page = poll_attention(host, nonce);
    require(page.observations.size() == 4 &&
        page.observations.back().attention_request_id == "approval_cancelled" &&
        page.observations.back().agent_id == "new-child" &&
        page.observations.back().attention_transition == addon::Attention_transition::RESOLVED,
        "native cancellation must settle only its exact child approval");

    progress = host.progress_count(window);
    auto answered = interaction("question_background", "reviewer", true);
    answered.set("response", json_util::object({{"answers", json_util::object({
        {"private-question", Json("Continue")}})}, {"method", Json("enter")}}));
    append_wire(child_wire, answered);
    wait_for_progress(host, window, progress);
    page = poll_attention(host, nonce);
    require(page.observations.size() == 5 &&
        page.observations.back().attention_request_id == "question_background" &&
        page.observations.back().attention_transition == addon::Attention_transition::RESOLVED,
        "background question resolution must wait for its native interaction response");
    require(addon::serialize_json(addon::encode_poll_observations_result(page)).find("private-question") ==
        std::string::npos, "continuous attention must not expose question or answer content");

    host.cancel(window);
    require(host.wait_outcome(window, 5000).has_value(), "stop the observation window before retiring its scope");
    invoke(host, {addon::k_lifecycle_correlation_lifetime_capability_id, 1}, "retire",
        addon::encode_lifecycle_correlation_retire_request({{nonce},
            addon::Lifecycle_correlation_retire_disposition::ABANDONED, std::nullopt}));
    nonce = addon::random_nonce_hex();
    invoke(host, {addon::k_lifecycle_correlation_lifetime_capability_id, 1}, "reserve",
        addon::encode_lifecycle_correlation_reserve_request({{nonce}}));
    start.transition_nonce = nonce;
    start.event = relay(package, executable, workspace.m_path, host,
        json_util::object({{"hook_event_name", Json("SessionStart")},
            {"session_id", Json("session-permission")}, {"source", Json("resume")}}));
    classify(host, start);
    append_wire(main_wire, interaction("question_next_process", "main"));
    window = observe(host, workspace.m_path, nonce);
    wait_for_progress(host, window, 0);
    page = poll_attention(host, nonce);
    require(page.observations.size() == 1 &&
        page.observations.front().attention_request_id == "question_next_process",
        "a new native launch must not revive an unresolved question from the retired process");
    host.cancel(window);
    require(host.wait_outcome(window, 5000).has_value(), "stop the second observation window");
    int exit_code = -1;
    require(host.shutdown(5000, &exit_code) && exit_code == 0, "interaction adapter must stop cleanly");
}

void interaction_acquisition_retries_missing_wire()
{
    Workspace workspace;
    const auto agents = workspace.m_path / "sessions/workspace/session-permission/agents";
    const auto main_wire   = agents / "main/wire.jsonl";
    const auto child_wire  = agents / "reviewer/wire.jsonl";
    const auto writer_wire = agents / "writer/wire.jsonl";
    append_wire(main_wire, Json::object());
    append_wire(child_wire, Json::object());
    append_wire(writer_wire, Json::object());
    kimi::Interaction_reader reader("session-permission",
        kimi::capture_interaction_frontier(addon::path_to_utf8(workspace.m_path), "session-permission"));
    const kimi::Interaction_inventory inventory{fs::canonical(agents), {
        {"main", main_wire, true}, {"reviewer", child_wire, true}, {"writer", writer_wire, true}}};
    append_wire(main_wire, interaction("question_main_during_acquisition", "main"));
    append_wire(child_wire, interaction("question_child_during_acquisition", "reviewer"));
    append_wire(writer_wire, interaction("question_writer_during_acquisition", "writer"));

    // Discovery and opening are separate operations. A temporary disappearance
    // must preserve earlier events and let available later wires make progress.
    const auto parked_wire = agents / "reviewer/parked.jsonl";
    fs::rename(child_wire, parked_wire);
    std::vector<addon::Lifecycle_observation> observations;
    require(!reader.acquire(inventory, observations, [] { return false; }),
        "a wire unavailable after discovery must leave acquisition incomplete");
    require(observations.size() == 2 &&
        observations.front().attention_request_id == "question_main_during_acquisition" &&
        observations.back().attention_request_id == "question_writer_during_acquisition",
        "an unavailable child wire must preserve earlier questions and acquire later wires");

    append_wire(writer_wire, interaction("question_writer_during_retry", "writer"));
    observations.clear();
    require(!reader.acquire(inventory, observations, [] { return false; }) &&
        observations.size() == 1 &&
        observations.front().attention_request_id == "question_writer_during_retry",
        "repeated unavailable opens must not starve later wires or replay earlier questions");

    fs::rename(parked_wire, child_wire);
    observations.clear();
    require(reader.acquire(inventory, observations, [] { return false; }),
        "restoring the same wire must allow acquisition to resume");
    require(observations.size() == 1 &&
        observations.front().attention_request_id == "question_child_during_acquisition",
        "resumed acquisition must deliver the child question without replaying the main question");

    append_wire(main_wire, interaction("question_main_during_acquisition", "main", true));
    append_wire(child_wire, interaction("question_child_during_acquisition", "reviewer", true));
    append_wire(writer_wire, interaction("question_writer_during_acquisition", "writer", true));
    append_wire(writer_wire, interaction("question_writer_during_retry", "writer", true));
    observations.clear();
    require(reader.acquire(inventory, observations, [] { return false; }) && observations.size() == 4,
        "all native resolutions must remain observable after acquisition recovers");
    for (const auto& observation : observations) {
        require(observation.attention_transition == addon::Attention_transition::RESOLVED,
            "recovered acquisition must preserve exact native retirement");
    }
}

void frontier_failures_are_explicit()
{
    Workspace workspace;
    const auto agents = workspace.m_path / "sessions/workspace/session-permission/agents";
    fs::create_directories(agents / "main/wire.jsonl");
    bool refused = false;
    try {
        kimi::capture_interaction_frontier(addon::path_to_utf8(workspace.m_path), "session-permission");
    }
    catch (const std::runtime_error& error) {
        refused = std::string_view(error.what()) == "interaction_wire_unavailable";
    }
    require(refused, "an unreadable native wire must not silently become a zero history frontier");
    fs::remove(agents / "main/wire.jsonl");
    for (int index = 0; index < 256; ++index) {
        fs::create_directories(agents / ("child-" + std::to_string(index)));
    }
    refused = false;
    try {
        kimi::capture_interaction_frontier(addon::path_to_utf8(workspace.m_path), "session-permission");
    }
    catch (const std::runtime_error& error) {
        refused = std::string_view(error.what()) == "interaction_inventory_capacity_exhausted";
    }
    require(refused, "oversized native agent inventory must fail before historical prompts can be replayed");
}

void investigate_interaction_records()
{
    // Kimi's llm.tools_snapshot schema permits arbitrary descriptions and
    // parameter values. Report these constructed extremes without treating
    // rejection as the desired contract or claiming an observed native log.
    const std::string prefix = R"({"type":"llm.tools_snapshot","agentId":"main","hash":"probe-tools","tools":[{"name":"probe","description":")";
    const std::string suffix = R"(","parameters":{}}],"time":1789200000500})";
    const std::string large  = prefix + std::string(4 * 1024 * 1024 + 1, 'x') +
        " interaction.request" + suffix;
    const std::string deep = R"({"agentId":"main","hash":"probe-tools","tools":[{"name":"probe","description":"ordinary","parameters":{"nested":)" +
        std::string(40, '[') + "0" + std::string(40, ']') +
        R"(}}],"time":1789200000500,"type":"llm.tools_snapshot"})";
    const std::pair<std::string, std::string> cases[] = {
        {"small_irrelevant",     prefix + "ordinary description" + suffix},
        {"oversized_irrelevant", large},
        {"deep_irrelevant",      deep},
    };
    for (const auto& [name, record] : cases) {
        addon::Json_parse_limits fixture_limits;
        fixture_limits.max_bytes        = record.size();
        fixture_limits.max_string_bytes = record.size();
        fixture_limits.max_depth        = 64;
        require(addon::parse_json(record, fixture_limits, nullptr).has_value(),
            "the diagnostic input must be valid JSON outside the consumed parser bounds");
        Workspace workspace;
        const auto wire = workspace.m_path / "sessions/workspace/session-permission/agents/main/wire.jsonl";
        append_wire(wire, Json::object());
        kimi::Interaction_reader reader("session-permission",
            kimi::capture_interaction_frontier(addon::path_to_utf8(workspace.m_path), "session-permission"));
        append_wire(wire, interaction("question_diagnostic", "main"));
        {
            std::ofstream output(wire, std::ios::binary | std::ios::app);
            output << record << '\n';
            output.close();
            require(output.good(), "diagnostic payload must be completely written");
        }
        append_wire(wire, interaction("question_diagnostic", "main", true));
        std::vector<addon::Lifecycle_observation> observations;
        std::string outcome;
        try {
            const auto inventory = kimi::discover_interaction_wires(
                addon::path_to_utf8(workspace.m_path), "session-permission");
            outcome = reader.acquire(inventory, observations, [] { return false; })
                ? "complete" : "incomplete";
        }
        catch (const std::runtime_error& error) {
            outcome = error.what();
        }
        std::cout << name << ": " << outcome << "; observations=" << observations.size() << '\n';
    }
}

void interaction_provenance_and_required_records_remain_strict()
{
    const std::string records[] = {
        R"({"type":"interaction.request","id":"question_malformed")",
        R"({"type":"context.append_loop_event","type":"interaction.request"})",
    };
    for (const auto& record : records) {
        Workspace workspace;
        const auto wire = workspace.m_path / "sessions/workspace/session-permission/agents/main/wire.jsonl";
        append_wire(wire, Json::object());
        kimi::Interaction_reader reader("session-permission",
            kimi::capture_interaction_frontier(addon::path_to_utf8(workspace.m_path), "session-permission"));
        {
            std::ofstream output(wire, std::ios::binary | std::ios::app);
            output << record << '\n';
            output.close();
            require(output.good(), "malformed required record fixture must be written");
        }
        std::vector<addon::Lifecycle_observation> observations;
        bool refused = false;
        try {
            reader.acquire(kimi::discover_interaction_wires(addon::path_to_utf8(workspace.m_path),
                "session-permission"), observations, [] { return false; });
        }
        catch (const std::runtime_error& error) {
            refused = std::string_view(error.what()) == "invalid_interaction_wire_record";
        }
        require(refused && observations.empty(), "malformed or ambiguous interaction records remain explicit failures");
    }

    Workspace workspace;
    const auto wire = workspace.m_path / "sessions/workspace/session-permission/agents/main/wire.jsonl";
    append_wire(wire, interaction("question_before", "main"));
    kimi::Interaction_reader reader("session-permission",
        kimi::capture_interaction_frontier(addon::path_to_utf8(workspace.m_path), "session-permission"));
    const auto previous_size = fs::file_size(wire);
    {
        std::ofstream output(wire, std::ios::binary);
        output << addon::serialize_json(interaction("question_after_", "main")) << '\n';
        output.close();
        require(output.good(), "changed history fixture must be written");
    }
    require(fs::file_size(wire) == previous_size, "history replacement must preserve the captured file size");
    std::vector<addon::Lifecycle_observation> observations;
    bool refused = false;
    try {
        reader.acquire(kimi::discover_interaction_wires(addon::path_to_utf8(workspace.m_path),
            "session-permission"), observations, [] { return false; });
    }
    catch (const std::runtime_error& error) {
        refused = std::string_view(error.what()) == "interaction_wire_changed";
    }
    require(refused && observations.empty(), "same-size rewritten history cannot establish current-process attention");
}

void frontier_failure_preserves_admission(const fs::path& package, const fs::path& executable, bool oversized)
{
    Workspace workspace;
    const auto agents = workspace.m_path / "sessions/workspace/session-permission/agents";
    const auto main_wire = agents / "main/wire.jsonl";
    append_wire(agents / "reviewer/wire.jsonl", interaction("question_historical_child", "reviewer"));
    if (oversized) {
        append_wire(main_wire, interaction("question_historical_main", "main"));
        for (int index = 0; index < 255; ++index) {
            fs::create_directories(agents / ("child-" + std::to_string(index)));
        }
    }
    else {
        fs::create_directories(main_wire);
    }

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
    std::string diagnostic;
    request.event = relay(package, executable, workspace.m_path, host,
        json_util::object({{"hook_event_name", Json("SessionStart")},
            {"session_id", Json("session-permission")}, {"source", Json("resume")},
            {"interaction_frontier", Json("untrusted native input")}}), &diagnostic);
    const auto* frontier = request.event.body.find("interaction_frontier");
    require(frontier && frontier->is_null(),
        "failed optional capture must explicitly disable interaction observation for this admission");
    require(diagnostic == "Logonomic native interaction observation is unavailable for this session.\n",
        "optional capture failure must report a bounded diagnostic without paths or native data");
    require(classify(host, request).base.classification == addon::Classification::TRANSITION,
        "failed optional capture must preserve authoritative SessionStart admission");
    request.ledger = {1, addon::Ledger_transition{addon::Transition_kind::RESUME, "session-permission", nonce}};
    for (const auto* event : {"PermissionRequest", "PermissionResult"}) {
        request.event = relay(package, executable, workspace.m_path, host,
            native_permission(event, "approval_degraded_reader", "reviewer"));
        const auto result = classify(host, request);
        require(result.base.classification == addon::Classification::EVENT &&
            result.base.observations.size() == 1 && result.attention_kind && result.attention_kind->kind == "permission",
            "optional interaction failure must preserve the published permission classification");
        const auto& observation = result.base.observations.front();
        require(observation.attention_request_id == "approval_degraded_reader" && observation.agent_id == "reviewer" &&
            observation.attention_transition == (std::string_view(event) == "PermissionRequest"
                ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED),
            "degraded interaction admission must retain the exact native permission pair");
    }

    // Repairing the inventory cannot retroactively establish a launch frontier.
    if (oversized) {
        fs::remove(agents / "child-254");
    }
    else {
        fs::remove(main_wire);
    }
    append_wire(main_wire, interaction("question_after_failed_capture", "main"));
    append_wire(main_wire, json_util::object({{"type", Json("subagent.started")},
        {"subagentId", Json("existing-lifecycle")}}));
    const auto window = observe(host, workspace.m_path, nonce);
    wait_for_progress(host, window, 0);
    const auto page = poll_attention(host, nonce);
    require(page.observations.size() == 1 && page.observations.front().agent_id == "existing-lifecycle" &&
        page.observations.front().subagent_transition == addon::Subagent_transition::STARTED &&
        page.observations.front().attention_transition == addon::Attention_transition::NONE,
        "existing lifecycle observation must continue without replaying interaction history after failed capture");
    host.cancel(window);
    require(host.wait_outcome(window, 5000).has_value(), "stop observation after degraded admission");
    int exit_code = -1;
    require(host.shutdown(5000, &exit_code) && exit_code == 0, "degraded interaction adapter must stop cleanly");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--investigate-records") {
            investigate_interaction_records();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--reader-isolation") {
            interaction_record_isolation();
            interaction_required_records_remain_strict();
            return 0;
        }
        if (argc > 2) {
            return capture_delivery(argc, argv);
        }
        require(argc == 2, "usage: kimi_permission_tests <package directory>");
        interaction_acquisition_retries_missing_wire();
        interaction_provenance_and_required_records_remain_strict();
        interaction_record_isolation();
        interaction_required_records_remain_strict();
        permission_pair(fs::absolute(addon::path_from_utf8(argv[1])), fs::absolute(addon::path_from_utf8(argv[0])));
        native_interactions(fs::absolute(addon::path_from_utf8(argv[1])), fs::absolute(addon::path_from_utf8(argv[0])));
        frontier_failures_are_explicit();
        frontier_failure_preserves_admission(fs::absolute(addon::path_from_utf8(argv[1])),
            fs::absolute(addon::path_from_utf8(argv[0])), false);
        frontier_failure_preserves_admission(fs::absolute(addon::path_from_utf8(argv[1])),
            fs::absolute(addon::path_from_utf8(argv[0])), true);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
