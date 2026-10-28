#include "kimi_accounts.h"
#include "kimi_acp.h"
#include "kimi_config.h"
#include "kimi_guidance.h"
#include "kimi_json.h"
#include "kimi_launcher.h"
#include "kimi_observer.h"
#include "kimi_quota.h"
#include "kimi_wire.h"

#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/probe_contracts.h>
#include <logonomic_cli_addon/quota.h>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <sys/wait.h>
#endif

namespace json_util = logonomic::cli_addon::json;

namespace {

namespace addon = logonomic::cli_addon;
using kimi::Json;

void require(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
}

Json json(const char* text)
{
    const auto result = json_util::parse(text);
    if (!result) throw std::runtime_error("invalid test input");
    return *result;
}

void configuration()
{
    // Oracle: native marked-block ownership preserves user's sibling tables,
    // comments and hooks while refreshing only Logonomic's hook entries.
    const std::string user = "model = \"team-model\"\r\n# User-owned hook\r\n[[hooks]]\r\nevent = \"SessionStart\"\r\ncommand = \"audit\"\r\n";
    const auto updated = kimi::updated_config(user);
    require(updated.starts_with(user), "unmanaged configuration bytes must survive");
    require(updated.find(kimi::hook_entry("SessionStart", 18)) != std::string::npos, "SessionStart relay deadline must be preserved");
    for (const auto* event : {"PermissionRequest", "PermissionResult"}) {
        require(updated.find(kimi::hook_entry(event, 13)) != std::string::npos,
            "native permission requests and results must both reach the helper");
    }
    require(kimi::updated_config(updated) == updated, "managed hook installation must be idempotent");
    const auto duplicate = kimi::updated_config(updated + kimi::hook_entry("SessionStart", 18));
    const auto marker = std::string("# Logonomic managed Kimi hook: SessionStart");
    require(duplicate.find(marker, duplicate.find(marker) + marker.size()) == std::string::npos, "duplicate owned hooks must collapse");
    const auto orphan = kimi::updated_config("# Logonomic managed Kimi hook: SessionStart\n[providers.team]\nkey = 'private'\n");
    require(orphan.find("[providers.team]\nkey = 'private'\n") != std::string::npos, "orphan marker must not consume a foreign table");
    const std::string multiline = R"toml([[hooks]]
event = "SessionStart"
command = '''
cat <<'PAYLOAD'
# Logonomic managed Kimi hook: SessionStart
[[hooks]]
event = "example"
command = "example"
PAYLOAD
'''
timeout = 1
)toml";
    const auto preserved = kimi::updated_config(multiline);
    require(preserved.starts_with(multiline), "marker and table text inside multiline literal data must survive exactly");
    require(kimi::updated_config(preserved) == preserved, "preserved multiline configuration remains valid and idempotent");
    const std::string basic = R"toml(message = """
# Logonomic managed Kimi hook: SessionStart
# closing string delimiter: """
[[hooks]]
event = "SessionStart"
command = "user-owned"
timeout = 1
)toml";
    require(kimi::updated_config(basic).starts_with(basic), "a comment-shaped string terminator does not lend ownership to an earlier string line");
    const std::string formatted = "# Logonomic managed Kimi hook: SessionStart\r\n"
        "[[\"hooks\"]] # table note\r\n'event' = 'SessionStart' # event note\r\n"
        "\"command\" = 'echo caf\xC3\xA9' # command note\r\ntimeout = 1 # deadline note\r\n";
    const auto refreshed = kimi::updated_config(formatted);
    for (const auto comment : {"# table note\r\n", "# event note\r\n", "# command note\r\n", "# deadline note\r\n"}) {
        require(refreshed.find(comment) != std::string::npos, "managed value refresh preserves inline comments and CRLF");
    }
    require(kimi::updated_config(refreshed) == refreshed, "quoted keys and UTF-8 source columns round-trip");
    const auto collapsed = kimi::updated_config(refreshed + formatted);
    require(collapsed.find("# command note\r\n", collapsed.find("# command note\r\n") + 1) != std::string::npos,
        "duplicate owned syntax removal preserves comments from both entries");
    require(kimi::updated_config(collapsed) == collapsed, "duplicate cleanup leaves valid stable TOML");
    bool rejected = false;
    try { (void)kimi::updated_config(formatted + "matcher = 'user-owned'\n"); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "a marked table with additional user fields must not be overwritten");
    rejected = false;
    try { (void)kimi::updated_config("command = '''unterminated\n"); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid input fails before any transformed bytes can be committed");
}

void session_guidance()
{
    namespace fs = std::filesystem;
    struct Fixture
    {
        fs::path root = fs::temp_directory_path() / ("kimi-guidance-" + addon::random_nonce_hex());
        ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    } fixture;
    const auto path = kimi::session_guidance_path(fixture.root, "generation-alpha", "session-alpha");
    const auto other = kimi::session_guidance_path(fixture.root, "generation-alpha", "session-beta");
    const auto resumed = kimi::session_guidance_path(fixture.root, "generation-beta", "session-alpha");
    require(path != other && path != resumed, "guidance is isolated by provider session and package generation");
    const auto traversal = kimi::session_guidance_path(fixture.root, "generation-alpha", "../../outside");
    require(traversal.parent_path().parent_path() == fixture.root / "scratch" / "logonomic",
        "untrusted native identities cannot escape the guidance directory");
    const std::string instruction =
        "Record the session title and summary. Report worktrees before editing.\n"
        "Use delegation and the session mailbox. Preserve Unicode: \xE6\x9D\xB1\xE4\xBA\xAC.\n";
    const auto reminder = kimi::publish_session_guidance(path, instruction);
    const auto read = [](const fs::path& file) {
        std::ifstream input(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    // Oracle: UserPromptSubmit stdout is both native model context and visible
    // hook output. The user requested a short reminder with the complete guide
    // available through Read, preserving every requested instruction.
    require(read(path) == instruction, "native Read receives the complete UTF-8 instructions");
    require(reminder.find(addon::serialize_json(Json(kimi::path_string(path)))) != std::string::npos &&
        reminder.find("native Read") != std::string::npos,
        "hook output points the native Read tool to the saved guide");
    require(reminder.find("Record the session title") == std::string::npos,
        "the hook must not print the internal manual unsolicited");
    (void)kimi::publish_session_guidance(other, "Other session guidance");
    (void)kimi::publish_session_guidance(path, "Reconsider the recorded summary and read pending mailbox messages.");
    require(read(path) == "Reconsider the recorded summary and read pending mailbox messages.",
        "periodic instructions replace the same bounded session file");
    require(read(other) == "Other session guidance", "an update cannot overwrite another session");
    require(std::distance(fs::directory_iterator(path.parent_path()), fs::directory_iterator()) == 1,
        "repeated updates leave one complete file and no temporary artifacts");
    bool refused = false;
    try { (void)kimi::publish_session_guidance(path, "invalid \xFF"); }
    catch (const std::exception&) { refused = true; }
    require(refused && read(path).starts_with("Reconsider"), "invalid UTF-8 cannot replace complete guidance");
    kimi::remove_session_guidance(path);
    require(!fs::exists(path.parent_path()) && fs::exists(other),
        "session end removes its own guidance while preserving other sessions");
    kimi::remove_session_guidance(path);
}

void wire_layout()
{
    namespace fs = std::filesystem;
    struct Fixture
    {
        fs::path root = fs::temp_directory_path() / ("kimi-wire-layout-" + addon::random_nonce_hex());
        ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    } fixture;
    const auto session = fixture.root / "sessions" / "workspace-alpha" / "session-alpha";
    const auto main_wire = session / "agents" / "main" / "wire.jsonl";
    fs::create_directories(main_wire.parent_path());
    std::ofstream(main_wire) << "{}\n";
    // The vendor keeps the root agent under agents/main, while child-agent
    // streams and a misplaced session-level file must not select the root.
    std::ofstream(session / "wire.jsonl") << "{}\n";
    fs::create_directories(session / "agents" / "child-alpha");
    std::ofstream(session / "agents" / "child-alpha" / "wire.jsonl") << "{}\n";
    const auto home = kimi::path_string(fixture.root);
    require(kimi::find_session_wire(home, "session-alpha") == fs::canonical(main_wire),
        "continuous observation binds the real root-agent wire layout");
    require(!kimi::find_session_wire(home, "session-other"), "another native session cannot be selected");
    bool rejected = false;
    try { kimi::find_session_wire(home, "../session-alpha"); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "session identity cannot traverse the selected profile");

    const auto second = fixture.root / "sessions" / "workspace-beta" / "session-alpha" / "agents" / "main" / "wire.jsonl";
    fs::create_directories(second.parent_path());
    std::ofstream(second) << "{}\n";
    rejected = false;
    try { kimi::find_session_wire(home, "session-alpha"); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "duplicate native session identities remain ambiguous across workspaces");
}

void saturated_session_end()
{
    namespace fs = std::filesystem;
    struct Fixture
    {
        fs::path root = fs::temp_directory_path() / ("kimi-wire-capacity-" + addon::random_nonce_hex());
        ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    } fixture;
    const auto path = fixture.root / "sessions" / "workspace" / "session" / "agents" / "main" / "wire.jsonl";
    fs::create_directories(path.parent_path());
    {
        std::ofstream wire(path);
        for (int index = 0; index < 2047; ++index) {
            wire << "{\"type\":\"context.append_loop_event\",\"event\":{\"type\":\"tool.call\",\"name\":\"Agent\",\"toolCallId\":\"call-" << index << "\"}}\n";
            wire << "{\"type\":\"context.append_loop_event\",\"event\":{\"type\":\"tool.result\",\"toolCallId\":\"call-" << index << "\"}}\n";
        }
        wire << R"({"type":"context.append_loop_event","event":{"type":"tool.call","name":"Agent","toolCallId":"last-live-call"}})" << '\n';
        require(static_cast<bool>(wire), "saturation fixture writes complete native records");
    }
    class Context : public addon::Invocation_context
    {
    public:
        const addon::Invoke_request& request() const override { return invocation; }
        bool cancel_requested() const override { return cancelled; }
        bool wait_for_cancel(int) override { return cancelled; }
        void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override { cancelled = true; }
        void succeed(Json) override { succeeded = true; }
        void fail(addon::Result_status, std::string reason, std::string) override { failure = std::move(reason); }
        bool finished() const override { return succeeded || !failure.empty(); }
        addon::Invoke_request invocation;
        bool cancelled = false;
        bool succeeded = false;
        std::string failure;
    } observing;
    const std::string nonce(32, 'a');
    kimi::Lifecycle_observer observer;
    addon::Classify_event_request start;
    start.event.name = "SessionStart";
    start.event.body = json_util::object({{"session_id", Json("session")}, {"kimi_code_home", Json(kimi::path_string(fixture.root))}});
    start.transition_nonce = nonce;
    Context reservation;
    reservation.invocation.capability = {"lifecycle-correlation-lifetime", 1};
    reservation.invocation.operation = "reserve";
    reservation.invocation.payload = addon::encode_lifecycle_correlation_reserve_request({{nonce}});
    observer.handle(reservation);
    require(reservation.succeeded, "host reserves the classifier correlation first");
    observer.classify(start);
    addon::Observe_request request;
    request.session = {kimi::path_string(fixture.root), {"kimi"}, "session"};
    request.window_ms = addon::limits::k_observe_window_ms;
    request.correlation = addon::Session_correlation{nonce};
    observing.invocation.operation = "observe";
    observing.invocation.payload = addon::encode_observe_request(request);
    observer.handle(observing);
    require(observing.succeeded, "full retained queue is established before SessionEnd");
    auto ending = start;
    ending.event.name = "SessionEnd";
    ending.ledger.current_epoch = 1;
    ending.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "session", nonce};
    bool rejected = false;
    try { observer.classify(ending); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "SessionEnd cannot acknowledge a terminal fact that exceeds retained capacity");
    addon::Poll_observations_request poll;
    poll.limit = 16;
    poll.correlation = addon::Session_correlation{nonce};
    for (int attempt = 0; attempt < 2; ++attempt) {
        Context polling;
        polling.invocation.operation = "poll_observations";
        polling.invocation.payload = addon::encode_poll_observations_request(poll);
        observer.handle(polling);
        require(!polling.succeeded && polling.failure == "observation_queue_capacity_exhausted",
            "poll reports retained failure repeatedly rather than silently draining and erasing the session");
    }
}

#ifndef _WIN32
void probe_reaps_owned_leader()
{
    class Context : public addon::Invocation_context
    {
    public:
        const addon::Invoke_request& request() const override { return invocation; }
        bool cancel_requested() const override
        {
            return cancel_at && std::chrono::steady_clock::now() >= *cancel_at;
        }
        bool wait_for_cancel(int) override { return cancel_requested(); }
        void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
        void succeed(Json) override {}
        void fail(addon::Result_status, std::string, std::string) override {}
        bool finished() const override { return false; }
        addon::Invoke_request invocation;
        std::optional<std::chrono::steady_clock::time_point> cancel_at;
    } context;
    namespace fs = std::filesystem;
    struct Fixture
    {
        fs::path root = fs::temp_directory_path() / ("kimi-probe-reap-" + addon::random_nonce_hex());
        ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    } fixture;
    fs::create_directories(fixture.root);
    const auto directory = kimi::path_string(fixture.root);
    const auto successful = kimi::probe({"/bin/sh", "-c", "printf '%s' \"$$\""}, {}, directory, 2000, context);
    require(successful.has_value(), "successful probe preserves its output");
    const auto successful_pid = static_cast<pid_t>(std::stol(successful->output));
    int status = 0;
    require(waitpid(successful_pid, &status, WNOHANG) == -1 && errno == ECHILD,
        "successful probe has already reaped its leader");

    for (const bool cancel : {false, true}) {
        const auto pid_file = fixture.root / (cancel ? "cancelled.pid" : "timed-out.pid");
        context.cancel_at = cancel
            ? std::optional(std::chrono::steady_clock::now() + std::chrono::milliseconds(200)) : std::nullopt;
        // Shell builtins avoid spawning an unrelated sleep descendant. The
        // fixture checks timeout and cancellation against the owned leader.
        const auto result = kimi::probe({"/bin/sh", "-c", "printf '%s' \"$$\" > \"$1\"; while :; do :; done",
            "kimi-probe-fixture", kimi::path_string(pid_file)}, {}, directory, cancel ? 2000 : 200, context);
        require(!result, "timeout and cancellation cannot return successful probe evidence");
        pid_t pid = 0;
        std::ifstream(pid_file) >> pid;
        require(pid > 0, "owned probe leader recorded its identity");
        require(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD,
            "failed probe synchronously reaps its killed leader");
    }
}
#endif

void acp()
{
    // Oracle: adopted native Kimi ACP transcript authenticates each response ID,
    // session identity, mode and task-server admission before the
    // implementation prompt or result.
    addon::Delegated_prepare_request task;
    task.prompt = "Implement the requested rename.";
    task.control.task_id = "task-alpha";
    task.control.servers.push_back({"coordination", "/opt/logonomic/logonomic_coordination_mcp", {"status", "notice"}, {}});
    addon::Structured_inspection request;
    request.protocol_state = kimi::initial_acp_state(task);
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":1}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"sessionId\":\"session-alpha\"}}\n";
    auto response = kimi::inspect_acp(request);
    require(!response.complete && !response.stdin_bytes,
        "new ACP session keeps launcher permissions and waits for the command catalog");
    request.protocol_state = response.protocol_state;
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"session-alpha\",\"update\":{\"sessionUpdate\":\"available_commands_update\",\"availableCommands\":[{\"name\":\"mcp\",\"description\":\"Show MCP server status\"}]}}}\n"
        ;
    response = kimi::inspect_acp(request);
    require(!response.complete && response.stdin_bytes && response.stdin_bytes->find("\"text\":\"/mcp\"") != std::string::npos &&
        response.stdin_bytes->find(task.prompt) == std::string::npos, "authenticated command catalog enables task-server status");
    request.protocol_state = response.protocol_state;
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"session-alpha\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"MCP servers (1):\\n- logonomic-task-task-alpha-coordination (stdio): connected, 2 tools\"}}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"stopReason\":\"end_turn\"}}\n";
    response = kimi::inspect_acp(request);
    require(!response.complete && response.stdin_bytes && response.stdin_bytes->find(task.prompt) != std::string::npos,
        "admitted task servers permit prompt");
    request.protocol_state = response.protocol_state;
    const auto prompt_state = request.protocol_state;
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"session-alpha\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"Renamed successfully.\"}}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"stopReason\":\"end_turn\"}}\n";
    response = kimi::inspect_acp(request);
    require(!response.complete && response.stdin_bytes && response.stdin_bytes->find("session/close") != std::string::npos,
        "prompt result must wait for authenticated close");
    request.protocol_state = response.protocol_state;
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":{}}\n";
    response = kimi::inspect_acp(request);
    require(response.complete && response.result_bytes == "Renamed successfully." && response.protocol_state.empty(),
        "authenticated close returns exact text and releases continuation");
    request.protocol_state = prompt_state;
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"foreign\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"untrusted\"}}}}\n";
    response = kimi::inspect_acp(request);
    require(response.complete && response.failure_reason.has_value() && !response.result_bytes, "foreign session output must fail authentication");
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"session/request_permission\",\"params\":{}}\n";
    response = kimi::inspect_acp(request);
    require(response.complete && response.failure_reason.has_value(), "malformed permission requests fail without approval");
    request.stdout_bytes = std::string(256 * 1024 + 1, 'x');
    response = kimi::inspect_acp(request);
    require(response.complete && response.failure_reason.has_value(), "unterminated ACP record remains bounded");
}

void wire()
{
    // Oracle: the native manager uses Agent tool-call identity across separate
    // subagent runtime IDs; terminal tombstones suppress replay and late spawn.
    kimi::Wire_decoder decoder("session-alpha");
    const auto start = json(R"({"type":"context.append_loop_event","event":{"type":"tool.call","name":"Agent","toolCallId":"call-alpha","args":{"subagent_type":"reviewer","run_in_background":true}}})");
    auto events = decoder.accept(start);
    require(events.size() == 1 && events.front().agent_id == "call-alpha" &&
        events.front().subagent_transition == addon::Subagent_transition::STARTED, "Agent tool call starts exactly one public agent");
    require(decoder.accept(json(R"({"type":"subagent.spawned","subagentId":"agent-alpha","parentToolCallId":"call-alpha","runInBackground":true,"taskId":"task-alpha"})")).empty(),
        "spawn correlation must not double-start agent");
    require(decoder.accept(json(R"({"type":"subagent.started","subagentId":"agent-alpha"})")).empty(), "correlated start is idempotent");
    require(decoder.accept(json(R"({"type":"context.append_loop_event","event":{"type":"tool.result","toolCallId":"call-alpha","result":{"output":"task_id: task-alpha\nstatus: running\n\nprivate output"}}})")).empty(),
        "background metadata header keeps agent active");
    events = decoder.accept(json(R"({"type":"context.append_message","message":{"origin":{"kind":"background_task","taskId":"task-alpha","status":"completed"}}})"));
    require(events.size() == 1 && events.front().agent_id == "call-alpha" &&
        events.front().subagent_transition == addon::Subagent_transition::STOPPED, "background terminal fact stops correlated public agent");
    decoder.rewind();
    require(decoder.accept(start).empty(), "file replay must not revive terminal direct call");
    require(decoder.accept(json(R"({"type":"subagent.started","subagentId":"agent-alpha"})")).empty(), "late runtime start must not revive terminal call");
    require(decoder.close("SessionEnd").empty(), "terminal calls do not stop twice");
    kimi::Wire_decoder independent("session-beta");
    events = independent.accept(json(R"({"type":"subagent.started","subagentId":"agent-beta"})"));
    require(events.size() == 1, "uncorrelated runtime start is supported");
    events = independent.close("SessionEnd");
    require(events.size() == 1 && events.front().agent_id == "agent-beta", "session close reconciles live agent once");

    // Native QString::size counts UTF-16 code units. A full-width BMP label
    // occupies 768 UTF-8 bytes at the admitted 256-unit boundary.
    kimi::Wire_decoder labelled("session-labelled");
    std::string label;
    for (int index = 0; index < 256; ++index) label += "\xE7\x95\x8C";
    auto labelled_start = json(R"({"type":"context.append_loop_event","event":{"type":"tool.call","toolCallId":"labelled-call","name":"Agent","display":{}}})");
    labelled_start.find("event")->find("display")->set("agent_name", kimi::Json(label));
    events = labelled.accept(labelled_start);
    require(events.size() == 1 && events.front().agent_label == label, "256 UTF-16 units preserve complete non-ASCII label");
    labelled_start.find("event")->set("toolCallId", kimi::Json("oversized-label-call"));
    labelled_start.find("event")->find("display")->set("agent_name", kimi::Json(label + "a"));
    events = labelled.accept(labelled_start);
    require(events.size() == 1 && events.front().agent_label.empty(), "native oversized label is omitted without losing agent start");
}

void quota()
{
    // Oracle: Kimi local oauth/usage response has a weekly summary and exactly
    // one supported five-hour rolling window, with percentages allowed >100.
    const auto response = kimi::normalize_quota(json(R"({"code":0,"data":{"kind":"ok","summary":{"used":50,"limit":200,"window":{"duration":1,"unit":"week"}},"limits":[{"used":125,"limit":100,"window":{"duration":5,"unit":"hour"}}]}})"));
    require(json_util::member(json_util::member(response, "primary"), "used_percent").as_number() == 125, "quota must not clamp over-limit usage");
    require(json_util::member(json_util::member(response, "secondary"), "window_minutes").as_integer() == 10080, "weekly quota normalization");
    // Oracle: Kimi Code 2.0.2's local oauth/usage reply (shape observed on
    // 2026-09-22) reports each window under data.quota.usages as a used ratio
    // with a UTC reset time, and no summary or limits.
    const auto usages = kimi::normalize_quota(json(R"({"code":0,"data":{"kind":"ok","quota":{"usages":{
        "limit5h":{"usedRatio":0.25,"resetAt":"2026-09-22T17:00:00Z"},
        "limit7d":{"usedRatio":1.5,"resetAt":"2026-09-28T09:00:00Z"}},"extraUsage":null}}})"));
    const auto& five_hour = json_util::member(usages, "primary");
    const auto& weekly    = json_util::member(usages, "secondary");
    require(json_util::member(five_hour, "used_percent").as_number() == 25 &&
        json_util::member(five_hour, "window_minutes").as_integer() == 300 &&
        json_util::text(five_hour, "reset_at") == "2026-09-22T17:00:00Z", "five-hour usage ratio is the primary window");
    require(json_util::member(weekly, "used_percent").as_number() == 150 &&
        json_util::member(weekly, "window_minutes").as_integer() == 10080, "weekly usage ratio is not clamped");
    const auto missing = kimi::normalize_quota(json(R"({"code":0,"data":{"kind":"error","message":"No token for \"kimi-code\". Run /login to authenticate."}})"));
    require(json_util::text(missing, "status") == "not_logged_in", "missing login remains distinct from service error");
    bool rejected = false;
    try {
        kimi::normalize_quota(json(R"({"code":0,"data":{"kind":"ok","limits":[{"used":2,"limit":10,"window":{"duration":5,"unit":"hour"}},{"used":3,"limit":10,"window":{"duration":5,"unit":"hour"}}]}})"));
    }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "duplicate supported rolling windows are ambiguous");
}

// The quota helper's normalization of a local usage reply; empty when refused.
std::optional<Json> normalized_reply(const char* reply)
{
    try {
        return kimi::normalize_quota(json(reply));
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
}

// The adapter's final inspection of that helper output, decoded as the host
// decodes a quota@2 result; empty when the adapter refuses the output.
std::optional<addon::Quota_inspect_result> inspected_quota(
    const Json& normalized, addon::Contract_failure* out_failure = nullptr)
{
    class Context : public addon::Invocation_context
    {
    public:
        const addon::Invoke_request& request() const override { return invocation; }
        bool cancel_requested() const override { return false; }
        bool wait_for_cancel(int) override { return false; }
        void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
        void succeed(Json value) override { payload = std::move(value); }
        void fail(addon::Result_status, std::string reason, std::string detail) override
        {
            failed = true;
            failure.reason_code = std::move(reason);
            failure.detail      = std::move(detail);
        }
        bool finished() const override { return payload || failed; }
        addon::Invoke_request invocation;
        std::optional<Json> payload;
        addon::Contract_failure failure;
        bool failed = false;
    } inspection;
    addon::Probe_inspect_request request;
    request.inspection.final        = true;
    request.inspection.exit_code    = 0;
    request.inspection.stdout_bytes = addon::serialize_json(normalized) + "\n";
    inspection.invocation.capability = {"quota", 2};
    inspection.invocation.operation  = "inspect";
    inspection.invocation.payload    = addon::encode_probe_inspect_request(request, 2);
    try {
        kimi::handle_quota(inspection);
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
    if (!inspection.payload) {
        if (out_failure) *out_failure = inspection.failure;
        return std::nullopt;
    }
    addon::Contract_failure failure;
    return addon::decode_quota_inspect_result(*inspection.payload, true, &failure, 2);
}

void quota_usages()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char* description) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", description);
            ++failures;
        }
    };

    // Oracle: https://moonshotai.github.io/kimi-code/en/reference/server-api.html
    // GET /api/v1/oauth/usage reports whichever windows the account has.
    // A successful empty usages object was also observed from Kimi 2.1.1.
    // Summary and limits never override or complete the quota shape.
    const auto both = normalized_reply(R"({"code":0,"data":{"kind":"ok","quota":{"usages":{
        "limit5h":{"usedRatio":0.25,"resetAt":"2026-09-22T17:00:00Z"},
        "limit7d":{"usedRatio":0.5,"resetAt":"2026-09-28T09:00:00Z"}}},
        "summary":{"used":90,"limit":100,"window":{"duration":1,"unit":"week"}},
        "limits":[{"used":80,"limit":100,"window":{"duration":5,"unit":"hour"}}]}})");
    check(both &&
        json_util::member(json_util::member(*both, "primary"),   "used_percent").as_number() == 25 &&
        json_util::member(json_util::member(*both, "secondary"), "used_percent").as_number() == 50,
        "quota.usages wins over summary and limits in the same reply");
    const auto primary_only = normalized_reply(R"({"code":0,"data":{"kind":"ok","quota":{"usages":{
        "limit5h":{"usedRatio":0.25}}}}})");
    const auto primary_observed = primary_only ? inspected_quota(*primary_only) : std::nullopt;
    check(primary_observed && primary_observed->observation &&
        primary_observed->observation->state == addon::Quota_status::READY &&
        primary_observed->observation->primary && !primary_observed->observation->secondary &&
        primary_observed->observation->primary->used_percent == 25,
        "an absent weekly window does not discard reported five-hour quota");
    const auto secondary_only = normalized_reply(R"({"code":0,"data":{"kind":"ok","quota":{"usages":{
        "limit7d":{"usedRatio":0.5}}}}})");
    const auto secondary_observed = secondary_only ? inspected_quota(*secondary_only) : std::nullopt;
    check(secondary_observed && secondary_observed->observation &&
        secondary_observed->observation->state == addon::Quota_status::READY &&
        !secondary_observed->observation->primary && secondary_observed->observation->secondary &&
        secondary_observed->observation->secondary->used_percent == 50,
        "an absent five-hour window does not discard reported weekly quota");
    const auto empty = normalized_reply(R"({"code":0,"data":{"kind":"ok","quota":{"usages":{},"extraUsage":null},
        "summary":{"used":90,"limit":100,"window":{"duration":1,"unit":"week"}},
        "limits":[{"used":80,"limit":100,"window":{"duration":5,"unit":"hour"}}]}})");
    const auto empty_observed = empty ? inspected_quota(*empty) : std::nullopt;
    check(empty_observed && empty_observed->complete && empty_observed->observation &&
        empty_observed->observation->state == addon::Quota_status::UNAVAILABLE &&
        !empty_observed->observation->primary && !empty_observed->observation->secondary,
        "an empty successful usage reply is unavailable without invented values or an authentication failure");
    check(!inspected_quota(json(R"({"status":"unavailable","primary":{"used_percent":0,"window_minutes":300,"window_label":"5-hour"}})")),
        "unavailable quota cannot carry a fabricated usage value");
    const struct
    {
        const char* description;
        const char* reply;
    }
    refused[] = {
        {"a null five-hour window is refused, not completed from summary and limits",
            R"({"code":0,"data":{"kind":"ok","quota":{"usages":{"limit5h":null,
                "limit7d":{"usedRatio":0.5,"resetAt":"2026-09-28T09:00:00Z"}}},
                "summary":{"used":90,"limit":100,"window":{"duration":1,"unit":"week"}},
                "limits":[{"used":80,"limit":100,"window":{"duration":5,"unit":"hour"}}]}})"},
        {"a malformed used ratio is refused",
            R"({"code":0,"data":{"kind":"ok","quota":{"usages":{
                "limit5h":{"usedRatio":"0.25","resetAt":"2026-09-22T17:00:00Z"},
                "limit7d":{"usedRatio":0.5,"resetAt":"2026-09-28T09:00:00Z"}}}}})"},
        {"a missing usages object remains malformed",
            R"({"code":0,"data":{"kind":"ok","quota":{}}})"},
    };
    for (const auto& reply : refused) {
        check(!normalized_reply(reply.reply), reply.description);
    }

    // Oracle: quota@2 carries reset times as UTC epoch milliseconds, and an
    // offset timestamp names the same instant as its UTC spelling.
    const auto utc = normalized_reply(R"({"code":0,"data":{"kind":"ok","quota":{"usages":{
        "limit5h":{"usedRatio":0.25,"resetAt":"2026-09-22T17:00:00Z"},
        "limit7d":{"usedRatio":0.5,"resetAt":"2026-09-28T09:00:00Z"}}}}})");
    const auto observed = utc ? inspected_quota(*utc) : std::nullopt;
    check(observed && observed->complete && observed->observation &&
        observed->observation->state == addon::Quota_status::READY &&
        observed->observation->primary && observed->observation->secondary &&
        observed->observation->primary->used_percent     == 25 &&
        observed->observation->primary->window_minutes   == 300u &&
        observed->observation->primary->resets_at_ms     == 1790096400000u &&
        observed->observation->secondary->used_percent   == 50 &&
        observed->observation->secondary->window_minutes == 10080u &&
        observed->observation->secondary->resets_at_ms   == 1790586000000u,
        "the host decodes both windows with their exact UTC reset milliseconds");
    const auto offset = normalized_reply(R"({"code":0,"data":{"kind":"ok","quota":{"usages":{
        "limit5h":{"usedRatio":0.25,"resetAt":"2026-09-22T19:00:00+02:00"},
        "limit7d":{"usedRatio":0.5,"resetAt":"2026-09-28T09:00:00Z"}}}}})");
    const auto shifted = offset ? inspected_quota(*offset) : std::nullopt;
    check(shifted && shifted->observation && shifted->observation->primary &&
        shifted->observation->primary->resets_at_ms == 1790096400000u,
        "an offset reset time decodes to the same instant as its UTC spelling");
    const auto malformed = normalized_reply(R"({"code":0,"data":{"kind":"ok","quota":{"usages":{
        "limit5h":{"usedRatio":0.25,"resetAt":"2026-09-22T25:00:00Z"},
        "limit7d":{"usedRatio":0.5,"resetAt":"2026-09-28T09:00:00Z"}}}}})");
    check(!malformed || !inspected_quota(*malformed), "a malformed reset time never reaches the host");

    // Probe failures must retain a safe cause; arbitrary exception or reply
    // text must never become a diagnostic, because it can contain credentials.
    addon::Contract_failure failure;
    auto timeout = kimi::quota_probe_failure("quota_http_timeout");
    timeout.set("detail", Json("Bearer private-test-token"));
    check(!inspected_quota(timeout, &failure) && failure.reason_code == "quota_http_timeout" &&
        failure.detail == "The Kimi quota service did not respond in time.",
        "the adapter keeps the recognized timeout cause and uses its own safe diagnostic");
    const auto unknown = kimi::quota_probe_failure("Bearer private-test-token");
    check(!inspected_quota(unknown, &failure) && failure.reason_code == "quota_probe_failed" &&
        failure.detail.find("private-test-token") == std::string::npos &&
        addon::serialize_json(unknown).find("private-test-token") == std::string::npos,
        "unknown exception text is not copied into helper output or host diagnostics");

    require(failures == 0, "quota.usages preserves reported windows, absence and failures");
}

void account_names()
{
    // Oracle: accounts@2 names an account with a filesystem profile after the
    // profile's folder through the SDK's account_display_name.
    class Context : public addon::Invocation_context
    {
    public:
        const addon::Invoke_request& request() const override { return invocation; }
        bool cancel_requested() const override { return false; }
        bool wait_for_cancel(int) override { return false; }
        void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
        void succeed(Json value) override { payload = std::move(value); }
        void fail(addon::Result_status, std::string, std::string) override { failed = true; }
        bool finished() const override { return payload || failed; }
        addon::Invoke_request invocation;
        std::optional<Json> payload;
        bool failed = false;
    };
#ifdef _WIN32
    const std::string platform = "windows-x86_64";
    const std::string home     = "C:/Users/Jane Doe";
    const std::string home_env = "USERPROFILE";
#else
    const std::string platform = "linux-x86_64";
    const std::string home     = "/home/jane";
    const std::string home_env = "HOME";
#endif
    const auto discovered_account = [&](const std::string& kimi_home) -> std::optional<addon::Account_record> {
        addon::Accounts_launcher_request request;
        request.launcher.script_body = "kimi";
        request.launcher.runner_key  = addon::k_runner_direct;
        request.launch_environment.platform_id = platform;
        request.launch_environment.effective_environment = Json::object();
        request.launch_environment.effective_environment.set(home_env, Json(home));
        request.launch_environment.effective_environment.set("KIMI_CODE_HOME", Json(kimi_home));
        request.default_home_fallback   = home;
        request.normalization_directory = home;
        Context discovery;
        discovery.invocation.operation = addon::k_accounts_discover_operation;
        discovery.invocation.payload   = addon::encode_accounts_launcher_request(request);
        kimi::handle_accounts(discovery);
        addon::Contract_failure failure;
        const auto accounts = discovery.payload ? addon::decode_accounts_result(*discovery.payload, &failure) : std::nullopt;
        return accounts && accounts->size() == 1 ? std::optional(accounts->front()) : std::nullopt;
    };
    const auto discovered_name = [&](const std::string& kimi_home) {
        const auto account = discovered_account(kimi_home);
        return account ? account->display_name : std::string{};
    };
    require(discovered_name(home + "/.kimi-code-work/") == ".kimi-code-work",
        "a trailing separator does not hide the profile folder's name");
    const std::string long_home = home + "/.kimi-code-account-for-the-varinomics-nightly-release-builds";
    require(discovered_name(long_home) == addon::account_display_name(long_home),
        "an over-long folder name is fitted by the SDK's account display-name rule");
#ifdef _WIN32
    // A profile on a share that cannot be reached is an unavailable account;
    // it must not fail the discovery of every account.
    const auto unreachable = discovered_account("\\\\localhost\\logonomic-absent-share\\");
    require(unreachable && unreachable->display_name == "\\\\localhost\\logonomic-absent-share" &&
        unreachable->status == addon::Account_status::UNAVAILABLE,
        "a profile on an unreachable share is discovered as an unavailable account");
#endif
}

} // namespace

int main()
{
    try {
        configuration();
        session_guidance();
        wire_layout();
        saturated_session_end();
#ifndef _WIN32
        probe_reaps_owned_leader();
#endif
        acp();
        wire();
        quota();
        quota_usages();
        account_names();
        require(kimi::recognize_launcher("kimi --yolo"), "Kimi literal launcher recognition");
        require(!kimi::recognize_launcher("echo kimi"), "mention does not establish provider invocation");
        std::puts("Kimi contract checks passed.");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
