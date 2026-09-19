#include "kimi_observer.h"
#include "kimi_json.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/sha256.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace kimi {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t k_record_bound = 4 * 1024 * 1024;
constexpr std::size_t k_queue_bound = 4096;

struct Cursor
{
    std::string key;
    std::uint64_t sequence = 0;
};

Cursor decode_cursor(std::string_view value)
{
    const auto separator = value.find(':');
    const auto key = value.substr(0, separator);
    if (key.empty() || key.size() > 128) throw std::runtime_error("invalid_observation_cursor");
    Cursor result{std::string(key), 0};
    if (separator != std::string::npos) {
        const auto suffix = value.substr(separator + 1);
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), result.sequence);
        if (suffix.empty() || parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
            std::to_string(result.sequence) != suffix) throw std::runtime_error("invalid_observation_cursor");
    }
    return result;
}

} // namespace

std::optional<fs::path> find_session_wire(const std::string& home, const std::string& session)
{
    if (session.empty() || session == "." || session == ".." ||
        session.find_first_of("/\\") != std::string::npos || session.find('\0') != std::string::npos) {
        throw std::runtime_error("invalid_kimi_session");
    }
    const auto root = addon::path_from_utf8(home) / "sessions";
    std::optional<fs::path> result;
    std::error_code error;
    for (fs::directory_iterator item(root, error), end; !error && item != end; item.increment(error)) {
        if (item->is_symlink(error) || !item->is_directory(error)) continue;
        for (fs::directory_iterator candidate(item->path(), error); !error && candidate != end; candidate.increment(error)) {
            if (path_string(candidate->path().filename()) != session || candidate->is_symlink(error) ||
                !candidate->is_directory(error)) continue;
            const auto path = candidate->path() / "agents" / "main" / "wire.jsonl";
            if (!fs::is_regular_file(path, error)) { error.clear(); continue; }
            const auto canonical = fs::canonical(path, error);
            if (error) throw std::runtime_error("wire_path_unavailable");
            if (result) throw std::runtime_error("ambiguous_wire_session");
            result = canonical;
        }
    }
    return result;
}

namespace {

class Directory_watch
{
public:
    explicit Directory_watch(const fs::path& directory)
    {
#ifdef _WIN32
        m_handle = FindFirstChangeNotificationW(directory.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (m_handle == INVALID_HANDLE_VALUE) throw std::runtime_error("wire_watch_failed");
#else
        m_descriptor = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_descriptor < 0) throw std::runtime_error("wire_watch_failed");
        if (inotify_add_watch(m_descriptor, directory.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE |
                IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF) < 0) {
            close(m_descriptor);
            m_descriptor = -1;
            throw std::runtime_error("wire_watch_failed");
        }
#endif
    }
    ~Directory_watch()
    {
#ifdef _WIN32
        FindCloseChangeNotification(m_handle);
#else
        close(m_descriptor);
#endif
    }
    Directory_watch(const Directory_watch&) = delete;
    Directory_watch& operator=(const Directory_watch&) = delete;
    bool changed()
    {
#ifdef _WIN32
        const auto result = WaitForSingleObject(m_handle, 100);
        if (result == WAIT_TIMEOUT) return false;
        if (result != WAIT_OBJECT_0 || !FindNextChangeNotification(m_handle)) m_invalidated = true;
        return true;
#else
        pollfd descriptor{m_descriptor, POLLIN, 0};
        const auto result = poll(&descriptor, 1, 100);
        if (result < 0) throw std::runtime_error("wire_watch_failed");
        if (result == 0) return false;
        std::array<char, 8192> events{};
        for (;;) {
            const auto count = read(m_descriptor, events.data(), events.size());
            if (count <= 0) break;
            std::size_t offset = 0;
            while (offset + sizeof(inotify_event) <= static_cast<std::size_t>(count)) {
                inotify_event event{};
                std::memcpy(&event, events.data() + offset, sizeof(event));
                if ((event.mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) m_invalidated = true;
                offset += sizeof(event) + event.len;
            }
        }
        return true;
#endif
    }
    bool invalidated() const
    {
        return m_invalidated;
    }

private:
#ifdef _WIN32
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_descriptor = -1;
#endif
    bool m_invalidated = false;
};

struct Binding
{
    explicit Binding(std::string id, std::string root)
    : session(std::move(id)), home(std::move(root)), decoder(session)
    {}

    std::string session;
    std::string home;
    std::string cursor_key;
    std::string nonce;
    std::optional<fs::path> path;
    Wire_decoder decoder;
    std::deque<addon::Lifecycle_observation> queue;
    std::uint64_t first_sequence = 0;
    std::uint64_t next_sequence = 0;
    std::uint64_t offset = 0;
    std::uint64_t anchor_offset = 0;
    std::string anchor;
    std::string pending;
    std::string failure;
    bool discarding = false;
    bool observing = false;
    bool closed = false;
    bool retired = false;
    std::optional<std::string> final_cursor;
    Clock::time_point bind_deadline = Clock::now() + std::chrono::seconds(60);
};

void enqueue(Binding& binding, std::vector<addon::Lifecycle_observation> observations)
{
    if (binding.retired || !binding.failure.empty()) return;
    if (binding.queue.size() + observations.size() > k_queue_bound) {
        binding.failure = "observation_queue_capacity_exhausted";
        return;
    }
    for (auto& observation : observations) {
        observation.provider_event_id = binding.nonce + ':' + std::to_string(binding.next_sequence);
        observation.provider_sequence = std::to_string(binding.next_sequence);
        ++binding.next_sequence;
        binding.queue.push_back(std::move(observation));
    }
}

void scan(Binding& binding, addon::Invocation_context* invocation)
{
    if (binding.retired || !binding.path || !binding.failure.empty()) return;
    std::ifstream input(*binding.path, std::ios::binary);
    if (!input) return;
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    bool rewind = length < 0 || static_cast<std::uint64_t>(length) < binding.offset;
    if (!rewind && !binding.anchor.empty()) {
        input.seekg(static_cast<std::streamoff>(binding.anchor_offset));
        std::string actual(binding.anchor.size(), '\0');
        input.read(actual.data(), static_cast<std::streamsize>(actual.size()));
        rewind = input.gcount() != static_cast<std::streamsize>(actual.size()) || actual != binding.anchor;
    }
    input.clear();
    if (rewind) {
        binding.offset = 0;
        binding.pending.clear();
        binding.anchor.clear();
        binding.discarding = false;
        binding.decoder.rewind();
    }
    input.seekg(static_cast<std::streamoff>(binding.offset));
    std::array<char, 65536> bytes{};
    const auto snapshot_end = length < 0 ? binding.offset : static_cast<std::uint64_t>(length);
    while (input && binding.offset < snapshot_end && (!invocation || !invocation->cancel_requested()) &&
           !binding.closed && binding.failure.empty()) {
        const auto remaining = std::min<std::uint64_t>(bytes.size(), snapshot_end - binding.offset);
        input.read(bytes.data(), static_cast<std::streamsize>(remaining));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0) break;
        binding.offset += count;
        for (std::size_t index = 0; index < count; ++index) {
            if (bytes[index] == '\n') {
                if (!binding.discarding) {
                    addon::Json_parse_limits limits;
                    limits.max_string_bytes = k_record_bound;
                    if (const auto record = addon::parse_json(binding.pending, limits, nullptr); record && record->is_object()) {
                        enqueue(binding, binding.decoder.accept(*record));
                    }
                    if (!binding.decoder.failure().empty()) binding.failure = binding.decoder.failure();
                }
                binding.pending.clear();
                binding.discarding = false;
            }
            else
            if (!binding.discarding) {
                if (binding.pending.size() == k_record_bound) { binding.pending.clear(); binding.discarding = true; }
                else binding.pending += bytes[index];
            }
        }
    }
    const auto size = std::min<std::uint64_t>(4096, binding.offset);
    binding.anchor_offset = binding.offset - size;
    binding.anchor.assign(static_cast<std::size_t>(size), '\0');
    input.clear();
    input.seekg(static_cast<std::streamoff>(binding.anchor_offset));
    input.read(binding.anchor.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) binding.anchor.clear();
}

} // namespace

struct Lifecycle_observer::State
{
    std::mutex mutex;
    // A null binding is a host-reserved candidate. Only authenticated
    // classification can populate its native home/session identity.
    std::map<std::string, std::shared_ptr<Binding>> sessions;
    std::map<std::string, std::shared_ptr<Binding>> cursors;
};

Lifecycle_observer::Lifecycle_observer() : m_state(std::make_unique<State>()) {}
Lifecycle_observer::~Lifecycle_observer() = default;

addon::Classify_event_result Lifecycle_observer::classify(const addon::Classify_event_request& request)
{
    addon::Classify_event_result result;
    const auto& event = request.event;
    if (event.name != "SessionStart" && event.name != "SessionEnd") return result;
    const auto session = text(event.body, "session_id");
    const auto source = text(event.body, "source");
    if (!bounded_string(member(event.body, "session_id"), 256) || source.size() > 128 ||
        source.find('\0') != std::string::npos || session.find_first_of("/\\") != std::string::npos || session == "." || session == "..") {
        throw std::runtime_error("invalid_kimi_session");
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = event.name;
    observation.provider_session_id = session;
    observation.session_source = source;
    std::lock_guard lock(m_state->mutex);
    if (event.name == "SessionStart") {
        const auto home = text(event.body, "kimi_code_home");
        if (home.empty() || !addon::path_from_utf8(home).is_absolute()) throw std::runtime_error("invalid_kimi_home");
        auto found = m_state->sessions.find(request.transition_nonce);
        if (found == m_state->sessions.end()) throw std::runtime_error("correlation_not_reserved");
        if (found->second &&
            (found->second->home != home || found->second->session != session)) {
            throw std::runtime_error("wire_session_already_bound");
        }
        if (!found->second) {
            auto binding = std::make_shared<Binding>(session, home);
            binding->nonce = request.transition_nonce;
            m_state->sessions[request.transition_nonce] = std::move(binding);
        }
        result.classification = addon::Classification::TRANSITION;
        result.transition = source == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        observation.session_transition = addon::Session_transition::STARTED;
        observation.resume_record = object({{"provider_session_id", Json(session)}});
    }
    else {
        const auto key = request.ledger.current_transition ? request.ledger.current_transition->nonce : std::string{};
        if (const auto found = m_state->sessions.find(key); found != m_state->sessions.end() && found->second && found->second->session == session) {
            scan(*found->second, nullptr);
            if (!found->second->failure.empty()) throw std::runtime_error(found->second->failure);
            enqueue(*found->second, found->second->decoder.close("SessionEnd"));
            if (!found->second->failure.empty()) throw std::runtime_error(found->second->failure);
            observation.session_transition = addon::Session_transition::ENDED;
            if (!found->second->cursor_key.empty()) {
                enqueue(*found->second, {observation});
                if (!found->second->failure.empty()) throw std::runtime_error(found->second->failure);
                found->second->closed = true;
                // The continuous queue owns the terminal fact so every retained
                // agent stop precedes it across bounded poll pages.
                return result;
            }
            found->second->closed = true;
        }
        result.classification = addon::Classification::EVENT;
        observation.session_transition = addon::Session_transition::ENDED;
    }
    result.observations.push_back(std::move(observation));
    return result;
}

void Lifecycle_observer::handle(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().capability.capability_id == "lifecycle-correlation-lifetime") {
        if (context.request().operation == addon::k_lifecycle_correlation_lifetime_reserve_operation) {
            const auto request = addon::decode_lifecycle_correlation_reserve_request(context.request().payload, &failure);
            if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
            std::lock_guard lock(m_state->mutex);
            if (context.cancel_requested()) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
            const auto& nonce = request->correlation.transition_nonce;
            if (!m_state->sessions.contains(nonce)) {
                if (m_state->sessions.size() >= 256) {
                    context.fail(addon::Result_status::FAILED, "wire_session_capacity_exhausted");
                    return;
                }
                m_state->sessions.emplace(nonce, nullptr);
            }
            context.succeed(addon::encode_lifecycle_correlation_lifetime_ack({}));
        }
        else
        if (context.request().operation == addon::k_lifecycle_correlation_lifetime_retire_operation) {
            const auto request = addon::decode_lifecycle_correlation_retire_request(context.request().payload, &failure);
            if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
            std::lock_guard lock(m_state->mutex);
            const auto found = m_state->sessions.find(request->correlation.transition_nonce);
            if (found != m_state->sessions.end()) {
                const auto binding = found->second;
                if (request->disposition == addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED &&
                    (!binding || !binding->closed || !binding->final_cursor || binding->final_cursor != request->consumed_cursor))
                {
                    context.fail(addon::Result_status::FAILED, "terminal_cursor_not_consumed");
                    return;
                }
                if (binding) {
                    binding->retired = true;
                    m_state->cursors.erase(binding->cursor_key);
                    binding->path.reset();
                    binding->queue.clear();
                    binding->pending.clear();
                    binding->anchor.clear();
                }
                m_state->sessions.erase(found);
            }
            context.succeed(addon::encode_lifecycle_correlation_lifetime_ack({}));
        }
        else context.reject_unknown_operation();
        return;
    }
    if (context.request().operation == "classify_event") {
        const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
        context.succeed(addon::encode_classify_event_result(classify(*request)));
        return;
    }
    if (context.request().operation == "poll_observations") {
        const auto request = addon::decode_poll_observations_request(context.request().payload, &failure, 2);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
        if (!request->correlation) { context.fail(addon::Result_status::UNAVAILABLE, "session_start_required"); return; }
        const auto cursor = request->resume_cursor.empty()
            ? Cursor{request->correlation->transition_nonce, 0} : decode_cursor(request->resume_cursor);
        if (cursor.key != request->correlation->transition_nonce) {
            context.fail(addon::Result_status::FAILED, "invalid_observation_cursor"); return;
        }
        std::lock_guard lock(m_state->mutex);
        const auto found = m_state->cursors.find(cursor.key);
        if (found == m_state->cursors.end()) { context.fail(addon::Result_status::FAILED, "unknown_observation_cursor"); return; }
        auto& binding = *found->second;
        if (!binding.failure.empty()) {
            context.fail(addon::Result_status::FAILED, binding.failure);
            return;
        }
        if (cursor.sequence < binding.first_sequence || cursor.sequence > binding.next_sequence) {
            context.fail(addon::Result_status::FAILED, "invalid_observation_cursor");
            return;
        }
        while (binding.first_sequence < cursor.sequence) { binding.queue.pop_front(); ++binding.first_sequence; }
        addon::Poll_observations_result result;
        const auto count = std::min(request->limit, binding.queue.size());
        result.observations.assign(binding.queue.begin(), binding.queue.begin() + static_cast<std::ptrdiff_t>(count));
        result.resume_cursor = cursor.key + ':' + std::to_string(cursor.sequence + count);
        result.more = count < binding.queue.size();
        if (binding.closed && !result.more) binding.final_cursor = result.resume_cursor;
        context.succeed(addon::encode_poll_observations_result(result, 2));
        return;
    }
    if (context.request().operation != "observe") { context.reject_unknown_operation(); return; }
    const auto request = addon::decode_observe_request(context.request().payload, &failure, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    if (!request->correlation) { context.fail(addon::Result_status::UNAVAILABLE, "session_start_required"); return; }
    const auto cursor = request->resume_cursor.empty()
        ? Cursor{request->correlation->transition_nonce, 0} : decode_cursor(request->resume_cursor);
    if (cursor.key != request->correlation->transition_nonce) {
        context.fail(addon::Result_status::FAILED, "invalid_observation_cursor"); return;
    }
    std::shared_ptr<Binding> binding;
    std::uint64_t initial_sequence = 0;
    {
        std::lock_guard lock(m_state->mutex);
        const auto found = m_state->sessions.find(cursor.key);
        if (found == m_state->sessions.end() || !found->second || found->second->session != request->session.provider_session_id) {
            context.fail(addon::Result_status::UNAVAILABLE, "session_start_required");
            return;
        }
        binding = found->second;
        if (binding->observing || (!binding->cursor_key.empty() && binding->cursor_key != cursor.key)) {
            context.fail(addon::Result_status::FAILED, "wire_session_already_bound");
            return;
        }
        binding->cursor_key = cursor.key;
        binding->observing = true;
        initial_sequence = binding->next_sequence;
        m_state->cursors[cursor.key] = binding;
    }
    std::string error;
    const auto deadline = Clock::now() + std::chrono::milliseconds(request->window_ms);
    std::unique_ptr<Directory_watch> watch;
    int retry_delay = 25;
    std::uint64_t reported_sequence = initial_sequence;
    try {
        while (!context.cancel_requested() && Clock::now() < deadline) {
            bool bound = false;
            {
                std::lock_guard lock(m_state->mutex);
                if (binding->retired) break;
                if (binding->next_sequence > reported_sequence) {
                    reported_sequence = binding->next_sequence;
                    const auto available = reported_sequence - initial_sequence;
                    context.report_progress(available, std::nullopt, addon::observe_progress_note(available));
                }
                if (binding->closed) break;
                if (!binding->path) {
                    binding->path = find_session_wire(binding->home, binding->session);
                    if (binding->path) {
                        for (const auto& [key, other] : m_state->sessions) {
                            (void)key;
                            if (other && other != binding && !other->closed && other->path == binding->path) {
                                binding->path.reset();
                                throw std::runtime_error("wire_file_already_bound");
                            }
                        }
                    }
                }
                if (binding->path && !watch) {
                    watch = std::make_unique<Directory_watch>(binding->path->parent_path());
                    scan(*binding, &context);
                }
                bound = binding->path.has_value();
                if (!binding->failure.empty()) { error = binding->failure; break; }
                if (!bound && Clock::now() >= binding->bind_deadline) { error = "wire_bind_timeout"; break; }
                if (binding->next_sequence > reported_sequence) {
                    reported_sequence = binding->next_sequence;
                    const auto available = reported_sequence - initial_sequence;
                    context.report_progress(available, std::nullopt, addon::observe_progress_note(available));
                }
            }
            if (!bound) {
                context.wait_for_cancel(retry_delay);
                retry_delay = std::min(retry_delay * 2, 2000);
                continue;
            }
            if (watch->changed()) {
                std::lock_guard lock(m_state->mutex);
                if (binding->retired) break;
                scan(*binding, &context);
                if (watch->invalidated()) {
                    watch.reset();
                    binding->path.reset();
                    binding->bind_deadline = Clock::now() + std::chrono::seconds(60);
                }
            }
        }
    }
    catch (const std::exception& exception) { error = exception.what(); }
    {
        std::lock_guard lock(m_state->mutex);
        binding->observing = false;
        if (binding->retired) {
            context.fail(addon::Result_status::CANCELLED, "correlation_retired");
            return;
        }
        if (!error.empty()) context.fail(addon::Result_status::FAILED,
            binding->failure.empty() ? "wire_observation_failed" : binding->failure);
        else context.succeed(addon::encode_observe_result({cursor.key + ':' + std::to_string(cursor.sequence)}));
    }
}

} // namespace kimi
