#include "kimi_accounts.h"
#include "kimi_json.h"
#include "kimi_preparation.h"

#include <logonomic_cli_addon_test/invocation_context.h>

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace json_util = addon::json;
namespace fs = std::filesystem;
using kimi::Json;

void require(bool condition, const std::string& description)
{
    if (!condition) {
        throw std::runtime_error(description);
    }
}

class Workspace
{
public:
    Workspace()
    :
        path(fs::temp_directory_path() / ("kimi-resume-history-" + addon::random_nonce_hex()))
    {
        fs::create_directories(path);
        path = fs::canonical(path);
    }

    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(addon::usage_cache_io_path(path), error);
    }

    fs::path path;
};

void write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(addon::usage_cache_io_path(path.parent_path()));
    std::ofstream output(addon::usage_cache_io_path(path), std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    require(output.good(), "native history fixture is written completely");
}

std::string read(const fs::path& path)
{
    std::ifstream input(addon::usage_cache_io_path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Native oracle: MoonshotAI/kimi-code eb2035f3cad57cc12d4d5082ec626f7f0ea5f651,
// sessionMetadataService.ts (new metadata), wire/record.ts (wire 1.5 header),
// agent/replayBuilder/fold.ts and its fold.test.ts (root user/model history).
// Logonomic provider issue #14 requires UNKNOWN for uncertain replay/writes.
constexpr std::string_view k_metadata =
    R"({"type":"metadata","protocol_version":"1.5","min_protocol_version":"1.5","created_at":1789200000000})" "\n";
constexpr std::string_view k_user =
    R"({"type":"context.append_message","message":{"role":"user","content":[{"type":"text","text":"Remember this conversation"}],"toolCalls":[],"origin":{"kind":"user"}},"time":1789200000100})" "\n";
constexpr std::string_view k_model =
    R"({"type":"context.append_loop_event","event":{"type":"step.begin","uuid":"step-1"},"time":1789200000200})" "\n"
    R"({"type":"context.append_loop_event","event":{"type":"content.part","stepUuid":"step-1","part":{"type":"text","text":"Remembered"}},"time":1789200000300})" "\n"
    R"({"type":"context.append_loop_event","event":{"type":"step.end","uuid":"step-1"},"time":1789200000400})" "\n";

Json native_metadata(std::string_view id)
{
    return json_util::object({{"id", Json(std::string(id))}, {"version", Json(2)},
        {"createdAt", Json(std::int64_t{1789200000000})},
        {"updatedAt", Json(std::int64_t{1789200000000})}, {"archived", Json(false)},
        {"agents", Json::object()}, {"custom", Json::object()}});
}

Json scoped_record(const fs::path& home, std::string_view id)
{
    return json_util::object({{"provider_session_id", Json(std::string(id))},
        {"native_store", json_util::object({{"version", Json(1)},
            {"kimi_code_home", Json(addon::path_to_utf8(home))}})}});
}

addon::Resume_inspect_request inspection_request(const fs::path& home, const fs::path& workspace)
{
    addon::Resume_inspect_request request;
    request.context.launcher = {"kimi", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"kimi"};
    request.context.working_directory = addon::path_to_utf8(workspace);
    request.context.account_context = addon::Account_context{2, kimi::account_key(home), addon::path_to_utf8(home)};
    request.context.launch_environment = addon::Launch_environment{addon::host_platform_id(), Json::object(), {}};
    request.context.launch_environment->effective_environment.set("KIMI_CODE_HOME", Json(addon::path_to_utf8(home)));
    request.resume_record = scoped_record(home, "session-a");
    return request;
}

Json inspect(const addon::Resume_inspect_request& request, std::uint16_t version,
    addon::Host_simulator* host)
{
    if (host) {
        const auto invocation = host->invoke({"interactive-resume", version}, "inspect_resume",
            addon::encode_resume_inspect_request(request), 5000);
        const auto outcome = host->wait_outcome(invocation, 6000);
        require(outcome && outcome->status == addon::Host_status::SUCCESS,
            "packaged adapter admits negotiated resume inspection");
        return outcome->payload;
    }
    addon::test::Throwing_invocation_context context;
    context.m_request = {"inspect", {"interactive-resume", version}, "inspect_resume",
        addon::encode_resume_inspect_request(request)};
    kimi::handle_preparation(context);
    require(context.m_result.has_value(), "resume inspection returns a result");
    return *context.m_result;
}

addon::Resume_inspect_result expect(const addon::Resume_inspect_request& request,
    addon::Resume_availability availability, addon::Resume_content content,
    addon::Host_simulator* host, const std::string& description, std::uint16_t version = 4)
{
    const auto payload = inspect(request, version, host);
    addon::Contract_failure failure;
    const auto result = addon::decode_resume_inspect_result(payload, &failure, version);
    require(result && addon::validate_resume_inspection(request, *result, &failure),
        description + ": exact request acknowledgment");
    require(result->availability == availability && result->content == content,
        description + ": availability/content classification");
    require((payload.find("content") != nullptr) == (version == 4),
        description + ": content is emitted only for negotiated version 4");
    return *result;
}

void native_initialization_contract(const fs::path& fixture, addon::Host_simulator* host)
{
    using Availability = addon::Resume_availability;
    using Content = addon::Resume_content;
    const auto home = fixture / "initialized-profile";
    const auto session = home / "sessions/wd_fixture/session-a";
    auto metadata = native_metadata("session-a");
    metadata.set("agents", json_util::object({{"main", json_util::object({{"type", Json("main")}})}}));
    write(session / "state.json", addon::serialize_json(metadata));
    const auto wire = session / "agents/main/wire.jsonl";
    const auto request = inspection_request(home, fixture);
    // Scrubbed five-record initializer captured from real Kimi Code 2.5.2
    // /new and /clear without model requests. The corresponding native
    // environmentBindingOps, profileOps and changeNotifierOps reducers only
    // persist configuration; none adds retained conversation messages.
    const std::string initialized =
        R"({"type":"metadata","protocol_version":"1.5","created_at":1789200000000})" "\n"
        R"({"type":"environment.set_binding","environmentId":"local","agentId":"main","time":1789200000001})" "\n"
        R"({"type":"profile.bind","agentId":"main","modelAlias":"offline-check","profileName":"agent","thinkingEffort":"off","systemPrompt":"Native system instructions","environmentDisclosure":{"cwd":"/fixture"},"agentsMdPaths":[],"activeToolNames":["Read","Bash"],"disallowedTools":[],"subagents":["explore"],"time":1789200000002})" "\n"
        R"({"type":"changeNotifier.snapshot","agentId":"main","agentsMdHash":null,"agentsMdFiles":{},"skillsHash":"fixture-skills","subagentNames":["explore"],"modelPool":null,"workspacePrimaryByWorkspace":{"wd_fixture":"/fixture"},"workspaceAdditionalDirs":[],"time":1789200000003})" "\n"
        R"({"type":"permission.set_mode","agentId":"main","mode":"manual","time":1789200000004})" "\n";
    write(wire, initialized);
    expect(request, Availability::AVAILABLE, Content::EMPTY, host, "actual native empty initialization shape");
    write(wire, initialized + std::string(k_user) + std::string(k_model));
    expect(request, Availability::AVAILABLE, Content::NONEMPTY, host, "actual initializer followed by retained conversation");
    for (const auto* malformed : {
            R"({"type":"environment.set_binding","environmentId":false,"agentId":"main"})",
            R"({"type":"profile.bind","agentId":"main","thinkingEffort":"off","systemPrompt":false,"disallowedTools":[]})",
            R"({"type":"changeNotifier.snapshot","agentId":"main","agentsMdHash":null,"skillsHash":null,"subagentNames":"explore"})"})
    {
        write(wire, std::string(k_metadata) + malformed + '\n');
        expect(request, Availability::AVAILABLE, Content::UNKNOWN, host,
            "malformed native initialization cannot prove emptiness");
    }
}

void native_completed_contract(const fs::path& fixture, addon::Host_simulator* host)
{
    using Availability = addon::Resume_availability;
    using Content = addon::Resume_content;
    const auto captured = fs::path(__FILE__).parent_path() / "fixtures/native-completed";
    const auto metadata = read(captured / "state.json");
    const auto completed = read(captured / "wire.jsonl");
    require(!metadata.empty() && !completed.empty(), "captured native completion fixture is present");
    const auto home = fixture / "completed-profile";
    const auto session = home / "sessions/wd_fixture/session-a";
    const auto wire = session / "agents/main/wire.jsonl";
    write(session / "state.json", metadata);
    write(wire, completed);
    const auto request = inspection_request(home, fixture);
    // Captured from official Kimi Code 2.5.2 with an offline model response.
    // Preserve all 23 record types, order, prompt/step references and completion
    // boundaries; scrub paths, system/date prose and tool descriptions/schemas.
    // Native oracle at eb2035f3: replayBuilder/fold.ts, human/agent/historySchema.ts,
    // loop/turnOps.ts, loop/llmRequestOps.ts, plugin/agentPluginOps.ts,
    // toolSelect/toolSelectOps.ts, tokenCounting/tokenCountingOps.ts,
    // prompt/promptEvents.ts and features/dateChange/dateChangeService.ts.
    expect(request, Availability::AVAILABLE, Content::NONEMPTY, host,
        "actual native completed user and assistant conversation");
    require(read(wire) == completed && read(session / "state.json") == metadata,
        "completed native conversation remains unchanged by inspection");
    std::vector<Json> records;
    std::size_t begin = 0;
    while (begin < completed.size()) {
        const auto end = completed.find('\n', begin);
        const auto record = addon::parse_json(std::string_view(completed).substr(begin, end - begin),
            addon::Json_parse_limits{}, nullptr);
        require(record.has_value(), "captured native record is valid JSON");
        records.push_back(*record);
        begin = end + 1;
    }
    require(records.size() == 23, "complete native fixture retains every captured record");
    struct Uncertain_record
    {
        const char* type;
        const char* field;
        Json replacement;
    };
    for (const auto& uncertain : std::vector<Uncertain_record>{
            {"agent.message.appended", "message", Json(false)},
            {"agent.turn.started", "turnId", Json("0")},
            {"plugin.session_start", "content", Json(false)},
            {"tool_select.initialized", "tools", Json::object()},
            {"llm.tools_snapshot", "hash", Json(12)},
            {"llm.request", "kind", Json("compaction")},
            {"agent.turn.ended", "outcome", Json("future")},
            {"turn.ended", "reason", Json("future")},
            {"token_counting.turn_recorded", "tokens", Json("95")},
            {"prompt.completed", "promptId", Json("")}})
    {
        bool replaced = false;
        std::string changed;
        for (auto record : records) {
            if (!replaced && json_util::text(record, "type") == uncertain.type) {
                record.set(uncertain.field, uncertain.replacement);
                replaced = true;
            }
            changed += addon::serialize_json(record) + '\n';
        }
        require(replaced, "native completion contains the record under test");
        write(wire, changed);
        expect(request, Availability::AVAILABLE, Content::UNKNOWN, host,
            "malformed or replay-dependent completion record remains uncertain");
    }
    std::string without_context;
    for (const auto& record : records) {
        const auto type = json_util::text(record, "type");
        if (type != "context.append_message" && type != "context.append_loop_event") {
            without_context += addon::serialize_json(record) + '\n';
        }
    }
    write(session / "state.json", addon::serialize_json(native_metadata("session-a")));
    write(wire, without_context);
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host,
        "mirrored UI messages and completion counters alone do not prove retained content or emptiness");
    write(session / "state.json", metadata);
    write(wire, completed + R"({"type":"context.undo","count":1})" "\n");
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host,
        "completed native history followed by replay mutation remains uncertain");
    write(wire, completed.substr(0, completed.size() - 1));
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host,
        "incomplete native completion write remains uncertain");
}

void long_native_history_contract(const fs::path& fixture, addon::Host_simulator* host)
{
#ifdef _WIN32
    using Availability = addon::Resume_availability;
    using Content = addon::Resume_content;
    auto home = fixture / "retained-profile";
    while (home.native().size() < 280) {
        home /= "working-directory-history";
    }
    const auto session = home / "sessions/wd_project_hash/session-a";
    const auto state = session / "state.json";
    const auto wire = session / "agents/main/wire.jsonl";
    const auto metadata = addon::serialize_json(native_metadata("session-a"));
    const auto history = std::string(k_metadata) + std::string(k_user) + std::string(k_model);
    write(state, metadata);
    write(wire, history);
    require(fs::is_regular_file(addon::usage_cache_io_path(state)) &&
        fs::is_regular_file(addon::usage_cache_io_path(wire)),
        "long-path native history is present before inspection");
    auto request = inspection_request(home, fixture);
    expect(request, Availability::AVAILABLE, Content::NONEMPTY, host,
        "native history beyond Windows MAX_PATH remains available");
    require(read(state) == metadata && read(wire) == history,
        "long-path inspection preserves native history bytes");
    request.resume_record = scoped_record(home, "absent-session");
    expect(request, Availability::ABSENT, Content::UNKNOWN, host,
        "a complete long-path scan can still prove native absence");
#else
    (void)fixture;
    (void)host;
#endif
}

void history_contract(const fs::path& fixture, addon::Host_simulator* host)
{
    long_native_history_contract(fixture, host);
    native_completed_contract(fixture, host);
    native_initialization_contract(fixture, host);
    using Availability = addon::Resume_availability;
    using Content = addon::Resume_content;
    const auto home = fixture / "native-home";
    const auto session = home / "sessions/wd_project_hash/session-a";
    const auto state = session / "state.json";
    const auto wire = session / "agents/main/wire.jsonl";
    auto request = inspection_request(home, fixture);
    auto metadata = native_metadata("session-a");
    write(state, addon::serialize_json(metadata));
    const auto header = std::string(k_metadata);
    const auto user = std::string(k_user);
    const auto model = std::string(k_model);

    write(wire, header + user + model);
    const auto original_state = read(state);
    const auto original_wire = read(wire);
    const auto nonempty = expect(request, Availability::AVAILABLE, Content::NONEMPTY, host,
        "durable root user/model conversation");
    require(read(state) == original_state && read(wire) == original_wire,
        "inspection never mutates native metadata or history");
    auto other_request = request;
    other_request.context.working_directory += "/nested";
    addon::Contract_failure failure;
    require(!addon::validate_resume_inspection(other_request, nonempty, &failure),
        "a result cannot be reused for another resolved request");
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host, "version 3 remains unchanged", 3);

    write(wire, header);
    expect(request, Availability::AVAILABLE, Content::EMPTY, host, "recognized metadata-only root");
    write(wire, header + R"({"type":"config.update","modelAlias":"test"})" "\n" +
        R"({"type":"permission.set_mode","mode":"manual"})" "\n" +
        R"({"type":"context.append_message","message":{"role":"user","content":[{"type":"text","text":"Native project context"}],"toolCalls":[],"origin":{"kind":"injection","variant":"project_context"}}})" "\n");
    expect(request, Availability::AVAILABLE, Content::EMPTY, host, "initial configuration and injected context");
    for (const auto* origin : {
            R"({"kind":"injection"})", R"({"kind":"injection","variant":"future_native_content"})"})
    {
        write(wire, header +
            R"({"type":"context.append_message","message":{"role":"user","content":[{"type":"text","text":"Unclassified native content"}],"toolCalls":[],"origin":)" +
            origin + "}}\n");
        expect(request, Availability::AVAILABLE, Content::UNKNOWN, host,
            "missing or unrecognized injection semantics cannot prove emptiness");
    }
    write(wire, header + model);
    expect(request, Availability::AVAILABLE, Content::NONEMPTY, host, "model-only root history");

    fs::remove(wire);
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host, "resumable metadata with missing history");
    write(wire, "");
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host, "uninitialized wire file");
    write(wire, header + user.substr(0, user.size() - 1));
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host, "unterminated native write");
    write(wire, header + user);
    expect(request, Availability::AVAILABLE, Content::NONEMPTY, host, "later completed write uses the same record");

    for (const auto& tail : std::vector<std::string>{
            "{malformed}\n",
            R"({"type":"context.undo","count":1})" "\n",
            R"({"type":"context.clear"})" "\n",
            R"({"type":"context.apply_compaction","summary":"saved summary"})" "\n",
            R"({"type":"agent.switched","branch":"undo-1","base":{"branch":"main","line":1}})" "\n",
            R"({"type":"forked","sourceSessionId":"parent"})" "\n",
            R"({"type":"future.native.record"})" "\n",
            R"({"type":"context.append_loop_event","event":{"type":"step.begin","uuid":"pending"}})" "\n",
            R"({"type":"context.append_message","message":{"role":"user","content":[{"type":"image_url","image_url":{"url":"blob:pending"}}],"toolCalls":[]}})" "\n"})
    {
        write(wire, header + user + tail);
        expect(request, Availability::AVAILABLE, Content::UNKNOWN, host,
            "unsupported, incomplete or replay-dependent history is recoverable");
    }
    write(wire, header + R"({"type":"usage.record","usage":{"inputOther":0,"output":0,"inputCacheRead":0,"inputCacheCreation":0}})" "\n");
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host, "usage counters are not empty-content proof");
    write(wire, R"({"type":"metadata","protocol_version":"9.0","created_at":1789200000000})" "\n" + user);
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host, "future wire format");
    write(wire, R"({"type":"metadata","protocol_version":"1.5","min_protocol_version":"9.0","created_at":1789200000000})" "\n");
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host, "unsupported minimum wire reader");

    write(wire, header);
    metadata.set("title", Json("A chosen title is not content"));
    write(state, addon::serialize_json(metadata));
    expect(request, Availability::AVAILABLE, Content::EMPTY, host, "titles do not manufacture user history");
    metadata.set("agents", json_util::object({{"agent-1", json_util::object({{"type", Json("sub")}})}}));
    write(state, addon::serialize_json(metadata));
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host, "child-only metadata cannot prove root emptiness");
    metadata = native_metadata("session-a");
    metadata.set("forkedFrom", Json("parent-session"));
    write(state, addon::serialize_json(metadata));
    expect(request, Availability::AVAILABLE, Content::UNKNOWN, host, "inherited history stays unknown");
    metadata = native_metadata("session-a");
    metadata.set("version", Json(99));
    write(state, addon::serialize_json(metadata));
    expect(request, Availability::UNKNOWN, Content::UNKNOWN, host, "unsupported session metadata");
    write(state, "{\"id\":");
    expect(request, Availability::UNKNOWN, Content::UNKNOWN, host, "partial session metadata");
    fs::remove(state);
    expect(request, Availability::UNKNOWN, Content::UNKNOWN, host, "wire alone does not prove native availability");
    write(state, addon::serialize_json(native_metadata("session-a")));
    write(home / "sessions/other-workspace/session-a/state.json", read(state));
    expect(request, Availability::UNKNOWN, Content::UNKNOWN, host, "duplicate native selectors are ambiguous");
    fs::remove_all(home / "sessions/other-workspace");
    write(wire, header + user);

    const auto other_home = fixture / "other-home";
    write(other_home / "sessions/workspace/session-a/state.json", addon::serialize_json(native_metadata("session-a")));
    write(other_home / "sessions/workspace/session-a/agents/main/wire.jsonl", header + model);
    const auto other_profile = inspection_request(other_home, fixture);
    expect(other_profile, Availability::AVAILABLE, Content::NONEMPTY, host, "same raw ID in another selected profile");
    require(other_profile.resume_record != request.resume_record &&
        other_profile.context.account_context->adapter_account_key != request.context.account_context->adapter_account_key,
        "native ID equality does not merge distinct provider profiles");
    auto foreign = request;
    foreign.resume_record = other_profile.resume_record;
    expect(foreign, Availability::UNKNOWN, Content::UNKNOWN, host, "record cannot inspect a different profile");
    auto unbound = request;
    unbound.context.launch_environment->effective_environment.set("KIMI_CODE_HOME", Json(addon::path_to_utf8(fixture / "other-home")));
    expect(unbound, Availability::UNKNOWN, Content::UNKNOWN, host, "frozen selector must agree with bound account");
    auto legacy = request;
    legacy.resume_record = json_util::object({{"provider_session_id", Json("session-a")}});
    expect(legacy, Availability::UNKNOWN, Content::UNKNOWN, host, "legacy record has no original native store proof");
    request.resume_record = scoped_record(home, "absent-session");
    expect(request, Availability::ABSENT, Content::UNKNOWN, host, "complete native store scan proves absence");
    fs::rename(home / "sessions", home / "unavailable-sessions");
    expect(request, Availability::UNKNOWN, Content::UNKNOWN, host, "unavailable store does not erase history");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        Workspace workspace;
        if (argc == 2) {
            addon::Simulator_options options;
            options.package_directory = addon::path_from_utf8(argv[1]);
            options.profile_storage_root = workspace.path / "host-storage";
            addon::Host_simulator host(options, nullptr);
            addon::Contract_failure failure;
            require(host.start(&failure), failure.detail);
            history_contract(workspace.path, &host);
        }
        else {
            history_contract(workspace.path, nullptr);
        }
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
