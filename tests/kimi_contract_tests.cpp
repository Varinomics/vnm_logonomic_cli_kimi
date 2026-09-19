#include "kimi_acp.h"
#include "kimi_config.h"
#include "kimi_json.h"
#include "kimi_launcher.h"
#include "kimi_observer.h"
#include "kimi_quota.h"
#include "kimi_wire.h"

#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/ids.h>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <sys/wait.h>
#endif

namespace {

namespace addon = logonomic::cli_addon;
using kimi::Json;

void require(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
}

Json json(const char* text)
{
    const auto result = kimi::parse(text);
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
    start.event.body = kimi::object({{"session_id", Json("session")}, {"kimi_code_home", Json(kimi::path_string(fixture.root))}});
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
    observing.invocation.payload = addon::encode_observe_request(request, 2);
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
        polling.invocation.payload = addon::encode_poll_observations_request(poll, 2);
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
    // session identity and mode before the implementation prompt or result.
    addon::Structured_inspection request;
    request.protocol_state = kimi::initial_acp_state("Implement the requested rename.");
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":1}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"sessionId\":\"session-alpha\"}}\n";
    auto response = kimi::inspect_acp(request);
    require(!response.complete && response.stdin_bytes && response.stdin_bytes->find("session/set_config_option") != std::string::npos,
        "new ACP session must request auto mode before prompt");
    request.protocol_state = response.protocol_state;
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"configOptions\":[{\"id\":\"mode\",\"currentValue\":\"auto\"}]}}\n";
    response = kimi::inspect_acp(request);
    require(!response.complete && response.stdin_bytes && response.stdin_bytes->find("session/prompt") != std::string::npos,
        "authenticated auto mode permits prompt");
    request.protocol_state = response.protocol_state;
    const auto prompt_state = request.protocol_state;
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"session-alpha\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"Renamed successfully.\"}}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"stopReason\":\"end_turn\"}}\n";
    response = kimi::inspect_acp(request);
    require(!response.complete && response.stdin_bytes && response.stdin_bytes->find("session/close") != std::string::npos,
        "prompt result must wait for authenticated close");
    request.protocol_state = response.protocol_state;
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{}}\n";
    response = kimi::inspect_acp(request);
    require(response.complete && response.result_bytes == "Renamed successfully." && response.protocol_state.empty(),
        "authenticated close returns exact text and releases continuation");
    request.protocol_state = prompt_state;
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"foreign\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"untrusted\"}}}}\n";
    response = kimi::inspect_acp(request);
    require(response.complete && response.failure_reason.has_value() && !response.result_bytes, "foreign session output must fail authentication");
    request.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"session/request_permission\",\"params\":{}}\n";
    response = kimi::inspect_acp(request);
    require(response.complete && response.failure_reason.has_value(), "native auto execution must refuse unexpected reverse authority requests");
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
    require(kimi::member(kimi::member(response, "primary"), "used_percent").as_number() == 125, "quota must not clamp over-limit usage");
    require(kimi::member(kimi::member(response, "secondary"), "window_minutes").as_integer() == 10080, "weekly quota normalization");
    const auto missing = kimi::normalize_quota(json(R"({"code":0,"data":{"kind":"error","message":"No token for \"kimi-code\". Run /login to authenticate."}})"));
    require(kimi::text(missing, "status") == "not_logged_in", "missing login remains distinct from service error");
    bool rejected = false;
    try {
        kimi::normalize_quota(json(R"({"code":0,"data":{"kind":"ok","limits":[{"used":2,"limit":10,"window":{"duration":5,"unit":"hour"}},{"used":3,"limit":10,"window":{"duration":5,"unit":"hour"}}]}})"));
    }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "duplicate supported rolling windows are ambiguous");
}

} // namespace

int main()
{
    try {
        configuration();
        wire_layout();
        saturated_session_end();
#ifndef _WIN32
        probe_reaps_owned_leader();
#endif
        acp();
        wire();
        quota();
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
