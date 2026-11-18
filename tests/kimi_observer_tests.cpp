#include "kimi_observer.h"
#include "kimi_execution_context.h"
#include "kimi_interactions.h"
#include "kimi_json.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sidecar.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancel.load(); }
    bool wait_for_cancel(int) override { return cancel_requested(); }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override
    {
        if (m_progress) m_progress();
        if (m_cancel_on_progress) m_cancel = true;
    }
    void succeed(addon::Json_value result) override { m_result = std::move(result); }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        m_status = status;
        m_failure = std::move(reason);
    }
    bool finished() const override { return m_result.has_value() || !m_failure.empty(); }

    addon::Invoke_request m_request;
    std::optional<addon::Json_value> m_result;
    addon::Result_status m_status = addon::Result_status::SUCCESS;
    std::string m_failure;
    std::atomic<bool> m_cancel = false;
    bool m_cancel_on_progress = false;
    std::function<void()> m_progress;
};

class Workspace final
{
public:
    Workspace() : m_root(fs::temp_directory_path() / ("kimi-lifetime-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_root);
    }
    ~Workspace() { std::error_code error; fs::remove_all(m_root, error); }
    fs::path m_root;
};

bool reserve(kimi::Lifecycle_observer& observer, const std::string& nonce)
{
    Context context;
    context.m_request.capability = {"lifecycle-correlation-lifetime", 1};
    context.m_request.operation = "reserve";
    context.m_request.payload = addon::encode_lifecycle_correlation_reserve_request({{nonce}});
    observer.handle(context);
    return context.m_result.has_value();
}

bool retire(kimi::Lifecycle_observer& observer, const std::string& nonce,
    std::optional<std::string> cursor = std::nullopt)
{
    Context context;
    context.m_request.capability = {"lifecycle-correlation-lifetime", 1};
    context.m_request.operation = "retire";
    addon::Lifecycle_correlation_retire_request request;
    request.correlation.transition_nonce = nonce;
    request.disposition = cursor ? addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED
                                 : addon::Lifecycle_correlation_retire_disposition::ABANDONED;
    request.consumed_cursor = std::move(cursor);
    context.m_request.payload = addon::encode_lifecycle_correlation_retire_request(request);
    observer.handle(context);
    return context.m_result.has_value();
}

addon::Classify_event_request start_request(const Workspace& workspace, const std::string& nonce,
    const std::string& session = "session")
{
    addon::Classify_event_request request;
    request.transition_nonce = nonce;
    request.event.name = "SessionStart";
    request.event.body = json_util::object({{"session_id", addon::Json_value(session)},
        {"kimi_code_home", addon::Json_value(kimi::path_string(workspace.m_root))}});
    return request;
}

void native_call(const Workspace& workspace, const std::string& session)
{
    const auto path = workspace.m_root / "sessions" / "workspace" / session / "agents" / "main" / "wire.jsonl";
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << R"({"type":"context.append_loop_event","event":{"type":"tool.call","name":"Agent","toolCallId":"live-child"}})" << '\n';
    require(static_cast<bool>(output), "native wire fixture must be complete");
}

void prepare_observe(Context& context, const Workspace& workspace, const std::string& nonce,
    const std::string& session = "session")
{
    addon::Observe_request request;
    request.session = {kimi::path_string(workspace.m_root), {"kimi"}, session};
    request.correlation = addon::Session_correlation{nonce};
    request.window_ms = addon::limits::k_observe_window_ms;
    context.m_request.capability = {"lifecycle-events", 2};
    context.m_request.operation = "observe";
    context.m_request.payload = addon::encode_observe_request(request);
}

std::optional<addon::Poll_observations_result> poll(kimi::Lifecycle_observer& observer,
    const std::string& nonce, std::string cursor = {}, std::size_t limit = 16, unsigned model_version = 0)
{
    addon::Poll_observations_request request;
    request.correlation = addon::Session_correlation{nonce};
    request.resume_cursor = std::move(cursor);
    request.limit = limit;
    request.model_state_version = model_version;
    Context context;
    context.m_request.capability = {"lifecycle-events", 2};
    context.m_request.operation = "poll_observations";
    context.m_request.payload = addon::encode_poll_observations_request(request);
    observer.handle(context);
    addon::Contract_failure failure;
    return context.m_result
        ? addon::decode_poll_observations_result(*context.m_result, request.limit, &failure)
        : std::nullopt;
}

bool wait_until(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

class Observe_window final
{
public:
    Observe_window(kimi::Lifecycle_observer& observer, const Workspace& workspace, const std::string& nonce,
        std::function<void()> model_progress = {})
    {
        prepare_observe(context, workspace, nonce);
        if (model_progress) {
            addon::encode_model_state_request(context.m_request.payload, addon::k_model_state_version);
            context.m_progress = std::move(model_progress);
        }
        worker = std::async(std::launch::async, [&observer, this] { observer.handle(context); });
    }
    ~Observe_window()
    {
        context.m_cancel = true;
        worker.wait();
    }
    void stop()
    {
        context.m_cancel = true;
        worker.wait();
    }
    Context context;
    std::future<void> worker;
};

std::string call_record(const std::string& id)
{
    return "{\"type\":\"context.append_loop_event\",\"event\":{\"type\":\"tool.call\","
        "\"name\":\"Agent\",\"toolCallId\":\"" + id + "\"}}\n";
}

void write_bytes(const fs::path& path, const std::string& bytes, bool append = false)
{
    std::ofstream output(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    output << bytes;
    output.close();
    require(static_cast<bool>(output), "wire fixture write must complete");
}

void irrelevant_interaction_records_preserve_observation()
{
    namespace observation = vnm::filesystem_observation;
    Workspace workspace;
    const auto path = workspace.m_root / "sessions/workspace/session/agents/main/wire.jsonl";
    fs::create_directories(path.parent_path());
    write_bytes(path, "{}\n");
    const auto service = std::make_shared<observation::Service>();
    observation::Configuration configuration;
    configuration.general_interval = std::chrono::milliseconds(25);
    kimi::Lifecycle_observer observer(service, configuration);
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve native interaction reader owner");
    auto start = start_request(workspace, nonce);
    start.event.body.set("interaction_frontier",
        kimi::capture_interaction_frontier(kimi::path_string(workspace.m_root), "session"));
    observer.classify(start);
    Observe_window window(observer, workspace, nonce);
    const auto settled = [&] {
        const auto statistics = service->statistics();
        return statistics.dispatches > 0 && statistics.outstanding == 0 &&
            statistics.pending_registrations == 0 && statistics.retained_events == 0;
    };
    require(wait_until(settled), "initial attention frontier must reach the actual receiver");
    const auto wait_observations = [&](std::size_t count) {
        const bool acquired = wait_until([&] {
            const auto page = poll(observer, nonce);
            return (page && page->observations.size() == count) ||
                window.worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        });
        if (window.worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            window.stop();
            throw std::runtime_error("reader isolation ended observation: " + window.context.m_failure);
        }
        const auto page = poll(observer, nonce);
        require(acquired && page && page->observations.size() == count,
            "native interaction observations must survive irrelevant records");
    };
    write_bytes(path,
        R"({"type":"interaction.request","id":"question_reader","agentId":"main","kind":"question","toolCallId":"question_call","request":{}})" "\n", true);
    wait_observations(1);
    require(wait_until(settled), "request application must finish before the partial append");
    const auto large = R"({"tools":[{"name":"probe","description":")" +
        std::string(4 * 1024 * 1024 + 17, 'x') +
        R"(","parameters":{}}],"type":"llm.tools_snapshot"})";
    const auto split = large.size() - 12;
    const auto dispatches = service->statistics().dispatches;
    write_bytes(path, large.substr(0, split), true);
    require(wait_until([&] {
        return (service->statistics().dispatches > dispatches && settled()) ||
            window.worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    }), "an oversized partial record must reach the actual native receiver");
    wait_observations(1);
    const auto deep = R"({"tools":[{"name":"probe","parameters":{"nested":)" +
        std::string(80, '[') + "0" + std::string(80, ']') +
        R"(}}],"type":"llm.tools_snapshot"})";
    write_bytes(path, large.substr(split) + '\n' + deep + '\n' +
        R"({"type":"interaction.resolved","id":"question_reader","agentId":"main","response":{"cancelled":true}})" "\n" +
        call_record("child_after_ignored_records"), true);
    wait_observations(3);
    window.stop();
    require(window.context.m_failure.empty() && window.context.m_result.has_value(),
        "the real observer must remain successful after reading ignored records");
    const auto page = poll(observer, nonce);
    require(page && page->observations.size() == 3, "all accepted observer facts remain available");
    std::vector<addon::Attention_transition> attention;
    bool child_started = false;
    for (const auto& item : page->observations) {
        if (item.attention_request_id == "question_reader") attention.push_back(item.attention_transition);
        if (item.agent_id == "child_after_ignored_records" &&
            item.subagent_transition == addon::Subagent_transition::STARTED) child_started = true;
    }
    require(attention == std::vector{addon::Attention_transition::REQUESTED, addon::Attention_transition::RESOLVED} &&
        child_started, "attention order and shared subagent observation both survive irrelevant wire records");
    const auto replay = poll(observer, nonce);
    require(replay && replay->resume_cursor == page->resume_cursor && replay->observations.size() == 3,
        "accepted observer facts remain replayable at their original cursor");
    require(retire(observer, nonce), "reader fixture retires its native observation owner");
}

void model_only_wire_reaches_the_shared_observer()
{
    namespace observation = vnm::filesystem_observation;
    Workspace workspace;
    const auto wire = workspace.m_root / "sessions/workspace/session/agents/main/wire.jsonl";
    fs::create_directories(wire.parent_path());
    write_bytes(wire, R"({"type":"llm.request","agentId":"main","kind":"loop","model":"historical"})" "\n");
    auto service = std::make_shared<observation::Service>();
    observation::Configuration options;
    options.general_interval = std::chrono::milliseconds(25);
    kimi::Lifecycle_observer observer(service, options);
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve Kimi model observer");
    auto start = start_request(workspace, nonce);
    start.event.body.set("interaction_frontier",
        kimi::capture_interaction_frontier(kimi::path_string(workspace.m_root), "session"));
    observer.classify(start);
    std::atomic<unsigned> wakes = 0;
    Observe_window window(observer, workspace, nonce, [&] { ++wakes; });
    write_bytes(wire, R"({"type":"llm.request","agentId":"main","kind":"loop","provider":"openai","model":"native-root","modelAlias":"scripted-alias"})" "\n", true);
    require(wait_until([&] { return wakes.load() > 0; }),
        "a root request without a lifecycle event must wake the host inside the observe window");
    const auto page = poll(observer, nonce, {}, 1, addon::k_model_state_version);
    require(page && page->model_updates.size() == 1, "the poll triggered by progress must contain the root model");
    const auto& sample = page->model_updates.front();
    require(page->observations.empty() && page->resume_cursor == nonce + ":0" && sample.model == "native-root" &&
        sample.provider.empty() && sample.transition_nonce == nonce && sample.sequence == fs::file_size(wire),
        "the page must preserve source order, root identity, and the native model without a backend");
    const auto unrequested = poll(observer, nonce);
    require(unrequested && unrequested->model_updates.empty(), "a host without model negotiation receives no model");
}

void inventory_growth_preserves_admitted_source()
{
    namespace observation = vnm::filesystem_observation;
    Workspace workspace;
    const auto agents = workspace.m_root / "sessions/workspace/session/agents";
    const auto wire   = agents / "main/wire.jsonl";
    fs::create_directories(wire.parent_path());
    write_bytes(wire, {});
    const auto service = std::make_shared<observation::Service>();
    observation::Configuration configuration;
    configuration.general_interval = std::chrono::milliseconds(25);
    kimi::Lifecycle_observer observer(service, configuration);
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve inventory diagnostic owner");
    auto start = start_request(workspace, nonce);
    start.event.body.set("interaction_frontier",
        kimi::capture_interaction_frontier(kimi::path_string(workspace.m_root), "session"));
    observer.classify(start);
    Observe_window window(observer, workspace, nonce);
    require(wait_until([&] {
        const auto statistics = service->statistics();
        return statistics.dispatches > 0 && statistics.outstanding == 0 &&
            statistics.pending_registrations == 0 && statistics.retained_events == 0;
    }), "initial admitted inventory must be acquired before it grows");
    for (int index = 0; index < 256; ++index) {
        fs::create_directories(agents / ("child-" + std::to_string(index)));
    }
    write_bytes(wire, call_record("child_after_inventory_growth"), true);
    require(wait_until([&] {
        const auto page = poll(observer, nonce);
        return page && page->observations.size() == 1 &&
            page->observations.front().agent_id == "child_after_inventory_growth";
    }), "an oversized optional inventory does not hide the admitted main wire's child fact");
    window.stop();
    require(window.context.m_result.has_value(), "optional inventory bounds cannot withdraw the lifecycle channel");
    require(retire(observer, nonce), "bounded optional inventory still permits abandoned retirement");
    require(!poll(observer, nonce), "retirement must release the diagnostic correlation");
}

void capacity_preserves_admitted_attention_and_root_end()
{
    namespace observation = vnm::filesystem_observation;
    Workspace workspace;
    const auto wire = workspace.m_root / "sessions/workspace/session/agents/main/wire.jsonl";
    fs::create_directories(wire.parent_path());
    write_bytes(wire, {});
    const auto service = std::make_shared<observation::Service>();
    observation::Configuration configuration;
    configuration.general_interval = std::chrono::milliseconds(25);
    kimi::Lifecycle_observer observer(service, configuration);
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve the bounded native interaction owner");
    auto start = start_request(workspace, nonce);
    start.event.body.set("interaction_frontier",
        kimi::capture_interaction_frontier(kimi::path_string(workspace.m_root), "session"));
    observer.classify(start);
    std::string requests = call_record("held-child");
    for (int index = 0; index < 129; ++index) {
        requests += "{\"type\":\"interaction.request\",\"id\":\"question-" + std::to_string(index) +
            "\",\"agentId\":\"main\",\"kind\":\"question\",\"toolCallId\":\"ask\",\"request\":{}}\n";
    }
    write_bytes(wire, requests, true);
    Observe_window window(observer, workspace, nonce);
    require(wait_until([&] {
        const auto page = poll(observer, nonce);
        const auto statistics = service->statistics();
        return page && !page->observations.empty() && statistics.outstanding == 0 &&
            statistics.pending_registrations == 0 && statistics.retained_events == 0;
    }), "initial native child and interaction acquisition completes");
    std::string cursor;
    std::size_t admitted = 0;
    for (;;) {
        const auto page = poll(observer, nonce, cursor);
        require(page.has_value(), "the 129th attention request cannot fail polling");
        admitted += page->observations.size();
        cursor = page->resume_cursor;
        if (!page->more) break;
    }
    require(admitted == 129, "the queue admits one child and the host's 128 live attention requests");
    window.stop();
    std::string records;
    for (int index = 0; index < 2100; ++index) {
        const auto id = "short-" + std::to_string(index);
        records += call_record(id) + "{\"type\":\"context.append_loop_event\",\"event\":{\"type\":\"tool.result\","
            "\"toolCallId\":\"" + id + "\"}}\n";
    }
    // Native request history is retained independently of the host's smaller
    // live-attention limit. Refusing #4097 must still reach a known resolution.
    for (int index = 129; index <= 4096; ++index) {
        records += "{\"type\":\"interaction.request\",\"id\":\"question-" + std::to_string(index) +
            "\",\"agentId\":\"main\",\"kind\":\"question\",\"toolCallId\":\"ask\",\"request\":{}}\n";
    }
    for (const auto* id : {"question-0", "question-128"}) {
        records += "{\"type\":\"interaction.resolved\",\"id\":\"" + std::string(id) +
            "\",\"agentId\":\"main\",\"response\":{}}\n";
    }
    write_bytes(wire, records, true);
    auto end = start_request(workspace, addon::random_nonce_hex());
    end.event.name = "SessionEnd";
    end.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, "session", nonce}};
    require(observer.classify(end).observations.empty(), "the final drain retains its terminal page");
    std::vector<addon::Lifecycle_observation> final;
    for (;;) {
        const auto page = poll(observer, nonce, cursor);
        require(page.has_value(), "queue saturation cannot discard admitted resolution or terminal state");
        final.insert(final.end(), page->observations.begin(), page->observations.end());
        cursor = page->resume_cursor;
        if (!page->more) break;
    }
    require(final.size() > 3 && final.size() <= 4096 && final.size() < 4203,
        "new optional child spans are refused within the retained queue bound");
    require(final[final.size() - 3].attention_transition == addon::Attention_transition::RESOLVED &&
        final[final.size() - 3].attention_request_id == "question-0" &&
        final[final.size() - 2].subagent_transition == addon::Subagent_transition::STOPPED &&
        final[final.size() - 2].agent_id == "held-child" &&
        final.back().session_transition == addon::Session_transition::ENDED,
        "admitted attention resolution and child STOP retain their slots before root END");
    require(retire(observer, nonce, cursor), "the saturated native stream retains exact terminal retirement");
}

void retained_interaction_capacity_preserves_resolution()
{
    Workspace workspace;
    const auto wire = workspace.m_root / "sessions/workspace/session/agents/main/wire.jsonl";
    fs::create_directories(wire.parent_path());
    write_bytes(wire, {});
    kimi::Interaction_reader reader("session",
        kimi::capture_interaction_frontier(kimi::path_string(workspace.m_root), "session"));
    const auto request = [](const std::string& id) {
        return "{\"type\":\"interaction.request\",\"id\":\"" + id +
            "\",\"agentId\":\"main\",\"kind\":\"question\",\"toolCallId\":\"ask\",\"request\":{}}\n";
    };
    const auto resolution = [](const std::string& id) {
        return "{\"type\":\"interaction.resolved\",\"id\":\"" + id +
            "\",\"agentId\":\"main\",\"response\":{}}\n";
    };
    std::string records = request("held");
    for (int index = 0; index < 4095; ++index) {
        const auto id = "past-" + std::to_string(index);
        records += request(id) + resolution(id);
    }
    write_bytes(wire, records, true);
    const auto inventory = kimi::discover_interaction_wires(kimi::path_string(workspace.m_root), "session");
    std::vector<addon::Lifecycle_observation> observations;
    require(reader.acquire(inventory, observations, [] { return false; }) && observations.size() == 8191,
        "the reader retains its actual 4096-request history with one unresolved request");
    observations.clear();
    write_bytes(wire, request("excess") + resolution("held"), true);
    std::string first_failure;
    std::string retry_failure;
    try { reader.acquire(inventory, observations, [] { return false; }); }
    catch (const std::runtime_error& error) { first_failure = error.what(); }
    try { reader.acquire(inventory, observations, [] { return false; }); }
    catch (const std::runtime_error& error) { retry_failure = error.what(); }
    if (!first_failure.empty() || !retry_failure.empty()) {
        std::cerr << "interaction_capacity_first=" << first_failure << "; retry=" << retry_failure << '\n';
    }
    require(first_failure.empty() && retry_failure.empty() && observations.size() == 1 &&
        observations.front().attention_request_id == "held" &&
        observations.front().attention_transition == addon::Attention_transition::RESOLVED,
        "capacity refusal preserves physical cursor/anchor progress and the admitted resolution");
}

void native_wire_reconciliation_preserves_records_and_final_drain()
{
    namespace observation = vnm::filesystem_observation;
    Workspace workspace;
    const auto path = workspace.m_root / "sessions/workspace/session/agents/main/wire.jsonl";
    fs::create_directories(path.parent_path());
    write_bytes(path, {});
    const auto service = std::make_shared<observation::Service>();
    observation::Configuration configuration;
    configuration.general_interval = std::chrono::milliseconds(25);
    kimi::Lifecycle_observer observer(service, configuration);
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve native reconciliation owner");
    observer.classify(start_request(workspace, nonce));
    Observe_window window(observer, workspace, nonce);
    const auto settled = [&] {
        const auto statistics = service->statistics();
        return statistics.dispatches > 0 && statistics.outstanding == 0 &&
            statistics.pending_registrations == 0 && statistics.retained_events == 0;
    };
    require(wait_until(settled), "initial wire acquisition follows native registration");
    const auto first = call_record("first-child");
    const auto split = first.size() / 2;
    auto dispatches = service->statistics().dispatches;
    write_bytes(path, first.substr(0, split), true);
    require(wait_until([&] { return service->statistics().dispatches > dispatches && settled(); }),
        "a partial append reaches the actual receiver");
    require(poll(observer, nonce)->observations.empty(), "a partial record is not an observation");
    write_bytes(path, first.substr(split), true);
    require(wait_until([&] { const auto page = poll(observer, nonce); return page && page->observations.size() == 1; }),
        "the final half converges without another write");
    const auto original = poll(observer, nonce);
    require(original && poll(observer, nonce)->resume_cursor == original->resume_cursor,
        "unacknowledged records remain replayable");
    require(wait_until(settled), "record application releases its capture");
    dispatches = service->statistics().dispatches;
    write_bytes(path.parent_path() / "irrelevant.txt", "unrelated sibling contents\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(service->statistics().dispatches == dispatches, "unrelated sibling content does not acquire the wire");

    write_bytes(path, call_record("truncated-child"));
    require(wait_until([&] { const auto page = poll(observer, nonce); return page && page->observations.size() == 2; }),
        "truncation rewinds bytes without replaying an already emitted record");
    const auto replacement = path.parent_path() / "replacement.jsonl";
    write_bytes(replacement, call_record("replaced-child"));
    fs::remove(path);
    fs::rename(replacement, path);
    require(wait_until([&] { const auto page = poll(observer, nonce); return page && page->observations.size() == 3; }),
        "replacement is acquired without a later file notification");

    window.stop();
    write_bytes(path, call_record("final-child"), true);
    auto end = start_request(workspace, nonce);
    end.event.name = "SessionEnd";
    end.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "session", nonce};
    observer.classify(end);
    const auto final = poll(observer, nonce);
    require(final && !final->more && final->observations.size() == 9,
        "final drain retains four starts, four stops and the terminal fact");
    require(final->observations.back().session_transition == addon::Session_transition::ENDED,
        "all child observations precede SessionEnd");
    require(retire(observer, nonce, final->resume_cursor), "exact final acknowledgement retires the binding");
    require(wait_until([&] { const auto statistics = service->statistics(); return statistics.subscriptions == 0 &&
        statistics.outstanding == 0 && statistics.pending_registrations == 0 && statistics.retiring_groups == 0; }),
        "terminal retirement releases observation ownership");
}

void native_cancellation_is_published_before_session_end()
{
    namespace observation = vnm::filesystem_observation;
    Workspace workspace;
    const auto path = workspace.m_root / "sessions/workspace/session/agents/main/wire.jsonl";
    fs::create_directories(path.parent_path());
    write_bytes(path,
        R"({"type":"context.append_loop_event","event":{"type":"tool.call","name":"Agent","toolCallId":"background-call","args":{"run_in_background":true}}})" "\n"
        R"({"type":"subagent.spawned","subagentId":"worker","parentToolCallId":"background-call","runInBackground":true,"taskId":"background-task"})" "\n"
        R"({"type":"subagent.started","subagentId":"worker"})" "\n"
        R"({"type":"context.append_loop_event","event":{"type":"tool.result","toolCallId":"background-call","result":{"output":"task_id: background-task\nstatus: running\n\n"}}})" "\n");
    const auto service = std::make_shared<observation::Service>();
    observation::Configuration configuration;
    configuration.general_interval = std::chrono::milliseconds(25);
    kimi::Lifecycle_observer observer(service, configuration);
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve native cancellation owner");
    observer.classify(start_request(workspace, nonce));
    Observe_window window(observer, workspace, nonce);
    require(wait_until([&] {
        const auto page = poll(observer, nonce);
        return page && page->observations.size() == 1;
    }), "native background Agent start must reach the observation queue");

    // The native producer makes cancellation durable while the parent session
    // continues. Publication must therefore happen before SessionEnd drains it.
    write_bytes(path, R"({"type":"subagent.cancelled","subagentId":"worker"})" "\n", true);
    require(wait_until([&] {
        const auto page = poll(observer, nonce);
        return page && page->observations.size() == 2;
    }), "native cancellation must reach the observation queue without SessionEnd");
    const auto page = poll(observer, nonce);
    require(page && page->observations.back().agent_id == "background-call" &&
        page->observations.back().provider_event_name == "subagent.cancelled" &&
        page->observations.back().subagent_transition == addon::Subagent_transition::STOPPED,
        "native wire cancellation must stop its correlated public agent");
    require(retire(observer, nonce), "release native cancellation ownership");
    window.stop();
}

void missing_ancestors_and_parent_replacement_reconcile_without_polling()
{
    namespace observation = vnm::filesystem_observation;
    Workspace workspace;
    fs::remove(workspace.m_root);
    const auto service = std::make_shared<observation::Service>();
    observation::Configuration configuration;
    configuration.general_interval = std::chrono::milliseconds(25);
    kimi::Lifecycle_observer observer(service, configuration);
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve missing home owner");
    observer.classify(start_request(workspace, nonce));
    bool discovery_allowed = true;
    std::atomic<int> timeouts = 0;
    std::atomic<bool> private_polling = false;
    // Registration and native detail-loss recovery may legitimately dispatch
    // while the wire is absent. Only delivery-driven discovery is allowed
    // after the initial subscription, including across actual wait timeouts.
    observer.set_acquisition_failures_for_tests([&] {
        if (!std::exchange(discovery_allowed, false)) private_polling = true;
    }, {});
    observer.set_wait_observer_for_tests([&](bool delivered) {
        discovery_allowed = delivered;
        if (!delivered) ++timeouts;
    });
    Observe_window window(observer, workspace, nonce);
    require(wait_until([&] { return timeouts >= 2; }), "missing wire reaches two observer wait timeouts");
    require(!private_polling, "missing wire does not cause private rebind polling");
    native_call(workspace, "session");
    require(wait_until([&] { const auto page = poll(observer, nonce); return page && page->observations.size() == 1; }),
        "one creation of the complete missing chain wakes binding and post-arm acquisition");
    const auto directory = workspace.m_root / "sessions/workspace/session/agents/main";
    fs::rename(directory, directory.parent_path() / "old-main");
    fs::create_directory(directory);
    write_bytes(directory / "wire.jsonl", call_record("replacement-parent-child"));
    require(wait_until([&] { const auto page = poll(observer, nonce); return page && page->observations.size() == 2; }),
        "new main directory replaces old native coverage without an extra edit");
    require(!private_polling, "wire discovery remains owned by received deliveries");
    require(retire(observer, nonce), "abandonment wakes an active receiver");
    window.stop();
    require(window.context.m_failure == "correlation_retired", "retired capture cannot publish observe success");
}

void final_append_during_acquisition_keeps_its_own_capture()
{
    namespace observation = vnm::filesystem_observation;
    Workspace workspace;
    const auto path = workspace.m_root / "sessions/workspace/session/agents/main/wire.jsonl";
    fs::create_directories(path.parent_path());
    write_bytes(path, {});
    const auto service = std::make_shared<observation::Service>();
    observation::Configuration configuration;
    configuration.general_interval = std::chrono::milliseconds(25);
    kimi::Lifecycle_observer observer(service, configuration);
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve held wire acquisition");
    observer.classify(start_request(workspace, nonce));
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> held = false;
    observer.set_scan_gate_for_tests([&] {
        if (!held.exchange(true)) {
            entered.set_value();
            require(released.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "held wire acquisition must be released");
        }
    });
    Observe_window window(observer, workspace, nonce);
    require(entered_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "hold actual acquisition after its snapshot length was captured");
    write_bytes(path, call_record("quiet-final-child"), true);
    require(wait_until([&] { return service->statistics().retained_events > 0; }),
        "actual native final write reaches the outstanding acquisition");
    release.set_value();
    require(wait_until([&] { const auto page = poll(observer, nonce); return page && page->observations.size() == 1; }),
        "old capture acknowledgement cannot lose the final append without another edit");
    window.stop();
    observer.set_scan_gate_for_tests({});
    require(retire(observer, nonce), "retire the held-acquisition cursor");
}

void session_end_retains_failed_drain_and_resolves_new_wire()
{
    namespace observation = vnm::filesystem_observation;
    for (const bool initially_missing : {false, true}) {
        Workspace workspace;
        const auto path = workspace.m_root / "sessions/workspace/session/agents/main/wire.jsonl";
        if (!initially_missing) {
            fs::create_directories(path.parent_path());
            write_bytes(path, {});
        }
        const auto service = std::make_shared<observation::Service>();
        observation::Configuration configuration;
        configuration.general_interval = std::chrono::milliseconds(25);
        kimi::Lifecycle_observer observer(service, configuration);
        const auto nonce = addon::random_nonce_hex();
        require(reserve(observer, nonce), "reserve final drain owner");
        observer.classify(start_request(workspace, nonce));
        {
            Observe_window window(observer, workspace, nonce);
            require(wait_until([&] { const auto statistics = service->statistics(); return statistics.dispatches > 0 &&
                statistics.outstanding == 0 && statistics.pending_registrations == 0; }), "settle initial wire frontier");
            window.stop();
        }
        native_call(workspace, "session");
        if (!initially_missing) observer.set_acquisition_failures_for_tests({}, [] { return false; });
        auto end = start_request(workspace, nonce);
        end.event.name = "SessionEnd";
        end.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "session", nonce};
        observer.classify(end);
        if (!initially_missing) {
            const auto unread = poll(observer, nonce);
            require(unread && unread->observations.empty(), "failed final read must not emit a terminal fact");
            require(!retire(observer, nonce, unread->resume_cursor), "unread final evidence cannot be acknowledged away");
            require(service->statistics().subscriptions > 0, "failed drain retains observation ownership");
            observer.set_acquisition_failures_for_tests({}, {});
            Observe_window retry(observer, workspace, nonce);
            require(wait_until([&] { const auto page = poll(observer, nonce); return page && page->observations.size() == 3; }),
                "retained final acquisition retries without a later file edit");
            retry.stop();
        }
        const auto final = poll(observer, nonce);
        require(final && final->observations.size() == 3 &&
            final->observations.front().subagent_transition == addon::Subagent_transition::STARTED &&
            final->observations[1].subagent_transition == addon::Subagent_transition::STOPPED &&
            final->observations.back().session_transition == addon::Session_transition::ENDED,
            "resolved final bytes and child close precede terminal delivery");
        require(retire(observer, nonce, final->resume_cursor), "complete final drain admits only its exact terminal cursor");
    }
}

void failed_discovery_recovers_inside_the_same_window()
{
    for (const bool initial_failure : {false, true}) {
        namespace observation = vnm::filesystem_observation;
        Workspace workspace;
        const auto path = workspace.m_root / "sessions/workspace/session/agents/main/wire.jsonl";
        fs::create_directories(path.parent_path());
        write_bytes(path, {});
        const auto service = std::make_shared<observation::Service>();
        observation::Configuration configuration;
        configuration.general_interval = std::chrono::milliseconds(25);
        kimi::Lifecycle_observer observer(service, configuration);
        const auto nonce = addon::random_nonce_hex();
        require(reserve(observer, nonce), "reserve transient discovery owner");
        observer.classify(start_request(workspace, nonce));
        if (!initial_failure) {
            Observe_window window(observer, workspace, nonce);
            require(wait_until([&] { const auto statistics = service->statistics(); return statistics.dispatches > 0 &&
                statistics.outstanding == 0 && statistics.pending_registrations == 0; }), "settle discovery receiver");
            window.stop();
        }
        std::atomic<int> failed_discoveries = 0;
        observer.set_acquisition_failures_for_tests([&] {
            ++failed_discoveries;
            throw std::runtime_error("wire_path_unavailable");
        }, {});
        write_bytes(path, call_record("unread-after-discovery-failure"));
        Observe_window retry(observer, workspace, nonce);
        require(wait_until([&] { return failed_discoveries > 0; }),
            "actual capture reaches the failing discovery boundary");
        require(poll(observer, nonce).has_value() &&
            retry.worker.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
            "optional discovery failure retains the live observation channel");
        observer.set_acquisition_failures_for_tests({}, {});
        require(wait_until([&] { const auto page = poll(observer, nonce); return page && page->observations.size() == 1; }),
            "failed capture recovers inside the same window without any later native file event");
        require(wait_until([&] { const auto statistics = service->statistics(); return statistics.outstanding == 0 &&
            statistics.pending_registrations == 0 && statistics.retained_events == 0; }),
            "successful recovery acknowledges the retained acquisition capture");
        retry.stop();
        require(retry.context.m_result.has_value(), "a recovered watch completes successfully");
        require(retire(observer, nonce), "retire recovered discovery owner");
    }
}

void workspace_frontier_rejects_excess_before_collecting_interests()
{
    Workspace workspace;
    for (int index = 0; index <= 4096; ++index) {
        fs::create_directories(workspace.m_root / "sessions" / std::to_string(index));
    }
    bool bounded = false;
    try { kimi::find_session_wire(kimi::path_string(workspace.m_root), "session"); }
    catch (const std::runtime_error& error) { bounded = std::string(error.what()) == "wire_interest_capacity_exhausted"; }
    require(bounded, "oversized workspace frontier fails during discovery before allocating subscription interests");
}

void candidates_reclaim_without_tombstones()
{
    Workspace workspace;
    kimi::Lifecycle_observer observer;
    for (int index = 0; index < 600; ++index) {
        const auto nonce = addon::random_nonce_hex();
        const auto request = start_request(workspace, nonce);
        require(reserve(observer, nonce), "abandoned candidates must not exhaust the 256 live reservations");
        require(reserve(observer, nonce), "duplicate active reservation is idempotent");
        if (index % 2 == 0) observer.classify(request);
        require(retire(observer, nonce) && retire(observer, nonce), "rejected or never-classified candidate retires idempotently");
        bool rejected = false;
        try { observer.classify(request); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "classification delivered after retirement cannot recreate its reservation");
    }
}

void retirement_releases_only_its_owner()
{
    Workspace workspace;
    native_call(workspace, "session");
    native_call(workspace, "unrelated");
    kimi::Lifecycle_observer observer;
    const auto first = addon::random_nonce_hex();
    const auto unrelated = addon::random_nonce_hex();
    for (const auto& [nonce, session] : {std::pair{first, std::string("session")}, std::pair{unrelated, std::string("unrelated")}}) {
        require(reserve(observer, nonce), "reserve active owner");
        observer.classify(start_request(workspace, nonce, session));
        Context window;
        prepare_observe(window, workspace, nonce, session);
        window.m_cancel_on_progress = true;
        observer.handle(window);
        require(window.m_result.has_value(), "window cancellation does not retire the live correlation");
    }
    const auto pending = poll(observer, unrelated);
    require(pending && pending->observations.size() == 1, "unrelated owner has unread child evidence");
    require(retire(observer, first), "between-window abandonment releases the exact path owner");
    require(!poll(observer, first), "retired cursor cannot expose abandoned observations");
    const auto replay = poll(observer, unrelated);
    require(replay && replay->resume_cursor == pending->resume_cursor && replay->observations.size() == 1,
        "retiring a sibling preserves the other owner's unacknowledged page");
    const auto replacement = addon::random_nonce_hex();
    require(reserve(observer, replacement), "fresh scope can reserve the released wire");
    observer.classify(start_request(workspace, replacement));
    Context resumed;
    prepare_observe(resumed, workspace, replacement);
    resumed.m_cancel_on_progress = true;
    observer.handle(resumed);
    require(resumed.m_result.has_value() && poll(observer, replacement).has_value(),
        "a fresh admitted nonce can observe the same native wire after retirement");
}

void final_page_requires_exact_acknowledgment()
{
    Workspace workspace;
    native_call(workspace, "session");
    kimi::Lifecycle_observer observer;
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve terminal owner");
    auto request = start_request(workspace, nonce);
    observer.classify(request);
    Context window;
    prepare_observe(window, workspace, nonce);
    window.m_cancel_on_progress = true;
    observer.handle(window);
    request.event.name = "SessionEnd";
    request.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "session", nonce};
    observer.classify(request);
    Context completed_window;
    prepare_observe(completed_window, workspace, nonce);
    observer.handle(completed_window);
    require(completed_window.m_result.has_value(), "a normally completed closed watch does not consume its queued pages");
    const auto first = poll(observer, nonce, {}, 1);
    require(first && first->more, "start precedes the retained stop and terminal pages");
    require(!retire(observer, nonce, first->resume_cursor), "an intermediate page is not terminal consumption");
    const auto stop = poll(observer, nonce, first->resume_cursor, 1);
    require(stop && stop->more && stop->observations.front().subagent_transition == addon::Subagent_transition::STOPPED,
        "final child stop precedes root end");
    const auto terminal = poll(observer, nonce, stop->resume_cursor, 1);
    require(terminal && !terminal->more && terminal->observations.front().session_transition == addon::Session_transition::ENDED,
        "last page carries the exact root terminal fact");
    const auto replay = poll(observer, nonce, stop->resume_cursor, 1);
    require(replay && replay->resume_cursor == terminal->resume_cursor && replay->observations.size() == 1,
        "an unacknowledged final page remains replayable");
    require(!retire(observer, nonce, nonce + ":999"), "a wrong final cursor must not discard retained terminal evidence");
    require(retire(observer, nonce, terminal->resume_cursor), "exact final consumption releases the closed scope");
    require(!poll(observer, nonce, stop->resume_cursor), "terminal retirement removes cursor ownership");
}

// Native oracle: KimiTUI creates/resumes B before setSession closes A. The host
// retires A's continuous correlation when it accepts B's SessionStart.
void native_start_before_previous_end_preserves_current_session()
{
    Workspace workspace;
    native_call(workspace, "previous");
    native_call(workspace, "current");
    kimi::Lifecycle_observer observer;
    const auto previous = addon::random_nonce_hex();
    const auto current = addon::random_nonce_hex();
    require(reserve(observer, previous), "reserve the departing native conversation");
    observer.classify(start_request(workspace, previous, "previous"));
    Context previous_window;
    prepare_observe(previous_window, workspace, previous, "previous");
    previous_window.m_cancel_on_progress = true;
    observer.handle(previous_window);
    require(previous_window.m_result.has_value(), "departing conversation owns its wire cursor");
    require(reserve(observer, current), "reserve the replacement native conversation");
    auto start = start_request(workspace, current, "current");
    start.ledger.current_epoch = 1;
    start.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "previous", previous};
    require(observer.classify(start).classification == addon::Classification::TRANSITION,
        "native replacement SessionStart activates its own conversation");
    require(retire(observer, previous), "host replacement retires the departed observation scope");
    Context current_window;
    prepare_observe(current_window, workspace, current, "current");
    current_window.m_cancel_on_progress = true;
    observer.handle(current_window);
    const auto before = poll(observer, current);
    require(current_window.m_result.has_value() && before && before->observations.size() == 1,
        "replacement conversation has an independent live child");
    auto end = start_request(workspace, addon::random_nonce_hex(), "previous");
    end.event.name = "SessionEnd";
    end.ledger.current_epoch = 2;
    end.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "current", current};
    const auto classified = observer.classify(end);
    require(classified.classification == addon::Classification::IGNORED && classified.observations.empty(),
        "late native SessionEnd for departed A must not publish into active B");
    const auto after = poll(observer, current);
    require(after && after->resume_cursor == before->resume_cursor && after->observations.size() == 1 &&
        after->observations.front().subagent_transition == addon::Subagent_transition::STARTED,
        "late departed end must preserve B's unread wire facts");
}

void native_end_before_start_retains_departed_terminal_page()
{
    Workspace workspace;
    native_call(workspace, "previous");
    native_call(workspace, "current");
    kimi::Lifecycle_observer observer;
    const auto previous = addon::random_nonce_hex();
    require(reserve(observer, previous), "reserve the ending conversation");
    auto end = start_request(workspace, previous, "previous");
    observer.classify(end);
    Context previous_window;
    prepare_observe(previous_window, workspace, previous, "previous");
    previous_window.m_cancel_on_progress = true;
    observer.handle(previous_window);
    end.event.name = "SessionEnd";
    end.event.body.set("reason", addon::Json_value("exit"));
    end.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, "previous", previous}};
    require(observer.classify(end).observations.empty(), "the ending scope retains its own terminal fact");
    const auto final = poll(observer, previous);
    require(final && final->observations.size() == 3 && !final->more &&
        final->observations[1].subagent_transition == addon::Subagent_transition::STOPPED &&
        final->observations[2].session_transition == addon::Session_transition::ENDED &&
        final->observations[2].session_end_reason == "exit",
        "observed child completion must precede A's final SessionEnd page");
    for (const auto& observation : final->observations) {
        require(observation.provider_session_id == "previous" && observation.provider_event_id.starts_with(previous + ':'),
            "departing observations retain their original identity and nonce");
    }
    const auto current = addon::random_nonce_hex();
    require(reserve(observer, current), "reserve the next conversation while A's page is retained");
    auto start = start_request(workspace, current, "current");
    start.ledger = end.ledger;
    observer.classify(start);
    Context current_window;
    prepare_observe(current_window, workspace, current, "current");
    current_window.m_cancel_on_progress = true;
    observer.handle(current_window);
    const auto replay = poll(observer, previous);
    require(replay && replay->resume_cursor == final->resume_cursor && replay->observations.size() == 3,
        "B's classification cannot consume or reassign A's retained final page");
    require(!retire(observer, previous, previous + ":1") && retire(observer, previous, final->resume_cursor),
        "only A's exact final cursor releases A's terminal evidence");
    const auto current_page = poll(observer, current);
    require(current_page && current_page->observations.size() == 1 &&
        current_page->observations.front().provider_session_id == "current",
        "consuming A's final page cannot close or discard B's current observations");
}

void native_resume_round_trip_reuses_identity_with_fresh_correlation()
{
    Workspace workspace;
    native_call(workspace, "first");
    native_call(workspace, "second");
    kimi::Lifecycle_observer observer;
    const auto first = addon::random_nonce_hex();
    require(reserve(observer, first), "reserve the first native activation");
    auto request = start_request(workspace, first, "first");
    request.event.body.set("source", addon::Json_value("startup"));
    const auto initial = observer.classify(request);
    require(initial.observations.size() == 1 && initial.observations.front().resume_record,
        "native activation publishes a scoped exact resume record");
    const auto record = addon::serialize_json(*initial.observations.front().resume_record);
    const auto duplicate = addon::random_nonce_hex();
    require(reserve(observer, duplicate), "reserve a repeated same-current notification");
    request.transition_nonce = duplicate;
    request.event.body.set("source", addon::Json_value("resume"));
    request.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, "first", first}};
    const auto repeated = observer.classify(request);
    require(repeated.classification == addon::Classification::IGNORED && repeated.observations.empty(),
        "selecting the same current native identity cannot mint another activation");
    require(retire(observer, duplicate), "ignored same-current delivery releases only its candidate reservation");
    const auto second = addon::random_nonce_hex();
    require(reserve(observer, second), "reserve the clear-created native conversation");
    request = start_request(workspace, second, "second");
    request.event.body.set("source", addon::Json_value("startup"));
    request.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, "first", first}};
    const auto cleared = observer.classify(request);
    require(cleared.classification == addon::Classification::TRANSITION &&
        cleared.transition == addon::Transition_kind::START &&
        addon::serialize_json(*cleared.observations.front().resume_record) != record,
        "native clear is a fresh conversation with its own stable resume identity");
    require(retire(observer, first), "the host releases the departed A observation lane");
    const auto returned = addon::random_nonce_hex();
    require(reserve(observer, returned), "reserve the resumed A activation");
    request = start_request(workspace, returned, "first");
    request.event.body.set("source", addon::Json_value("resume"));
    request.ledger = {2, addon::Ledger_transition{addon::Transition_kind::START, "second", second}};
    const auto resumed = observer.classify(request);
    require(resumed.classification == addon::Classification::TRANSITION &&
        resumed.transition == addon::Transition_kind::RESUME &&
        addon::serialize_json(*resumed.observations.front().resume_record) == record,
        "A to B to A keeps A's exact history identity while activating a new correlation");
    require(retire(observer, second), "the host releases B when A returns");
    Context window;
    prepare_observe(window, workspace, returned, "first");
    window.m_cancel_on_progress = true;
    observer.handle(window);
    const auto page = poll(observer, returned);
    require(window.m_result && page && page->observations.size() == 1 &&
        page->observations.front().provider_event_id.starts_with(returned + ':'),
        "returning A observes under its fresh nonce rather than reviving its retired cursor");
    request.event.name = "SessionEnd";
    request.event.body.set("kimi_code_home", addon::Json_value(kimi::path_string(workspace.m_root / "different-store")));
    request.ledger = {3, addon::Ledger_transition{addon::Transition_kind::RESUME, "first", returned}};
    const auto foreign = observer.classify(request);
    require(foreign.classification == addon::Classification::IGNORED && foreign.observations.empty(),
        "a native ID from another store cannot end the current conversation");
    require(poll(observer, returned)->observations.size() == 1,
        "an ignored foreign end leaves the current source cursor intact");
}

void native_session_start_context_requires_current_local_evidence()
{
    Workspace workspace;
    const auto session_directory = workspace.m_root / "sessions/workspace/session";
    const auto state = session_directory / "state.json";
    fs::create_directories(session_directory);
    const auto cwd = kimi::path_string(workspace.m_root / "directory with spaces/nested");
    const auto nonce = addon::random_nonce_hex();
    auto metadata = json_util::object({{"id", addon::Json_value("session")},
        {"version", addon::Json_value(2)}, {"createdAt", addon::Json_value(1)},
        {"updatedAt", addon::Json_value(2)}, {"archived", addon::Json_value(false)},
        {"cwd", addon::Json_value(cwd)}, {"agents", addon::Json_value::object()},
        {"custom", addon::Json_value::object()}});
    const auto capture = [&](std::string_view native_cwd) {
        return kimi::capture_session_start_context(workspace.m_root, "session", nonce, native_cwd);
    };
    require(capture(cwd).status == addon::Execution_context_status::UNKNOWN,
        "a hook cwd without a verified native environment cannot establish a local default");
    write_bytes(state, addon::serialize_json(metadata));
    const auto local = capture(cwd);
    require(addon::is_valid_execution_context_update(local) &&
        local.provider_session_id == "session" && local.transition_nonce == nonce && local.sequence == 0 &&
        local.status == addon::Execution_context_status::CONFIRMED && local.environment == "local" && local.cwd == cwd,
        "native local SessionStart preserves its exact nested directory and activation");
    require(capture({}).status == addon::Execution_context_status::UNKNOWN &&
        capture(kimi::path_string(workspace.m_root)).status == addon::Execution_context_status::UNKNOWN,
        "metadata or process launch cwd cannot replace the native SessionStart cwd");
    const auto environment_cwd = kimi::path_string(workspace.m_root / "another local default");
    metadata.set("environmentId", addon::Json_value("local"));
    metadata.set("environmentCwd", addon::Json_value(environment_cwd));
    write_bytes(state, addon::serialize_json(metadata));
    require(capture(cwd).status == addon::Execution_context_status::UNKNOWN &&
        capture(environment_cwd).cwd == environment_cwd,
        "a native local environment binding must agree with the hook's actual default");
    metadata.set("environmentId", addon::Json_value("remote-environment"));
    metadata.set("environmentCwd", addon::Json_value("/remote/default"));
    write_bytes(state, addon::serialize_json(metadata));
    const auto remote = capture(cwd);
    require(addon::is_valid_execution_context_update(remote) &&
        remote.status == addon::Execution_context_status::UNAVAILABLE && remote.environment == "unsupported" &&
        remote.cwd.empty(), "a remote SessionStart carrier path cannot become a local default");
    metadata.set("version", addon::Json_value(3));
    write_bytes(state, addon::serialize_json(metadata));
    require(capture(cwd).status == addon::Execution_context_status::UNKNOWN,
        "unrecognized native metadata cannot establish an environment invariant");
}

void session_start_context_rejects_foreign_activations()
{
    Workspace workspace;
    auto request = start_request(workspace, addon::random_nonce_hex());
    request.execution_context_version = 1;
    addon::Execution_context_update update;
    update.provider_session_id = "session";
    update.transition_nonce = request.transition_nonce;
    update.source = "SessionStart:session-environment";
    addon::encode_execution_updates(request.event.body, {update});
    addon::Classify_event_result result;
    kimi::classify_session_start_context(request, result);
    require(result.execution_updates.size() == 1 && !result.execution_failure,
        "a joint unknown snapshot remains attributed to its exact native activation");
    request.execution_context_version = 0;
    result = {};
    kimi::classify_session_start_context(request, result);
    require(result.execution_updates.empty() && !result.execution_failure,
        "context is omitted when its capability was not requested");
    request.execution_context_version = 1;
    request.transition_nonce = addon::random_nonce_hex();
    result = {};
    kimi::classify_session_start_context(request, result);
    require(result.execution_updates.empty() && result.execution_failure,
        "an old A snapshot cannot acquire a new A activation's nonce at delivery");
    request.transition_nonce = update.transition_nonce;
    update.sequence = 1;
    addon::encode_execution_updates(request.event.body, {update});
    result = {};
    kimi::classify_session_start_context(request, result);
    require(result.execution_updates.empty() && result.execution_failure,
        "a one-shot native SessionStart snapshot cannot assert a later source sequence");
    request.event.body.set("execution_context", json_util::object({{"version", addon::Json_value(99)}}));
    result = {};
    kimi::classify_session_start_context(request, result);
    require(result.execution_updates.empty() && result.execution_failure,
        "unsupported optional context fails without invalidating native lifecycle classification");
    kimi::Lifecycle_observer observer;
    require(reserve(observer, request.transition_nonce), "reserve identity beside malformed optional context");
    const auto classified = observer.classify(request);
    const auto decoded = addon::decode_classify_event_result(addon::encode_classify_event_result(classified), nullptr);
    require(decoded && decoded->classification == addon::Classification::TRANSITION &&
        decoded->observations.size() == 1 && decoded->execution_failure && decoded->execution_updates.empty(),
        "malformed optional context must leave the native identity admissible through the shared codec");
    request = start_request(workspace, addon::random_nonce_hex(), "native.id.outside-resume-selector");
    require(reserve(observer, request.transition_nonce), "reserve identity outside the existing resume-selector grammar");
    const auto unsupported_resume = observer.classify(request);
    require(unsupported_resume.classification == addon::Classification::TRANSITION &&
        unsupported_resume.observations.size() == 1 && !unsupported_resume.observations.front().resume_record,
        "optional resume-record limits cannot suppress an otherwise valid native identity");
}

// Oracle: the host's feature gate. It rejects an observation carrying a fact
// the package's own declaration leaves unsupported. The observer reports no
// native turn hook; session and permission hooks and wire children retain their own facts.
void declared_profile_admits_every_shape(const fs::path& package)
{
    const addon::Directory_package_source source(package);
    const auto path = addon::sidecar_relative_path(
        addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_version);
    const auto bytes = source.read(path, addon::limits::k_max_sidecar_bytes, nullptr);
    const auto sidecar = bytes ? addon::parse_sidecar(*bytes, nullptr) : std::nullopt;
    const auto declaration = sidecar ? addon::decode_lifecycle_declaration(sidecar->payload, nullptr) : std::nullopt;
    require(declaration.has_value(), "package lifecycle declaration is readable");
    std::size_t observed = 0;
    const auto admitted = [&declaration, &observed](const std::vector<addon::Lifecycle_observation>& observations) {
        for (const auto& observation : observations) {
            require(addon::lifecycle_profile_admits(declaration->features, observation),
                "the declaration refuses an observation the package emits");
            ++observed;
        }
    };
    Workspace workspace;
    native_call(workspace, "session");
    kimi::Lifecycle_observer observer;
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve the gated owner");
    auto request = start_request(workspace, nonce);
    admitted(observer.classify(request).observations);
    request.ledger.current_epoch = 1;
    request.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "session", nonce};
    request.event.body.set("id", addon::Json_value("approval_b590bd4c-78da-4d18-a319-b06c5a7f6453"));
    request.event.body.set("agent_id", addon::Json_value("main"));
    request.event.body.set("tool_call_id", addon::Json_value("call_deploy_preview"));
    for (const auto* name : {"PermissionRequest", "PermissionResult"}) {
        request.event.name = name;
        const auto permission = observer.classify(request);
        require(permission.observations.size() == 1, "native permission hook produces one identified fact");
        admitted(permission.observations);
    }
    Context window;
    prepare_observe(window, workspace, nonce);
    window.m_cancel_on_progress = true;
    observer.handle(window);
    request.event.name = "SessionEnd";
    request.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "session", nonce};
    admitted(observer.classify(request).observations);
    std::string cursor;
    for (bool more = true; more;) {
        const auto page = poll(observer, nonce, cursor);
        require(page.has_value(), "the gated owner's observations are readable");
        admitted(page->observations);
        cursor = page->resume_cursor;
        more = page->more;
    }
    require(observed >= 6, "session, permission and child facts were all gated");
}

void in_flight_watch_stops_on_retirement()
{
    Workspace workspace;
    native_call(workspace, "session");
    kimi::Lifecycle_observer observer;
    const auto nonce = addon::random_nonce_hex();
    require(reserve(observer, nonce), "reserve in-flight watch");
    observer.classify(start_request(workspace, nonce));
    Context window;
    prepare_observe(window, workspace, nonce);
    std::promise<void> ready;
    auto ready_future = ready.get_future();
    window.m_progress = [&] { ready.set_value(); };
    std::jthread thread([&] { observer.handle(window); });
    if (ready_future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        window.m_cancel = true;
        throw std::runtime_error("watch did not reach its initial native scan");
    }
    require(retire(observer, nonce), "retire synchronizes with the watch state lock");
    thread.join();
    require(window.m_status == addon::Result_status::CANCELLED && window.m_failure == "correlation_retired",
        "a held watch reference cannot publish successful evidence after scope retirement");
    require(!poll(observer, nonce), "in-flight retirement drops only the abandoned cursor");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: kimi_observer_tests <package directory>\n";
        return 2;
    }
    try {
        if (std::string_view(argv[1]) == "--investigate-inventory") {
            inventory_growth_preserves_admitted_source();
            return 0;
        }
        if (std::string_view(argv[1]) == "--reader-isolation") {
            irrelevant_interaction_records_preserve_observation();
            return 0;
        }
        if (std::string_view(argv[1]) == "--live-switch") {
            native_start_before_previous_end_preserves_current_session();
            native_end_before_start_retains_departed_terminal_page();
            native_resume_round_trip_reuses_identity_with_fresh_correlation();
            return 0;
        }
        irrelevant_interaction_records_preserve_observation();
        model_only_wire_reaches_the_shared_observer();
        inventory_growth_preserves_admitted_source();
        retained_interaction_capacity_preserves_resolution();
        capacity_preserves_admitted_attention_and_root_end();
        declared_profile_admits_every_shape(addon::path_from_utf8(argv[1]));
        candidates_reclaim_without_tombstones();
        retirement_releases_only_its_owner();
        final_page_requires_exact_acknowledgment();
        native_start_before_previous_end_preserves_current_session();
        native_end_before_start_retains_departed_terminal_page();
        native_resume_round_trip_reuses_identity_with_fresh_correlation();
        native_session_start_context_requires_current_local_evidence();
        session_start_context_rejects_foreign_activations();
        in_flight_watch_stops_on_retirement();
        native_wire_reconciliation_preserves_records_and_final_drain();
        native_cancellation_is_published_before_session_end();
        missing_ancestors_and_parent_replacement_reconcile_without_polling();
        final_append_during_acquisition_keeps_its_own_capture();
        session_end_retains_failed_drain_and_resolves_new_wire();
        failed_discovery_recovers_inside_the_same_window();
        workspace_frontier_rejects_excess_before_collecting_interests();
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
