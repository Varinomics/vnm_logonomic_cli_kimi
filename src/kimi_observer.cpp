#include "kimi_observer.h"
#include "kimi_execution_context.h"
#include "kimi_resume.h"
#include "kimi_json.h"
#include "kimi_interactions.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>
#include <logonomic_cli_addon/sha256.h>

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace json_util = logonomic::cli_addon::json;

namespace kimi {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace observation = vnm::filesystem_observation;
namespace path_name = vnm::path_name;
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t k_record_bound = 4 * 1024 * 1024;
constexpr std::size_t k_queue_bound = 4096;
constexpr std::size_t k_interest_bound = 4096;
constexpr std::size_t k_interest_characters = 1024 * 1024;

[[noreturn]] void reject_event_field(
    const addon::Classify_event_request& request,
    std::string field,
    addon::Provider_event_problem problem,
    std::string rule,
    std::optional<std::uint64_t> limit = {})
{
    throw addon::Provider_event_error(addon::make_provider_event_diagnostic(
        request.event.name, field, problem, request.event.body.find(field), limit, std::move(rule)));
}

void require_event_identity(const addon::Classify_event_request& request, const std::string& field)
{
    const auto* value = request.event.body.find(field);
    if (value && json_util::bounded_string(*value, 256)) {
        return;
    }
    const auto problem = !value ? addon::Provider_event_problem::MISSING
        : !value->is_string() ? addon::Provider_event_problem::WRONG_TYPE
        : value->as_string().empty() ? addon::Provider_event_problem::EMPTY
        : value->as_string().size() > 256 ? addon::Provider_event_problem::TOO_LARGE
                                         : addon::Provider_event_problem::CONTROL_CHARACTER;
    reject_event_field(request, field, problem, "bounded_identity", 256);
}

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
    std::size_t workspaces = 0;
    for (fs::directory_iterator item(root, error), end; !error && item != end; item.increment(error)) {
        if (item->is_symlink(error) || !item->is_directory(error)) continue;
        if (++workspaces > k_interest_bound) throw std::runtime_error("wire_interest_capacity_exhausted");
        const auto candidate = item->path() / session;
        if (fs::is_symlink(candidate, error)) continue;
        if (error == std::errc::no_such_file_or_directory) error.clear();
        if (error) break;
        const auto path = candidate / "agents" / "main" / "wire.jsonl";
        if (!fs::is_regular_file(path, error)) {
            if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) error.clear();
            continue;
        }
        const auto canonical = fs::canonical(path, error);
        if (error) throw std::runtime_error("wire_path_unavailable");
        if (result) throw std::runtime_error("ambiguous_wire_session");
        result = canonical;
    }
    if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) {
        throw std::runtime_error("wire_path_unavailable");
    }
    return result;
}

namespace {

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
    std::unique_ptr<Interaction_reader> interactions;
    Interaction_inventory interaction_inventory;
    observation::Subscription subscription;
    std::shared_ptr<observation::Wait_channel> channel;
    QStringList watched_directories;
    QStringList watched_files;
#ifdef KIMI_OBSERVER_TEST_HOOKS
    std::function<void()> scan_gate;
    std::function<void(bool)> wait_observer;
    std::function<void()> discovery_gate;
    std::function<bool()> read_allowed;
#endif
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
    std::optional<addon::Lifecycle_observation> pending_end;
    bool retired = false;
    std::optional<std::string> final_cursor;
    Clock::time_point bind_deadline = Clock::now() + std::chrono::seconds(60);
};

QString observation_path(const fs::path& path)
{
    return path_name::normalized(QString::fromStdString(path_string(path)));
}

void stop_observation(Binding& binding)
{
    binding.subscription.reset();
    if (binding.channel) binding.channel->cancel();
    binding.channel.reset();
    binding.watched_directories.clear();
    binding.watched_files.clear();
}

bool directory_exists(const fs::path& path)
{
    std::error_code error;
    const bool exists = fs::is_directory(path, error);
    if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) {
        throw std::runtime_error("wire_path_unavailable");
    }
    return exists;
}

// Missing descendants are represented by the first absent child of an existing
// directory. Its creation replaces these interests and earns a post-arm scan.
bool update_observation(Binding& binding, observation::Service& service,
    const observation::Configuration& configuration,
    const std::map<std::string, std::shared_ptr<Binding>>& sessions)
{
#ifdef KIMI_OBSERVER_TEST_HOOKS
    if (binding.discovery_gate) binding.discovery_gate();
#endif
    const auto resolved = find_session_wire(binding.home, binding.session);
    if (resolved) {
        for (const auto& [key, other] : sessions) {
            (void)key;
            if (other && other.get() != &binding && !other->closed && other->path == resolved) {
                throw std::runtime_error("wire_file_already_bound");
            }
        }
    }
    if (binding.path && !resolved) {
        binding.bind_deadline = Clock::now() + std::chrono::seconds(60);
    }
    binding.path = resolved;
    if (binding.interactions) {
        binding.interaction_inventory = discover_interaction_wires(binding.home, binding.session);
    }

    QStringList directories;
    QStringList files;
    QStringList wires;
    std::size_t characters = 0;
    const auto add_interest = [&](QStringList& interests, const fs::path& path) {
        const auto spelling = observation_path(path);
        if (interests.contains(spelling)) return;
        if (std::size_t(interests.size()) >= k_interest_bound ||
            std::size_t(spelling.size()) > k_interest_characters - characters) {
            throw std::runtime_error("wire_interest_capacity_exhausted");
        }
        characters += std::size_t(spelling.size());
        interests.append(spelling);
    };
    const fs::path home = addon::path_from_utf8(binding.home);
    fs::path frontier = home;
    while (!frontier.parent_path().empty() && !directory_exists(frontier.parent_path())) {
        const auto parent = frontier.parent_path();
        if (parent == frontier) break;
        frontier = parent;
    }
    add_interest(files, frontier);
    const fs::path root = home / "sessions";
    if (directory_exists(home)) add_interest(files, root);
    if (directory_exists(root)) {
        add_interest(directories, root);
        std::error_code error;
        for (fs::directory_iterator workspace(root, error), end;
             !error && workspace != end; workspace.increment(error))
        {
            if (workspace->is_symlink(error) || !workspace->is_directory(error)) continue;
            fs::path candidate = workspace->path() / binding.session;
            add_interest(files, candidate);
            if (fs::is_symlink(candidate, error)) continue;
            error.clear();
            for (const auto* component : {"agents", "main", "wire.jsonl"}) {
                if (!directory_exists(candidate)) break;
                candidate /= component;
                add_interest(files, candidate);
                if (std::string_view(component) == "wire.jsonl") wires.append(observation_path(candidate));
            }
        }
        if (error) throw std::runtime_error("wire_path_unavailable");
    }
    if (binding.path) {
        add_interest(files, *binding.path);
        wires.append(observation_path(*binding.path));
    }
    if (binding.interaction_inventory.root && directory_exists(*binding.interaction_inventory.root)) {
        add_interest(directories, *binding.interaction_inventory.root);
        for (const auto& wire : binding.interaction_inventory.wires) {
            add_interest(files, wire.path.parent_path());
            add_interest(files, wire.path);
            wires.append(observation_path(wire.path));
        }
    }
    directories.removeDuplicates();
    directories.sort();
    files.removeDuplicates();
    files.sort();
    if (binding.channel && directories == binding.watched_directories && files == binding.watched_files) {
        return false;
    }

    observation::Subscription_options options;
    options.mode = observation::Watch_mode::DIRECT;
    options.interval = configuration.general_interval;
    options.policy_epoch = std::make_shared<path_name::Policy_epoch>();
    QList<path_name::Key> exact_keys;
    QList<path_name::Key> wire_keys;
    QList<path_name::Key> directory_keys;
    for (const auto& directory : directories) {
        options.directories.append({directory, 0});
        directory_keys.append(options.policy_epoch->key(directory));
    }
    for (const auto& file : files) exact_keys.append(options.policy_epoch->key(file));
    for (const auto& wire : wires) wire_keys.append(options.policy_epoch->key(wire));
    options.exact_files = files;
    options.classifier = [exact_keys, wire_keys, directory_keys](const observation::Event& event) {
        const auto matches = [&](const auto& keys) {
            return std::any_of(keys.cbegin(), keys.cend(), [&](const auto& key) {
                return path_name::matches_key(event.path, key) || path_name::matches_key(event.old_path, key);
            });
        };
        if (matches(wire_keys)) return true;
        if (event.kind == observation::Event_kind::MODIFIED) return false;
        if (matches(exact_keys)) return true;
        if (event.entry_type == observation::Entry_type::NON_DIRECTORY) return false;
        return std::any_of(directory_keys.cbegin(), directory_keys.cend(), [&](const auto& key) {
            return path_name::matches_key(QFileInfo(event.path).absolutePath(), key) ||
                (!event.old_path.isEmpty() && path_name::matches_key(QFileInfo(event.old_path).absolutePath(), key));
        });
    };
    const auto channel = std::make_shared<observation::Wait_channel>();
    auto subscription = service.subscribe(std::move(options), channel);
    stop_observation(binding);
    binding.subscription = std::move(subscription);
    binding.channel = channel;
    binding.watched_directories = std::move(directories);
    binding.watched_files = std::move(files);
    return true;
}

bool observation_covered(const Binding& binding)
{
    for (const auto& directory : binding.watched_directories) {
        if (!binding.subscription.covers_directory(directory)) return false;
    }
    for (const auto& file : binding.watched_files) {
        if (!binding.subscription.covers_directory(QFileInfo(file).absolutePath())) return false;
    }
    return true;
}

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

bool scan(Binding& binding, addon::Invocation_context* invocation)
{
    if (binding.retired || !binding.path || !binding.failure.empty()) return false;
#ifdef KIMI_OBSERVER_TEST_HOOKS
    if (binding.read_allowed && !binding.read_allowed()) return false;
#endif
    std::ifstream input(*binding.path, std::ios::binary);
    if (!input) return false;
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
#ifdef KIMI_OBSERVER_TEST_HOOKS
    if (binding.scan_gate) binding.scan_gate();
#endif
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
    bool complete = length >= 0 && binding.offset == snapshot_end && binding.failure.empty();
    if (complete && binding.interactions) {
        std::vector<addon::Lifecycle_observation> interactions;
        complete = binding.interactions->acquire(binding.interaction_inventory, interactions,
            [&] { return invocation && invocation->cancel_requested(); });
        enqueue(binding, std::move(interactions));
    }
    return complete && binding.failure.empty();
}

void finish_session(Binding& binding)
{
    enqueue(binding, binding.decoder.close("SessionEnd"));
    if (!binding.failure.empty()) throw std::runtime_error(binding.failure);
    if (!binding.cursor_key.empty()) enqueue(binding, {*binding.pending_end});
    if (!binding.failure.empty()) throw std::runtime_error(binding.failure);
    binding.closed = true;
    stop_observation(binding);
}

} // namespace

struct Lifecycle_observer::State
{
    std::shared_ptr<observation::Service> service;
    observation::Configuration configuration;
    std::mutex mutex;
    // A null binding is a host-reserved candidate. Only authenticated
    // classification can populate its native home/session identity.
    std::map<std::string, std::shared_ptr<Binding>> sessions;
    std::map<std::string, std::shared_ptr<Binding>> cursors;
};

Lifecycle_observer::Lifecycle_observer()
: Lifecycle_observer(std::make_shared<observation::Service>(), {})
{}

Lifecycle_observer::Lifecycle_observer(std::shared_ptr<observation::Service> service,
    observation::Configuration configuration)
: m_state(std::make_unique<State>())
{
    if (!service) throw std::invalid_argument("Kimi observer requires a process observation service");
    configuration.validate();
    m_state->service = std::move(service);
    m_state->configuration = configuration;
}
Lifecycle_observer::~Lifecycle_observer() = default;

#ifdef KIMI_OBSERVER_TEST_HOOKS
void Lifecycle_observer::set_scan_gate_for_tests(std::function<void()> gate)
{
    std::lock_guard lock(m_state->mutex);
    for (const auto& [key, binding] : m_state->sessions) {
        (void)key;
        if (binding) binding->scan_gate = gate;
    }
}
void Lifecycle_observer::set_acquisition_failures_for_tests(
    std::function<void()> discovery_gate, std::function<bool()> read_allowed)
{
    std::lock_guard lock(m_state->mutex);
    for (const auto& [key, binding] : m_state->sessions) {
        (void)key;
        if (!binding) continue;
        binding->discovery_gate = discovery_gate;
        binding->read_allowed = read_allowed;
    }
}

void Lifecycle_observer::set_wait_observer_for_tests(std::function<void(bool)> observer)
{
    std::lock_guard lock(m_state->mutex);
    for (const auto& [key, binding] : m_state->sessions) {
        (void)key;
        if (binding) binding->wait_observer = observer;
    }
}

#endif

addon::Classify_event_result Lifecycle_observer::classify(const addon::Classify_event_request& request)
{
    addon::Classify_event_result result;
    const auto& event = request.event;
    const bool permission = event.name == "PermissionRequest" || event.name == "PermissionResult";
    if (event.name != "SessionStart" && event.name != "SessionEnd" && !permission) {
        return result;
    }
    const auto session = json_util::text(event.body, "session_id");
    const auto source = json_util::text(event.body, "source");
    require_event_identity(request, "session_id");
    if (source.size() > 128 || source.find('\0') != std::string::npos) {
        reject_event_field(request, "source", source.size() > 128 ? addon::Provider_event_problem::TOO_LARGE
                : addon::Provider_event_problem::CONTROL_CHARACTER,
            "session_source", 128);
    }
    if (session.find_first_of("/\\") != std::string::npos || session == "." || session == "..") {
        reject_event_field(request, "session_id", addon::Provider_event_problem::INVALID_VALUE, "session_component");
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = event.name;
    observation.provider_session_id = session;
    observation.session_source = source;
    std::lock_guard lock(m_state->mutex);
    if (permission) {
        for (const auto key : {"id", "agent_id", "tool_call_id"}) {
            require_event_identity(request, key);
        }
        const auto& transition = request.ledger.current_transition;
        if (!transition || request.ledger.current_epoch == 0 || transition->provider_session_id != session) {
            return result;
        }
        const auto found = m_state->sessions.find(transition->nonce);
        if (found == m_state->sessions.end() || !found->second || found->second->closed ||
            found->second->session != session || found->second->home != json_util::text(event.body, "kimi_code_home"))
        {
            return result;
        }
        result.classification = addon::Classification::EVENT;
        observation.attention_request_id = json_util::text(event.body, "id");
        observation.agent_id             = json_util::text(event.body, "agent_id");
        observation.attention_transition = event.name == "PermissionRequest"
            ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED;
        observation.attention_evidence   = addon::Evidence::CONFIRMED;
    }
    else
    if (event.name == "SessionStart") {
        const auto home = json_util::text(event.body, "kimi_code_home");
        if (home.empty() || !addon::path_from_utf8(home).is_absolute()) {
            const auto* value = event.body.find("kimi_code_home");
            reject_event_field(request, "kimi_code_home", !value ? addon::Provider_event_problem::MISSING
                    : !value->is_string() ? addon::Provider_event_problem::WRONG_TYPE
                    : home.empty() ? addon::Provider_event_problem::EMPTY : addon::Provider_event_problem::INVALID_VALUE,
                "absolute_path");
        }
        auto found = m_state->sessions.find(request.transition_nonce);
        if (found == m_state->sessions.end()) {
            reject_event_field(request, "transition_nonce", addon::Provider_event_problem::UNAVAILABLE,
                "correlation_not_reserved");
        }
        if (found->second &&
            (found->second->home != home || found->second->session != session)) {
            reject_event_field(request, found->second->home != home ? "kimi_code_home" : "session_id",
                addon::Provider_event_problem::MISMATCH, "wire_session_already_bound");
        }
        const auto& current = request.ledger.current_transition;
        if (current && current->provider_session_id == session) {
            const auto active = m_state->sessions.find(current->nonce);
            if (active != m_state->sessions.end() && active->second && !active->second->closed &&
                active->second->session == session && active->second->home == home)
            {
                // Kimi's same-current resume is a no-op. A repeated notification
                // must not replace its admitted cursor or interaction frontier.
                return result;
            }
        }
        if (!found->second) {
            auto binding = std::make_shared<Binding>(session, home);
            binding->nonce = request.transition_nonce;
            if (const auto* frontier = event.body.find("interaction_frontier"); frontier && !frontier->is_null()) {
                binding->interactions = std::make_unique<Interaction_reader>(session, *frontier);
            }
            m_state->sessions[request.transition_nonce] = std::move(binding);
        }
        result.classification = addon::Classification::TRANSITION;
        result.transition = source == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        observation.session_transition = addon::Session_transition::STARTED;
        try {
            observation.resume_record = make_resume_record(addon::path_from_utf8(home), session);
        }
        catch (const std::exception&) {
            // Unsupported optional resume evidence cannot suppress native identity.
        }
        classify_session_start_context(request, result);
    }
    else {
        const auto key = request.ledger.current_transition ? request.ledger.current_transition->nonce : std::string{};
        if (const auto found = m_state->sessions.find(key); found != m_state->sessions.end() && found->second &&
            found->second->session == session && found->second->home == json_util::text(event.body, "kimi_code_home"))
        {
            auto& binding = *found->second;
            if (binding.closed) {
                return result;
            }
            observation.session_transition = addon::Session_transition::ENDED;
            if (json_util::bounded_string(json_util::member(event.body, "reason"), 160, true)) {
                observation.session_end_reason = json_util::text(event.body, "reason");
            }
            binding.pending_end = observation;
            if (!binding.closed) {
                try {
                    update_observation(binding, *m_state->service, m_state->configuration, m_state->sessions);
                    if (scan(binding, nullptr)) finish_session(binding);
                    else if (!binding.failure.empty()) throw std::runtime_error(binding.failure);
                    else binding.subscription.request_rescan(QStringLiteral("Kimi final wire acquisition is incomplete"));
                }
                catch (...) {
                    binding.subscription.request_rescan(QStringLiteral("Kimi final wire discovery is incomplete"));
                    if (binding.cursor_key.empty() || !binding.failure.empty()) throw;
                }
            }
            // Retain the terminal fact behind acquisition of the captured wire
            // snapshot and its exact cursor. Native SessionEnd is not a flush seal.
            if (!binding.cursor_key.empty()) return result;
            if (!binding.closed) throw std::runtime_error("wire_final_drain_incomplete");
        }
        else {
            // Native /new and /resume announce B before closing A. The host
            // owns A's retirement; its later hook cannot publish through B.
            return result;
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
                    stop_observation(*binding);
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
        if (context.request().capability.capability_id == addon::k_attention_kind_capability_id) {
            const auto request = addon::decode_attention_classification_request(context.request().payload, &failure);
            if (!request) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            addon::Attention_classification_result result{classify(request->base), std::nullopt};
            if (!result.base.observations.empty() &&
                result.base.observations.front().attention_transition != addon::Attention_transition::NONE)
            {
                result.attention_kind = addon::Attention_kind{0, "permission"};
            }
            context.succeed(addon::encode_attention_classification_result(result));
            return;
        }
        const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
        context.succeed(addon::encode_classify_event_result(classify(*request)));
        return;
    }
    if (context.request().operation == "poll_observations") {
        const auto request = addon::decode_poll_observations_request(context.request().payload, &failure);
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
        context.succeed(addon::encode_poll_observations_result(result));
        return;
    }
    if (context.request().operation != "observe") { context.reject_unknown_operation(); return; }
    const auto request = addon::decode_observe_request(context.request().payload, &failure);
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
    std::uint64_t reported_sequence = initial_sequence;
    try {
        while (!context.cancel_requested() && Clock::now() < deadline) {
            std::shared_ptr<observation::Wait_channel> channel;
            {
                std::lock_guard lock(m_state->mutex);
                if (binding->retired) break;
                if (binding->next_sequence > reported_sequence) {
                    reported_sequence = binding->next_sequence;
                    const auto available = reported_sequence - initial_sequence;
                    context.report_progress(available, std::nullopt, addon::observe_progress_note(available));
                }
                if (binding->closed) break;
                if (!binding->channel) {
                    update_observation(*binding, *m_state->service, m_state->configuration, m_state->sessions);
                }
                if (!binding->failure.empty()) { error = binding->failure; break; }
                if (!binding->path && Clock::now() >= binding->bind_deadline) { error = "wire_bind_timeout"; break; }
                channel = binding->channel;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            // This wait only bounds SDK cancellation/deadline latency. A timeout
            // does not trigger a filesystem search or read.
            auto delivery = channel->wait(std::min(remaining, std::chrono::milliseconds(100)));
#ifdef KIMI_OBSERVER_TEST_HOOKS
            {
                std::lock_guard lock(m_state->mutex);
                if (binding->wait_observer) binding->wait_observer(delivery.has_value());
            }
#endif
            if (!delivery) continue;
            {
                std::lock_guard lock(m_state->mutex);
                if (delivery->valid() && !binding->retired && !binding->closed && binding->channel == channel) {
                    try {
                        const bool replaced = update_observation(
                            *binding, *m_state->service, m_state->configuration, m_state->sessions);
                        if (!replaced) {
                            const bool acquired = binding->path ? scan(*binding, &context) : !binding->pending_end;
                            if (acquired && binding->pending_end) finish_session(*binding);
                            else if (acquired && observation_covered(*binding)) {
                                binding->subscription.acknowledge(delivery->batch.capture);
                            }
                            else {
                                binding->subscription.request_rescan(QStringLiteral("Kimi wire acquisition is incomplete"));
                            }
                        }
                    }
                    catch (...) {
                        binding->subscription.request_rescan(QStringLiteral("Kimi wire discovery is incomplete"));
                        throw;
                    }
                }
            }
            delivery->completion.complete();
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
