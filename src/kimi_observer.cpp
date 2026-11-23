#include "kimi_observer.h"
#include "kimi_execution_context.h"
#include "kimi_resume.h"
#include "kimi_json.h"
#include "kimi_interactions.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/filesystem_lifecycle_observer.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>
#include <logonomic_cli_addon/sha256.h>

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
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

constexpr std::size_t k_record_bound = 4 * 1024 * 1024;
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

struct Binding final : addon::Filesystem_lifecycle_binding
{
    explicit Binding(std::string id, std::string root)

    :
        home(std::move(root)),
        decoder(id)
    {
        session = std::move(id);
    }

    std::string home;
    std::optional<fs::path> path;
    Wire_decoder decoder;
    std::unique_ptr<Interaction_reader> interactions;
    Interaction_inventory interaction_inventory;
    std::optional<observation::Subscription_options> last_interests;
    bool discovery_complete = true;
#ifdef KIMI_OBSERVER_TEST_HOOKS
    std::function<void()> scan_gate;
    std::function<void(bool)> wait_observer;
    std::function<void()> discovery_gate;
    std::function<bool()> read_allowed;
    void wait_completed(bool delivered) override
    {
        if (wait_observer) wait_observer(delivered);
    }
#endif
    std::uint64_t offset = 0;
    std::uint64_t anchor_offset = 0;
    std::string anchor;
    std::string pending;
    bool discarding = false;
    std::optional<addon::Lifecycle_observation> pending_end;

    observation::Subscription_options interests(const addon::Filesystem_lifecycle_bindings& bindings) override;
    bool reconcile(addon::Invocation_context* invocation) override;
    std::optional<addon::Model_update> root_model_observation() const override
    {
        return interactions ? interactions->model_observation() : std::nullopt;
    }
    void release() override
    {
        path.reset();
        pending.clear();
        anchor.clear();
        last_interests.reset();
        interactions.reset();
        interaction_inventory = {};
    }
};

QString observation_path(const fs::path& path)
{
    return path_name::normalized(QString::fromStdString(path_string(path)));
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
observation::Subscription_options discover_interests(
    Binding& binding, const addon::Filesystem_lifecycle_bindings& sessions)
{
#ifdef KIMI_OBSERVER_TEST_HOOKS
    if (binding.discovery_gate) binding.discovery_gate();
#endif
    const auto resolved = find_session_wire(binding.home, binding.session);
    if (resolved) {
        for (const auto& [key, value] : sessions) {
            (void)key;
            const auto other = std::static_pointer_cast<Binding>(value);
            if (other && other.get() != &binding && !other->closed && other->path == resolved) {
                throw std::runtime_error("wire_file_already_bound");
            }
        }
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

    observation::Subscription_options options;
    options.mode = observation::Watch_mode::DIRECT;
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
    return options;
}

void enqueue(Binding& binding, std::vector<addon::Lifecycle_observation> observations)
{
    for (auto& observation : observations) {
        observation.provider_event_id = binding.nonce + ':' + std::to_string(binding.next_sequence());
        observation.provider_sequence = std::to_string(binding.next_sequence());
        binding.publish(std::move(observation));
    }
}

bool scan(Binding& binding, addon::Invocation_context* invocation)
{
    if (binding.retired || !binding.path) return false;
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
           !binding.closed) {
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
    bool complete = length >= 0 && binding.offset == snapshot_end;
    if (complete && binding.interactions) {
        std::vector<addon::Lifecycle_observation> interactions;
        complete = binding.interactions->acquire(binding.interaction_inventory, interactions,
            [&] { return invocation && invocation->cancel_requested(); }, binding.nonce);
        enqueue(binding, std::move(interactions));
    }
    return complete;
}

void finish_session(Binding& binding)
{
    enqueue(binding, binding.decoder.close("SessionEnd"));
    if (binding.watch_started) {
        auto terminal = *binding.pending_end;
        terminal.provider_event_id = binding.nonce + ':' + std::to_string(binding.next_sequence());
        terminal.provider_sequence = std::to_string(binding.next_sequence());
        binding.finish(std::move(terminal));
    }
    else {
        binding.closed = true;
        binding.stop_observation();
    }
}

observation::Subscription_options Binding::interests(const addon::Filesystem_lifecycle_bindings& bindings)
{
    try {
        auto options = discover_interests(*this, bindings);
        last_interests = options;
        discovery_complete = true;
        return options;
    }
    catch (const std::exception&) {
        discovery_complete = false;
        if (last_interests) return *last_interests;
        // Initial discovery can fail before any native file is available.
        // Keep a bounded home frontier so service deliveries own its retries.
        auto frontier = observation_path(addon::path_from_utf8(home));
        auto parent = QFileInfo(frontier).absolutePath();
        while (!QFileInfo(parent).isDir() && parent != frontier) {
            frontier = parent;
            parent = QFileInfo(frontier).absolutePath();
        }
        return addon::lifecycle_file_interests({frontier}, {});
    }
}

bool Binding::reconcile(addon::Invocation_context* invocation)
{
    const bool acquired = path ? scan(*this, invocation) : !pending_end;
    if (acquired && discovery_complete && pending_end) finish_session(*this);
    return acquired && discovery_complete;
}

} // namespace

struct Lifecycle_observer::State
{
    State(std::shared_ptr<observation::Service> service, observation::Configuration configuration)

    :
        driver(std::move(service), configuration)
    {}

    addon::Filesystem_lifecycle_observer driver;
};

Lifecycle_observer::Lifecycle_observer()

:
    Lifecycle_observer(std::make_shared<observation::Service>(), {})
{}

Lifecycle_observer::Lifecycle_observer(std::shared_ptr<observation::Service> service,
    observation::Configuration configuration)

:
    m_state(std::make_unique<State>(std::move(service), configuration))
{}
Lifecycle_observer::~Lifecycle_observer() = default;

#ifdef KIMI_OBSERVER_TEST_HOOKS
void Lifecycle_observer::set_scan_gate_for_tests(std::function<void()> gate)
{
    auto guard = m_state->driver.lock();
    for (const auto& [key, value] : m_state->driver.bindings()) {
        (void)key;
        const auto binding = std::static_pointer_cast<Binding>(value);
        if (binding) binding->scan_gate = gate;
    }
}
void Lifecycle_observer::set_acquisition_failures_for_tests(
    std::function<void()> discovery_gate, std::function<bool()> read_allowed)
{
    auto guard = m_state->driver.lock();
    for (const auto& [key, value] : m_state->driver.bindings()) {
        (void)key;
        const auto binding = std::static_pointer_cast<Binding>(value);
        if (!binding) continue;
        binding->discovery_gate = discovery_gate;
        binding->read_allowed = read_allowed;
    }
}

void Lifecycle_observer::set_wait_observer_for_tests(std::function<void(bool)> observer)
{
    auto guard = m_state->driver.lock();
    for (const auto& [key, value] : m_state->driver.bindings()) {
        (void)key;
        const auto binding = std::static_pointer_cast<Binding>(value);
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
    auto& driver = m_state->driver;
    auto guard = driver.lock();
    if (permission) {
        for (const auto key : {"id", "agent_id", "tool_call_id"}) {
            require_event_identity(request, key);
        }
        const auto& transition = request.ledger.current_transition;
        if (!transition || request.ledger.current_epoch == 0 || transition->provider_session_id != session) {
            return result;
        }
        const auto owner = std::static_pointer_cast<Binding>(driver.find(transition->nonce));
        if (!owner || owner->closed || owner->session != session ||
            owner->home != json_util::text(event.body, "kimi_code_home"))
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
        const auto owner = std::static_pointer_cast<Binding>(driver.find(request.transition_nonce));
        if (!driver.reserved(request.transition_nonce)) {
            reject_event_field(request, "transition_nonce", addon::Provider_event_problem::UNAVAILABLE,
                "correlation_not_reserved");
        }
        if (owner && (owner->home != home || owner->session != session)) {
            reject_event_field(request, owner->home != home ? "kimi_code_home" : "session_id",
                addon::Provider_event_problem::MISMATCH, "wire_session_already_bound");
        }
        const auto& current = request.ledger.current_transition;
        if (current && current->provider_session_id == session) {
            const auto active = std::static_pointer_cast<Binding>(driver.find(current->nonce));
            if (active && !active->closed && active->session == session && active->home == home)
            {
                // Kimi's same-current resume is a no-op. A repeated notification
                // must not replace its admitted cursor or interaction frontier.
                return result;
            }
        }
        if (!owner) {
            auto binding = std::make_shared<Binding>(session, home);
            binding->nonce = request.transition_nonce;
            if (const auto* frontier = event.body.find("interaction_frontier"); frontier && !frontier->is_null()) {
                binding->interactions = std::make_unique<Interaction_reader>(session, *frontier);
            }
            driver.bind(std::move(binding));
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
        if (const auto owner = std::static_pointer_cast<Binding>(driver.find(key)); owner &&
            owner->session == session && owner->home == json_util::text(event.body, "kimi_code_home"))
        {
            auto& binding = *owner;
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
                    driver.refresh_interests(binding);
                    if (scan(binding, nullptr) && binding.discovery_complete) finish_session(binding);
                    else binding.request_rescan();
                }
                catch (...) {
                    binding.request_rescan();
                    if (!binding.watch_started) throw;
                }
            }
            // Retain the terminal fact behind acquisition of the captured wire
            // snapshot and its exact cursor. Native SessionEnd is not a flush seal.
            if (binding.watch_started) return result;
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
    if (m_state->driver.handle(context)) return;
    addon::Contract_failure failure;
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
    context.reject_unknown_operation();
}

} // namespace kimi
