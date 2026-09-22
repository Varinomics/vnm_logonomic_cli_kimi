#include "kimi_observer.h"
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
#include <thread>
#include <utility>
#include <vector>

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
    request.event.body = kimi::object({{"session_id", addon::Json_value(session)},
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
    context.m_request.payload = addon::encode_observe_request(request, 2);
}

std::optional<addon::Poll_observations_result> poll(kimi::Lifecycle_observer& observer,
    const std::string& nonce, std::string cursor = {}, std::size_t limit = 16)
{
    addon::Poll_observations_request request;
    request.correlation = addon::Session_correlation{nonce};
    request.resume_cursor = std::move(cursor);
    request.limit = limit;
    Context context;
    context.m_request.capability = {"lifecycle-events", 2};
    context.m_request.operation = "poll_observations";
    context.m_request.payload = addon::encode_poll_observations_request(request, 2);
    observer.handle(context);
    addon::Contract_failure failure;
    return context.m_result
        ? addon::decode_poll_observations_result(*context.m_result, request.limit, &failure, 2)
        : std::nullopt;
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

// Oracle: the host's feature gate. It rejects an observation carrying a fact
// the package's own declaration leaves unsupported. The observer reports no
// native turn hook, only session boundaries and the children its wire shows.
void declared_profile_admits_every_shape(const fs::path& package)
{
    const addon::Directory_package_source source(package);
    const auto path = addon::sidecar_relative_path(
        addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_correlated_version);
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
    require(observed >= 4, "the session start, child start, child stop and session end were all gated");
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
        declared_profile_admits_every_shape(addon::path_from_utf8(argv[1]));
        candidates_reclaim_without_tombstones();
        retirement_releases_only_its_owner();
        final_page_requires_exact_acknowledgment();
        in_flight_watch_stops_on_retirement();
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
