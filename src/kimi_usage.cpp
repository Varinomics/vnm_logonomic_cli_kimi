#include "kimi_usage.h"
#include "kimi_usage_correction.h"
#include "kimi_accounts.h"
#include "kimi_json.h"
#include "kimi_launcher.h"
#include "kimi_wire_record.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/stored_usage_accounting.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/usage_temporal.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <iterator>
#include <set>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <utility>

namespace json_util = logonomic::cli_addon::json;

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
constexpr std::uint64_t k_max_exact = 9007199254740991;
constexpr std::size_t k_read_bytes = 1024 * 1024;

struct File_record
{
    std::uint64_t size = 0;
    std::int64_t modified_ms = 0;
    std::string cwd;
    bool stale_breakdown = false;
    std::optional<addon::Usage_record> usage;
    std::optional<Json> temporal_carrier;
    std::optional<Json> fork_carrier;
    std::optional<Json> fork_evidence;
    std::uint64_t event_begin = 0;
    std::uint64_t event_end = 0;
    std::uint64_t accepted_event_count = 0;
    bool legacy_prefix_matches = false;
    bool retained_prefix_matches = false;
};

class Scan_workspace;

struct Snapshot
{
    std::string owner;
    addon::Observation_scope scope;
    std::unique_ptr<Scan_workspace> workspace;
    fs::path records;
    std::uint64_t bytes = 0;
    std::uint64_t touched_ms = 0;
    std::optional<addon::Usage_cache_reference> cache_candidate;
};

std::mutex s_mutex;
std::map<std::string, Snapshot> s_snapshots;

fs::path cache_io_path(const fs::path& path)
{
#ifdef _WIN32
    // Cache descendants can exceed MAX_PATH in account-scoped storage.
    // Keep the extended spelling local to I/O, never account or wire identity.
    auto native = fs::absolute(path).lexically_normal().make_preferred().native();
    if (native.starts_with(L"\\\\?\\")) return fs::path(native);
    if (native.starts_with(L"\\\\")) return fs::path(L"\\\\?\\UNC\\" + native.substr(2));
    return fs::path(L"\\\\?\\" + native);
#else
    return path;
#endif
}

void check_cancel(addon::Invocation_context& context)
{
    if (context.cancel_requested()) throw std::runtime_error("cancelled");
}

std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::int64_t file_modified_ms(fs::file_time_type value)
{
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    const auto modified = std::chrono::clock_cast<std::chrono::system_clock>(value);
#else
    const auto modified = fs::file_time_type::clock::to_sys(value);
#endif
    return std::chrono::duration_cast<std::chrono::milliseconds>(modified.time_since_epoch()).count();
}

void add(std::uint64_t& destination, std::uint64_t value)
{
    if (value > k_max_exact - destination) throw std::runtime_error("usage_counter_overflow");
    destination += value;
}

std::uint64_t count(const Json& value)
{
    if (!value.is_number() || value.as_number() < 0 || value.as_number() > k_max_exact ||
        std::floor(value.as_number()) != value.as_number())
    {
        throw std::runtime_error("usage_counter_invalid");
    }
    return static_cast<std::uint64_t>(value.as_number());
}

std::string local_date(std::uint64_t milliseconds)
{
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &seconds)) throw std::runtime_error("usage_timestamp_invalid");
#else
    if (!localtime_r(&seconds, &local)) throw std::runtime_error("usage_timestamp_invalid");
#endif
    char date[11]{};
    if (std::strftime(date, sizeof(date), "%Y-%m-%d", &local) != 10) {
        throw std::runtime_error("usage_timestamp_invalid");
    }
    return date;
}

std::optional<std::string> epoch_instant(std::uint64_t milliseconds)
{
    // RFC3339 has four-digit years. This bounds optional representation only.
    if (milliseconds > 253402300799999ull) return std::nullopt;
    const std::chrono::sys_time<std::chrono::milliseconds> instant{
        std::chrono::milliseconds{static_cast<std::int64_t>(milliseconds)}};
    const auto day = std::chrono::floor<std::chrono::days>(instant);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{instant - day};
    char text[25]{};
    const auto size = std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
        static_cast<long long>(time.hours().count()), static_cast<long long>(time.minutes().count()),
        static_cast<long long>(time.seconds().count()), static_cast<long long>(time.subseconds().count()));
    if (size != 24) return std::nullopt;
    return std::string(text, 24);
}

addon::Usage_temporal unavailable_temporal(const addon::Usage_record& record, std::string reason)
{
    addon::Usage_temporal evidence;
    evidence.parent = {record.record_id, record.total_tokens, "provider-file", std::nullopt};
    evidence.unavailable_tokens = record.total_tokens;
    evidence.coverage_reason = std::move(reason);
    return evidence;
}

void assign_temporal(addon::Usage_record& record, addon::Usage_temporal evidence)
{
    if (!addon::decode_usage_temporal(addon::encode_usage_temporal(evidence), nullptr)) {
        evidence.portions.clear();
        evidence.unavailable_tokens = evidence.parent.total_tokens;
        evidence.coverage_reason = "capacity";
        if (!addon::decode_usage_temporal(addon::encode_usage_temporal(evidence), nullptr)) {
            record.temporal.reset();
            return;
        }
    }
    record.temporal = std::move(evidence);
}

void publish_temporal(addon::Usage_record& record, addon::Usage_temporal evidence)
{
    assign_temporal(record, std::move(evidence));
    const auto valid_record = [&record] {
        const auto encoded = addon::encode_usage_record(record, 3);
        return addon::decode_usage_record(encoded, nullptr, 3) &&
            addon::parse_json(addon::serialize_json(encoded), {}, nullptr);
    };
    if (!record.temporal || valid_record()) return;
    evidence = *record.temporal;
    evidence.portions.clear();
    evidence.unavailable_tokens = record.total_tokens;
    evidence.coverage_reason = "capacity";
    assign_temporal(record, std::move(evidence));
    if (record.temporal && !valid_record()) record.temporal.reset();
}

Json spool_record(const addon::Usage_record& record)
{
    auto value = addon::encode_stored_usage_accounting(record);
    if (record.aggregate_evidence) value.set("aggregate-evidence", Json(*record.aggregate_evidence));
    if (!record.capabilities.as_object().empty()) value.set("record-capabilities", record.capabilities);
    if (record.temporal) {
        value.set("usage-temporal", addon::encode_usage_temporal(*record.temporal));
        if (addon::serialize_json(value).size() > addon::Json_parse_limits{}.max_bytes) {
            value.erase("usage-temporal");
        }
    }
    return value;
}

std::optional<addon::Usage_record> spool_record(const Json& value)
{
    auto accounting = value;
    accounting.erase("usage-temporal");
    accounting.erase("record-capabilities");
    accounting.erase("aggregate-evidence");
    auto record = addon::decode_stored_usage_accounting(accounting, nullptr);
    if (record) {
        if (const auto* capabilities = value.find("record-capabilities")) record->capabilities = *capabilities;
        if (const auto* evidence = value.find("aggregate-evidence")) record->aggregate_evidence = evidence->as_string();
        const auto* payload = value.find("usage-temporal");
        auto evidence = payload ? addon::decode_usage_temporal(*payload, nullptr) : std::nullopt;
        if (evidence && evidence->parent.record_id == record->record_id &&
            evidence->parent.total_tokens == record->total_tokens)
        {
            record->temporal = std::move(evidence);
        }
    }
    return record;
}

void merge_temporal(addon::Usage_record& accumulated, const addon::Usage_record& incoming,
    addon::Invocation_context& context)
{
    if (!accumulated.temporal && !incoming.temporal) return;
    auto evidence = accumulated.temporal.value_or(unavailable_temporal(accumulated, "missing-evidence"));
    const auto other = incoming.temporal.value_or(unavailable_temporal(incoming, "missing-evidence"));
    add(evidence.parent.total_tokens, incoming.total_tokens);
    add(evidence.unavailable_tokens, other.unavailable_tokens);
    const bool partial_capacity = evidence.coverage_reason == "capacity" || other.coverage_reason == "capacity";
    if (!evidence.retained_from) evidence.retained_from = other.retained_from;
    std::map<std::string, std::uint64_t> positions;
    bool capacity = false;
    const std::array<const std::vector<addon::Usage_temporal_portion>*, 2> sources{
        &evidence.portions, &other.portions};
    for (const auto* portions : sources) {
        for (const auto& portion : *portions) {
            check_cancel(context);
            if (!positions.contains(portion.instant) && positions.size() == addon::k_max_usage_temporal_portions) {
                capacity = true;
                break;
            }
            add(positions[portion.instant], portion.total_tokens);
        }
        if (capacity) break;
    }
    evidence.portions.clear();
    if (capacity) {
        evidence.unavailable_tokens = evidence.parent.total_tokens;
        evidence.coverage_reason = "capacity";
    }
    else {
        for (const auto& [instant, tokens] : positions) evidence.portions.push_back({"instant", instant, {}, {}, tokens});
        evidence.coverage_reason = evidence.unavailable_tokens ?
            (partial_capacity ? "capacity" : "missing-evidence") : "complete";
    }
    // The accounting total is updated by the authoritative merger immediately after this.
    assign_temporal(accumulated, std::move(evidence));
}

std::string path_key(const fs::path& path)
{
    // Account and file identities use the same platform path comparison rules.
    return account_key(fs::absolute(path).lexically_normal());
}

std::optional<std::string> repository(std::string_view cwd)
{
    auto path = addon::path_from_utf8(cwd);
    std::error_code error;
    if (!path.is_absolute() || !fs::is_directory(path, error) || error) return std::nullopt;
    path = path.lexically_normal();
    while (!path.empty()) {
        const auto marker = path / ".git";
        if (fs::is_directory(marker, error) || fs::is_regular_file(marker, error)) return path_string(path);
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    return std::nullopt;
}

void attribute(addon::Usage_record& record, std::string_view cwd)
{
    addon::Usage_repository allocation;
    if (record.total_tokens) {
        if (const auto root = repository(cwd)) allocation.allocations.push_back({*root, record.total_tokens});
        else allocation.unattributed_tokens = record.total_tokens;
    }
    record.repository_usage = std::move(allocation);
}

enum class Wire_read
{
    STABLE,
    UNAVAILABLE,
    CHANGED,
};

// Both native wires and strict host predecessor streams use the same stable
// acquisition boundary. A changed or unreadable file cannot replace evidence.
Wire_read read_stable_chunks(const fs::path& path, File_record& fingerprint,
    addon::Invocation_context& context, const std::function<void(std::string_view)>& consume)
{
    std::error_code error;
    const auto before_size = fs::file_size(path, error);
    if (error) return Wire_read::UNAVAILABLE;
    const auto before_time = fs::last_write_time(path, error);
    if (error) return Wire_read::UNAVAILABLE;
    // A failed line blames the log only if the log was at rest.
    fingerprint.size = before_size;
    fingerprint.modified_ms = file_modified_ms(before_time);
    const auto observation = [&] {
        std::error_code stat_error;
        const auto size = fs::file_size(path, stat_error);
        if (stat_error) {
            return stat_error == std::errc::no_such_file_or_directory
                ? Wire_read::CHANGED : Wire_read::UNAVAILABLE;
        }
        if (size != before_size) return Wire_read::CHANGED;
        const auto time = fs::last_write_time(path, stat_error);
        if (stat_error) {
            return stat_error == std::errc::no_such_file_or_directory
                ? Wire_read::CHANGED : Wire_read::UNAVAILABLE;
        }
        return time == before_time ? Wire_read::STABLE : Wire_read::CHANGED;
    };
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Wire_read::UNAVAILABLE;
    std::string buffer(k_read_bytes, '\0');
    std::uint64_t consumed = 0;
    try {
        for (;;) {
            check_cancel(context);
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto received = static_cast<std::size_t>(stream.gcount());
            if (received == 0) {
                break;
            }
            consumed += received;
            consume(std::string_view(buffer).substr(0, received));
        }
    }
    catch (...) {
        const auto observed = observation();
        if (observed != Wire_read::STABLE) return observed;
        throw;
    }
    if (stream.bad()) {
        return Wire_read::UNAVAILABLE;
    }
    const auto observed = observation();
    if (observed != Wire_read::STABLE) return observed;
    if (consumed != before_size) {
        return Wire_read::CHANGED;
    }
    return Wire_read::STABLE;
}

Wire_read read_stable_lines(const fs::path& path, File_record& fingerprint,
    addon::Invocation_context& context,
    const std::function<void(std::string_view)>& consume,
    const std::function<void(std::string_view)>& observe_bytes)
{
    const auto limit = static_cast<std::size_t>(addon::Json_parse_limits{}.max_bytes);
    std::string pending;
    return read_stable_chunks(path, fingerprint, context, [&](std::string_view bytes) {
        observe_bytes(bytes);
        while (!bytes.empty()) {
            const auto newline = bytes.find('\n');
            const auto part = bytes.substr(0, newline);
            if (part.size() > limit - pending.size()) {
                throw std::runtime_error("usage_record_invalid");
            }
            pending.append(part);
            if (newline == std::string_view::npos) {
                break;
            }
            consume(pending);
            pending.clear();
            bytes.remove_prefix(newline + 1);
        }
    });
}

// Native payloads that accounting does not consume need only an unambiguous,
// valid envelope. Required records keep their original bytes and SDK limits.
// Every complete physical record is delivered, including an ignored one, so
// filtering cannot change first-record ownership or accepted-event offsets.
Wire_read read_native_usage_lines(const fs::path& path, File_record& fingerprint,
    addon::Invocation_context& context,
    const std::function<void(const std::optional<Json>&, std::uint64_t)>& consume,
    const std::function<void(std::string_view)>& observe_bytes)
{
    const auto limit = static_cast<std::size_t>(addon::Json_parse_limits{}.max_bytes);
    Wire_record_envelope envelope;
    std::string pending;
    std::uint64_t physical_bytes = 0;
    bool leading_nul = true;
    bool oversized = false;
    bool first_record = true;
    const auto required = [&] {
        const auto kind = envelope.kind();
        return kind == Wire_record_kind::USAGE || kind == Wire_record_kind::FORKED ||
            (first_record && kind == Wire_record_kind::METADATA);
    };
    return read_stable_chunks(path, fingerprint, context, [&](std::string_view bytes) {
        observe_bytes(bytes);
        while (!bytes.empty()) {
            const auto newline = bytes.find('\n');
            const auto part = bytes.substr(0, newline);
            physical_bytes += part.size();
            auto content = part;
            while (leading_nul && !content.empty() && content.front() == '\0') {
                content.remove_prefix(1);
            }
            if (!content.empty()) {
                leading_nul = false;
                envelope.append(content);
            }
            if (!envelope.type_known() || required()) {
                if (!oversized && part.size() <= limit - pending.size()) {
                    pending.append(part);
                }
                else {
                    oversized = true;
                    pending.clear();
                }
            }
            else {
                pending.clear();
            }
            if (oversized && required()) {
                throw std::runtime_error("usage_record_invalid");
            }
            if (newline == std::string_view::npos) {
                break;
            }
            if (!envelope.finish()) {
                throw std::runtime_error("usage_record_invalid");
            }
            std::optional<Json> event;
            if (required()) {
                auto line = std::string_view(pending);
                while (!line.empty() && line.front() == '\0') {
                    line.remove_prefix(1);
                }
                if (!oversized) {
                    event = json_util::parse(line);
                }
                if (!event || !event->is_object()) {
                    throw std::runtime_error("usage_record_invalid");
                }
            }
            consume(event, physical_bytes + 1);
            first_record = false;
            envelope.reset();
            pending.clear();
            physical_bytes = 0;
            leading_nul = true;
            oversized = false;
            bytes.remove_prefix(newline + 1);
        }
    });
}

Json optional_document(const fs::path& path, bool* missing = nullptr)
{
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (missing) {
        *missing = error == std::errc::no_such_file_or_directory;
    }
    if (size > 2 * 1024 * 1024 || error) return Json::object();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (missing) {
            const auto status = fs::status(path, error);
            *missing = status.type() == fs::file_type::not_found;
        }
        return Json::object();
    }
    std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    const auto result = json_util::parse(bytes);
    return result && result->is_object() ? *result : Json::object();
}

std::string recorded_cwd(const fs::path& session, const Json& workspaces)
{
    const auto metadata = optional_document(session / "state.json");
    auto state = json_util::text(metadata, "cwd");
    if (!metadata.find("cwd")) state = json_util::text(metadata, "workDir");
    const auto mapped = json_util::text(json_util::member(json_util::member(workspaces, "workspaces"), path_string(session.parent_path().filename())), "root");
    if (!state.empty() && !mapped.empty() && path_key(addon::path_from_utf8(state)) != path_key(addon::path_from_utf8(mapped))) return {};
    return state.empty() ? mapped : state;
}

std::array<addon::Usage_amount*, 4> amounts(addon::Usage_record& record)
{
    return {&record.input, &record.output, &record.cache_read, &record.cache_creation};
}

// Accumulates one session's usage from the lines of a wire log.
class Wire_usage final
{
public:
    explicit Wire_usage(const fs::path& session)
    {
        m_record.record_id = "kimi:" + path_string(session.filename());
        m_record.session_key = m_record.record_id;
        m_record.source_path = path_string(fs::absolute(session).lexically_normal());
        m_record.counter_field = "usage";
        m_record.source_counter_semantics = "per_event_sum";
    }

    void consume(std::string_view line, addon::Invocation_context& context)
    {
        check_cancel(context);
        while (!line.empty() && line.front() == '\0') line.remove_prefix(1);
        const auto event = json_util::parse(line);
        if (!event || !event->is_object()) throw std::runtime_error("usage_record_invalid");
        consume_event(*event, context);
    }

    void consume_event(const Json& event, addon::Invocation_context& context)
    {
        check_cancel(context);
        if (json_util::text(event, "type") != "usage.record") return;
        const auto& native_time = json_util::member(event, "time");
        const auto milliseconds = count(native_time);
        const auto date = local_date(milliseconds);
        const auto& usage = json_util::member(event, "usage");
        if (!usage.is_object()) throw std::runtime_error("usage_record_invalid");
        bool recognized = false;
        std::uint64_t event_tokens = 0;
        constexpr std::array<std::string_view, 4> names{"inputOther", "output", "inputCacheRead", "inputCacheCreation"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (const auto* value = usage.find(names[index])) {
                const auto tokens = count(*value);
                add(m_record.total_tokens, tokens);
                add(event_tokens, tokens);
                add(m_values[index], tokens);
                ++m_coverage[index];
                recognized = true;
            }
        }
        if (!recognized) throw std::runtime_error("usage_counters_missing");
        if (event_tokens && !m_temporal_capacity) {
            const auto instant = native_time.is_integer() ? epoch_instant(milliseconds) : std::nullopt;
            if (!instant) add(m_temporal_unavailable, event_tokens);
            else if (!m_positions.contains(*instant) && m_positions.size() == addon::k_max_usage_temporal_portions) {
                m_positions.clear();
                m_temporal_capacity = true;
            }
            else add(m_positions[*instant], event_tokens);
        }
        const auto model = json_util::text(event, "model");
        if (!m_record.model || m_record.model->empty()) m_record.model = model;
        else if (!model.empty() && *m_record.model != model) m_record.model = "multiple";
        m_record.completed_date = std::max(m_record.completed_date, date);
        ++m_records;
    }

    std::optional<addon::Usage_record> finish(std::string_view cwd,
        const addon::Usage_record* empty_identity = nullptr)
    {
        if (!m_records) {
            if (!empty_identity) return std::nullopt;
            m_record.completed_date = empty_identity->completed_date;
            m_record.model = empty_identity->model;
        }
        auto fields = amounts(m_record);
        for (std::size_t index = 0; index < fields.size(); ++index) {
            fields[index]->applicable = true;
            fields[index]->value = m_values[index];
            fields[index]->coverage = addon::Usage_field_coverage{m_coverage[index], m_records, false,
                m_coverage[index] ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN};
        }
        attribute(m_record, cwd);
        auto evidence = unavailable_temporal(m_record, m_temporal_capacity ? "capacity" : "complete");
        if (!m_temporal_capacity) {
            evidence.unavailable_tokens = m_temporal_unavailable;
            if (m_temporal_unavailable) evidence.coverage_reason = "missing-evidence";
            for (const auto& [instant, tokens] : m_positions) evidence.portions.push_back({"instant", instant, {}, {}, tokens});
        }
        assign_temporal(m_record, std::move(evidence));
        return std::move(m_record);
    }

private:
    addon::Usage_record          m_record;
    std::array<std::uint64_t, 4> m_values{};
    std::array<std::uint64_t, 4> m_coverage{};
    std::uint64_t                m_records = 0;
    std::map<std::string, std::uint64_t> m_positions;
    std::uint64_t m_temporal_unavailable = 0;
    bool m_temporal_capacity = false;
};

std::optional<Json> fork_session_identity(const fs::path& wire, bool& missing_state)
{
    const auto session = wire.parent_path().parent_path().parent_path();
    const auto state = optional_document(session / "state.json", &missing_state);
    const auto id = path_string(session.filename());
    const auto* version = state.find("version");
    const bool legacy = !version || (version->is_integer() && version->as_integer() == 1);
    const auto* stored_id = state.find("id");
    const auto* created = state.find("createdAt");
    const auto* parent = state.find("forkedFrom");
    std::optional<std::int64_t> created_at;
    if (created && created->is_integer()) created_at = created->as_integer();
    else if (legacy && created && created->is_string() && created->as_string().ends_with('Z')) {
        // Native legacy metadata uses an ISO UTC date and the containing
        // directory's identity. Kimi normalizes both when opening that session.
        created_at = addon::parse_native_usage_timestamp(created->as_string());
    }
    if ((stored_id ? json_util::text(state, "id") != id : !legacy || !created || !created->is_string()) ||
        !created_at || *created_at < 0 ||
        (parent && !parent->is_string())) return std::nullopt;
    const auto parent_id = parent ? parent->as_string() : std::string{};
    const auto parent_path = addon::path_from_utf8(parent_id);
    if (!parent_id.empty() && (parent_path.is_absolute() || parent_path.has_parent_path() ||
            parent_id == "." || parent_id == "..")) return std::nullopt;
    return json_util::object({{"session_id", Json(id)}, {"parent_session_id", Json(parent_id)},
        {"created_at", Json(*created_at)}, {"agent_id", Json(path_string(wire.parent_path().filename()))}});
}

bool same_counted_usage(const addon::Usage_record& observed, const addon::Usage_record& accepted)
{
    const auto numeric = [](const addon::Usage_record& record) {
        const auto encoded = addon::encode_stored_usage_accounting(record);
        auto result = Json::object();
        for (const auto* key : {"total_tokens", "coverage_unit", "input", "output", "cache_read", "cache_creation"}) {
            result.set(key, json_util::member(encoded, key));
        }
        return addon::serialize_json_canonical(result);
    };
    if (numeric(observed) != numeric(accepted)) return false;
    if (!accepted.temporal) return true;
    if (!observed.temporal) return false;
    auto previous = *accepted.temporal;
    auto current = *observed.temporal;
    previous.parent = current.parent;
    previous.retained_from.reset();
    current.retained_from.reset();
    return addon::serialize_json_canonical(addon::encode_usage_temporal(previous)) ==
        addon::serialize_json_canonical(addon::encode_usage_temporal(current));
}

bool valid_fork_evidence(const Json& evidence, const fs::path& path)
{
    auto shape = evidence;
    shape.erase("unresolved_event_count");
    shape.erase("creator_session_id");
    if (!json_util::exact_keys(shape, {"identity", "native_prefix_sha256_hex", "event_sha256_hex",
            "event_count", "fork_event_count", "forked", "accounting"})) return false;
    const auto& identity = json_util::member(evidence, "identity");
    if (!json_util::exact_keys(identity, {"session_id", "parent_session_id", "created_at", "agent_id"}) ||
        json_util::text(identity, "session_id") != path_string(path.parent_path().parent_path().parent_path().filename()) ||
        json_util::text(identity, "agent_id") != path_string(path.parent_path().filename()) ||
        !json_util::member(identity, "parent_session_id").is_string() || !json_util::member(identity, "created_at").is_integer() ||
        json_util::member(identity, "created_at").as_integer() < 0 ||
        !addon::sha256_from_hex(json_util::text(evidence, "native_prefix_sha256_hex")) ||
        !addon::sha256_from_hex(json_util::text(evidence, "event_sha256_hex")) ||
        !json_util::member(evidence, "event_count").is_integer() || !json_util::member(evidence, "fork_event_count").is_integer() ||
        !json_util::member(evidence, "forked").is_boolean()) return false;
    const auto events = json_util::member(evidence, "event_count").as_integer();
    const auto boundary = json_util::member(evidence, "fork_event_count").as_integer();
    const auto accounting = json_util::text(evidence, "accounting");
    const auto parent_id = json_util::text(identity, "parent_session_id");
    const auto parent = addon::path_from_utf8(parent_id);
    const auto* unresolved = evidence.find("unresolved_event_count");
    const auto* creator = evidence.find("creator_session_id");
    if (creator && (!creator->is_string() || creator->as_string().empty())) return false;
    const bool retained = accounting == "retained";
    if (retained && (!unresolved || !unresolved->is_integer() || unresolved->as_integer() < 0 ||
        unresolved->as_integer() > events)) return false;
    if (!retained && unresolved) return false;
    return (parent_id.empty() || (!parent.is_absolute() && !parent.has_parent_path() &&
            parent_id != "." && parent_id != "..")) &&
        events >= 0 && static_cast<std::uint64_t>(events) <= k_max_exact && boundary >= 0 && boundary <= events &&
        (json_util::member(evidence, "forked").as_boolean() || boundary == 0) &&
        (accounting == "raw" || accounting == "owned" || retained);
}

void require_io(bool condition)
{
    if (!condition) throw std::runtime_error("usage_storage_io_failed");
}

// Each persisted value is bounded; the number of values is not.
bool read_line(std::istream& input, std::string& line)
{
    line.clear();
    char byte;
    while (input.get(byte)) {
        if (byte == '\n') return true;
        if (line.size() >= addon::Json_parse_limits{}.max_bytes * 2) throw std::runtime_error("usage_cache_record_invalid");
        line.push_back(byte);
    }
    require_io(!input.bad());
    if (!line.empty()) throw std::runtime_error("usage_cache_truncated");
    return false;
}

void write_line(std::ostream& output, std::string_view line)
{
    output << line << '\n';
    require_io(static_cast<bool>(output));
}

class Scan_workspace final
{
public:
    explicit Scan_workspace(const fs::path& storage)
        : m_path(cache_io_path(storage / ("usage-" + addon::random_nonce_hex() + ".tmp")))
    {
        require_io(fs::create_directory(m_path));
        try {
            fs::permissions(m_path, fs::perms::owner_all, fs::perm_options::replace);
        }
        catch (...) {
            std::error_code error;
            fs::remove(m_path, error);
            throw;
        }
    }
    ~Scan_workspace() { std::error_code error; fs::remove_all(m_path, error); }
    const fs::path& path() const { return m_path; }
private:
    fs::path m_path;
};

// Like the Codex scanner, use bounded sorted runs and pairwise disk merges.
// A run contains at most 8 MiB of encoded rows; merge holds two rows.
class Row_sorter final
{
public:
    Row_sorter(fs::path directory, std::string name, addon::Invocation_context& context)
        : m_directory(std::move(directory)), m_name(std::move(name)), m_context(context) {}

    void add(std::string key, const Json& value)
    {
        check_cancel(m_context);
        auto line = std::move(key) + '\t' + addon::serialize_json(value);
        if (m_bytes + line.size() > 8 * 1024 * 1024) flush();
        m_bytes += line.size();
        m_rows.push_back(std::move(line));
    }

    fs::path finish()
    {
        flush();
        std::uint64_t level = 0;
        if (!m_runs) {
            std::ofstream empty(run(0, 0), std::ios::binary);
            require_io(static_cast<bool>(empty));
            m_runs = 1;
        }
        while (m_runs > 1) {
            const auto count = (m_runs + 1) / 2;
            for (std::uint64_t index = 0; index < count; ++index) {
                check_cancel(m_context);
                const auto destination = run(level + 1, index);
                const auto left_path = run(level, index * 2);
                if (index * 2 + 1 == m_runs) {
                    fs::rename(left_path, destination);
                    continue;
                }
                const auto right_path = run(level, index * 2 + 1);
                std::ifstream left(left_path, std::ios::binary), right(right_path, std::ios::binary);
                std::ofstream output(destination, std::ios::binary);
                require_io(left && right && output);
                std::string a, b;
                bool have_a = read_line(left, a), have_b = read_line(right, b);
                while (have_a || have_b) {
                    check_cancel(m_context);
                    if (have_a && (!have_b || a < b)) {
                        write_line(output, a);
                        have_a = read_line(left, a);
                    }
                    else {
                        write_line(output, b);
                        have_b = read_line(right, b);
                    }
                }
                output.close();
                require_io(static_cast<bool>(output));
                left.close(); right.close();
                fs::remove(left_path); fs::remove(right_path);
            }
            ++level;
            m_runs = count;
        }
        return run(level, 0);
    }
private:
    fs::path run(std::uint64_t level, std::uint64_t index) const
    {
        return m_directory / (m_name + '-' + std::to_string(level) + '-' + std::to_string(index));
    }
    void flush()
    {
        if (m_rows.empty()) return;
        check_cancel(m_context);
        std::sort(m_rows.begin(), m_rows.end());
        std::ofstream output(run(0, m_runs++), std::ios::binary);
        require_io(static_cast<bool>(output));
        for (const auto& line : m_rows) {
            check_cancel(m_context);
            write_line(output, line);
        }
        output.close();
        require_io(static_cast<bool>(output));
        m_rows.clear();
        m_bytes = 0;
    }
    fs::path m_directory;
    std::string m_name;
    addon::Invocation_context& m_context;
    std::vector<std::string> m_rows;
    std::size_t m_bytes = 0;
    std::uint64_t m_runs = 0;
};

class Row_stream final
{
public:
    explicit Row_stream(const fs::path& path) : m_input(path, std::ios::binary)
    {
        require_io(static_cast<bool>(m_input));
        next();
    }
    bool available() const { return m_available; }
    const std::string& key() const { return m_key; }
    Json value() const
    {
        auto value = json_util::parse(std::string_view(m_line).substr(m_key.size() + 1));
        if (!value) throw std::runtime_error("usage_cache_record_invalid");
        return std::move(*value);
    }
    void next()
    {
        m_available = read_line(m_input, m_line);
        if (!m_available) return;
        const auto tab = m_line.find('\t');
        if (tab == std::string::npos) throw std::runtime_error("usage_cache_record_invalid");
        m_key = m_line.substr(0, tab);
    }
private:
    std::ifstream m_input;
    std::string m_line;
    std::string m_key;
    bool m_available = false;
};

// The sorted observations and their offsets stay on disk. Ancestor lookup
// therefore does not turn the retained history into an in-memory graph.
class Row_lookup final
{
public:
    Row_lookup(const fs::path& rows, const fs::path& offsets, addon::Invocation_context& context)
        : m_rows(rows, std::ios::binary)
    {
        require_io(static_cast<bool>(m_rows));
        std::ofstream output(offsets, std::ios::binary);
        require_io(static_cast<bool>(output));
        std::string line;
        for (;;) {
            check_cancel(context);
            const auto position = static_cast<std::uint64_t>(m_rows.tellg());
            if (!read_line(m_rows, line)) break;
            output.write(reinterpret_cast<const char*>(&position), sizeof(position));
            ++m_count;
        }
        output.close();
        require_io(static_cast<bool>(output));
        m_offsets.open(offsets, std::ios::binary);
        require_io(static_cast<bool>(m_offsets));
    }

    std::uint64_t size() const { return m_count; }

    std::pair<std::string, Json> at(std::uint64_t index)
    {
        require_io(index < m_count);
        std::uint64_t position = 0;
        m_offsets.clear();
        m_offsets.seekg(static_cast<std::streamoff>(index * sizeof(position)));
        m_offsets.read(reinterpret_cast<char*>(&position), sizeof(position));
        require_io(static_cast<bool>(m_offsets));
        m_rows.clear();
        m_rows.seekg(static_cast<std::streamoff>(position));
        std::string line;
        require_io(read_line(m_rows, line));
        const auto delimiter = line.find('\t');
        require_io(delimiter != std::string::npos);
        auto value = json_util::parse(std::string_view(line).substr(delimiter + 1));
        if (!value) throw std::runtime_error("usage_cache_record_invalid");
        return {line.substr(0, delimiter), std::move(*value)};
    }

    std::optional<std::uint64_t> find(std::string_view key)
    {
        std::uint64_t begin = 0, end = m_count;
        while (begin < end) {
            const auto middle = begin + (end - begin) / 2;
            const auto row = at(middle);
            if (row.first == key) return middle;
            if (row.first < key) begin = middle + 1;
            else end = middle;
        }
        return std::nullopt;
    }

private:
    std::ifstream m_rows;
    std::ifstream m_offsets;
    std::uint64_t m_count = 0;
};

class Event_stream final
{
public:
    Event_stream(const fs::path& path, std::uint64_t begin, std::uint64_t end)
        : m_input(path, std::ios::binary), m_end(end)
    {
        require_io(m_input && begin <= end);
        m_input.seekg(static_cast<std::streamoff>(begin));
    }

    bool next(std::string& line)
    {
        const auto position = static_cast<std::uint64_t>(m_input.tellg());
        require_io(position <= m_end);
        if (position == m_end) return false;
        require_io(read_line(m_input, line) && static_cast<std::uint64_t>(m_input.tellg()) <= m_end);
        return true;
    }

private:
    std::ifstream m_input;
    std::uint64_t m_end;
};

class Ownership_origins final
{
public:
    Ownership_origins(const fs::path& work, std::uint64_t files, addon::Invocation_context& context)
        : m_path(work / "event-origins")
    {
        std::ofstream offsets(work / "origin-offsets", std::ios::binary);
        require_io(static_cast<bool>(offsets));
        const std::array<std::uint64_t, 2> empty{};
        for (std::uint64_t index = 0; index < files; ++index) {
            check_cancel(context);
            offsets.write(reinterpret_cast<const char*>(empty.data()), sizeof(empty));
        }
        offsets.close();
        require_io(static_cast<bool>(offsets));
        m_offsets.open(work / "origin-offsets", std::ios::binary | std::ios::in | std::ios::out);
        m_output.open(m_path, std::ios::binary);
        require_io(m_offsets && m_output);
    }

    void begin() { m_begin = static_cast<std::uint64_t>(m_output.tellp()); }
    void append(const Json& owner) { write_line(m_output, addon::serialize_json(owner)); }
    void finish(std::uint64_t index)
    {
        const std::array<std::uint64_t, 2> range{m_begin, static_cast<std::uint64_t>(m_output.tellp())};
        m_output.flush();
        m_offsets.clear();
        m_offsets.seekp(static_cast<std::streamoff>(index * sizeof(range)));
        m_offsets.write(reinterpret_cast<const char*>(range.data()), sizeof(range));
        require_io(m_output && m_offsets);
    }
    Event_stream read(std::uint64_t index)
    {
        std::array<std::uint64_t, 2> range{};
        m_offsets.clear();
        m_offsets.seekg(static_cast<std::streamoff>(index * sizeof(range)));
        m_offsets.read(reinterpret_cast<char*>(range.data()), sizeof(range));
        require_io(static_cast<bool>(m_offsets));
        return Event_stream(m_path, range[0], range[1]);
    }

private:
    fs::path m_path;
    std::fstream m_offsets;
    std::ofstream m_output;
    std::uint64_t m_begin = 0;
};

// Read the JSONL header one member at a time, then stream each cache row.
class Cache_reader final
{
public:
    Cache_reader(const fs::path& path, addon::Invocation_context& context)
        : m_input(path, std::ios::binary), m_context(context)
    {
        require_io(static_cast<bool>(m_input));
    }
    void read(std::string_view owner, const std::function<void(const Json&)>& file,
        const std::function<void(const Json&)>& receipt,
        const std::function<void(const Json&)>& event)
    {
        expect('{');
        auto header = Json::object();
        std::set<std::string> keys;
        for (;;) {
            const auto key = value();
            if (!key.is_string() || (key.as_string() != "version" &&
                key.as_string() != "profile" && key.as_string() != "format") ||
                !keys.insert(key.as_string()).second) invalid();
            expect(':');
            header.set(key.as_string(), value());
            if (peek() == '}') break;
            expect(',');
        }
        expect('}');
        if (count(json_util::member(header, "version")) != 1 || json_util::text(header, "profile") != owner) invalid();
        if (!json_util::exact_keys(header, {"version", "profile", "format"}) || json_util::text(header, "format") != "jsonl") invalid();
        // Header and each following row are separate complete JSON documents.
        std::string line;
        auto delimiter = m_input.get();
        if (delimiter == '\r') delimiter = m_input.get();
        require_io(delimiter == '\n');
        while (read_line(m_input, line)) {
            check_cancel(m_context);
            const auto row = json_util::parse(line);
            if (!row || !row->is_object()) invalid();
            if (row->find("receipt")) {
                if (!json_util::exact_keys(*row, {"receipt"})) invalid();
                receipt(json_util::member(*row, "receipt"));
            }
            else if (row->find("fork_event")) {
                if (!json_util::exact_keys(*row, {"fork_event"})) invalid();
                event(json_util::member(*row, "fork_event"));
            }
            else file(*row);
        }
    }
private:
    [[noreturn]] static void invalid() { throw std::runtime_error("usage_cache_invalid"); }
    int peek()
    {
        while (m_input && (m_input.peek() == ' ' || m_input.peek() == '\r' || m_input.peek() == '\n' || m_input.peek() == '\t')) get();
        return m_input.peek();
    }
    int get()
    {
        if (++m_bytes % 4096 == 0) check_cancel(m_context);
        const auto byte = m_input.get();
        require_io(!m_input.bad());
        return byte;
    }
    void expect(char byte)
    {
        if (peek() != byte) invalid();
        get();
    }
    Json value()
    {
        check_cancel(m_context);
        peek();
        std::string bytes;
        int depth = 0;
        bool quoted = false, escaped = false;
        for (;;) {
            const int next = m_input.peek();
            if (next == EOF) break;
            const char byte = static_cast<char>(next);
            if (!quoted && depth == 0 && !bytes.empty() &&
                (byte == ',' || byte == ':' || byte == ']' || byte == '}' || byte == ' ' || byte == '\r' || byte == '\n' || byte == '\t')) break;
            bytes.push_back(static_cast<char>(get()));
            if (bytes.size() > addon::Json_parse_limits{}.max_bytes) invalid();
            if (quoted) {
                if (escaped) escaped = false;
                else if (byte == '\\') escaped = true;
                else if (byte == '"') quoted = false;
            }
            else if (byte == '"') quoted = true;
            else if (byte == '{' || byte == '[') ++depth;
            else if (byte == '}' || byte == ']') --depth;
            if (!quoted && depth == 0 && (byte == '"' || byte == '}' || byte == ']')) break;
        }
        auto parsed = json_util::parse(bytes);
        if (!parsed) invalid();
        return std::move(*parsed);
    }
    std::ifstream m_input;
    addon::Invocation_context& m_context;
    std::uint64_t m_bytes = 0;
};

Json encode_file_accounting(std::string_view path, const File_record& file)
{
    return json_util::object({{"path", Json(path)}, {"size", Json(file.size)},
        {"modified_msecs", Json(file.modified_ms)}, {"recorded_cwd", Json(file.cwd)},
        {"stale_breakdown", Json(file.stale_breakdown)},
        {"usage", file.usage ? addon::encode_stored_usage_accounting(*file.usage) : Json{}}});
}

Json encode_file(std::string_view path, const File_record& file)
{
    auto row = encode_file_accounting(path, file);
    if (file.fork_evidence) {
        row.set("usage-fork-ownership", json_util::object({{"version", Json(1)},
            {"parent_sha256_hex", Json(addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(row))))},
            {"evidence", *file.fork_evidence}}));
    }
    else if (file.fork_carrier) row.set("usage-fork-ownership", *file.fork_carrier);
    if (file.usage && file.usage->temporal) {
        auto evidence = *file.usage->temporal;
        evidence.retained_from.reset();
        auto carrier = json_util::object({{"version", Json(1)},
            {"parent_sha256_hex", Json(addon::to_hex(addon::Sha256::digest(
                addon::serialize_json_canonical(encode_file_accounting(path, file)))))},
            {"evidence", addon::encode_usage_temporal(evidence)}});
        row.set("usage-temporal", std::move(carrier));
    }
    else if (file.temporal_carrier) row.set("usage-temporal", *file.temporal_carrier);
    if (addon::serialize_json(row).size() > addon::Json_parse_limits{}.max_bytes) row.erase("usage-temporal");
    return row;
}

File_record decode_file(const Json& row)
{
    auto accounting = row;
    accounting.erase("usage-temporal");
    accounting.erase("usage-fork-ownership");
    File_record file;
    const auto path = json_util::text(row, "path");
    if (!json_util::exact_keys(accounting, {"path", "size", "modified_msecs", "recorded_cwd", "usage", "stale_breakdown"}) ||
        !addon::path_from_utf8(path).is_absolute() || !json_util::member(row, "modified_msecs").is_integer() ||
        !json_util::member(row, "stale_breakdown").is_boolean())
    {
        throw std::runtime_error("usage_cache_invalid");
    }
    file.stale_breakdown = json_util::member(row, "stale_breakdown").as_boolean();
    file.size = count(json_util::member(row, "size"));
    file.modified_ms = json_util::member(row, "modified_msecs").as_integer();
    file.cwd = json_util::text(row, "recorded_cwd");
    if (!json_util::member(row, "usage").is_null()) {
        addon::Contract_failure failure;
        file.usage = addon::decode_stored_usage_accounting(json_util::member(row, "usage"), &failure);
        if (!file.usage) throw std::runtime_error("usage_cache_record_invalid");
    }
    if (const auto* carrier = row.find("usage-fork-ownership")) {
        file.fork_carrier = *carrier;
        if (json_util::exact_keys(*carrier, {"version", "parent_sha256_hex", "evidence"}) &&
            json_util::member(*carrier, "version").is_integer() && json_util::member(*carrier, "version").as_integer() == 1 &&
            json_util::text(*carrier, "parent_sha256_hex") ==
                addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(accounting))) &&
            valid_fork_evidence(json_util::member(*carrier, "evidence"), addon::path_from_utf8(path)))
        {
            file.fork_evidence = json_util::member(*carrier, "evidence");
        }
    }
    if (const auto* carrier = row.find("usage-temporal")) {
        file.temporal_carrier = *carrier;
        if (file.usage && json_util::exact_keys(*carrier, {"version", "parent_sha256_hex", "evidence"}) &&
            json_util::member(*carrier, "version").is_integer() && json_util::member(*carrier, "version").as_integer() == 1 &&
            json_util::text(*carrier, "parent_sha256_hex") ==
                addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(accounting))) &&
            addon::serialize_json(*carrier).size() <= addon::k_max_usage_temporal_bytes)
        {
            auto evidence = addon::decode_usage_temporal(json_util::member(*carrier, "evidence"), nullptr);
            if (evidence && evidence->parent.record_id == file.usage->record_id &&
                evidence->parent.total_tokens == file.usage->total_tokens &&
                evidence->parent.snapshot_id == "provider-file" && !evidence->parent.source_account_key &&
                !evidence->retained_from &&
                std::all_of(evidence->portions.begin(), evidence->portions.end(), [](const auto& portion) {
                    return portion.basis == "instant" && portion.instant.size() == 24 &&
                        portion.instant[10] == 'T' && portion.instant[19] == '.' && portion.instant.back() == 'Z' &&
                        portion.instant.substr(17, 2) != "60" &&
                        portion.instant >= "1970-01-01T00:00:00.000Z";
                }))
            {
                evidence->retained_from.reset();
                file.usage->temporal = std::move(evidence);
            }
        }
    }
    return file;
}


// Flush a completed spool, then atomically replace the cache. No history-sized
// buffer is needed, and cancellation cannot publish a partially written file.
void store_cache(const fs::path& destination, const fs::path& temporary)
{
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#ifdef _WIN32
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("usage_cache_write_failed");
    const bool saved = FlushFileBuffers(handle);
    CloseHandle(handle);
    const bool replaced = saved && MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CLOEXEC);
    if (descriptor < 0) throw std::runtime_error("usage_cache_write_failed");
    const bool saved = fsync(descriptor) == 0;
    close(descriptor);
    bool replaced = saved && rename(temporary.c_str(), destination.c_str()) == 0;
    if (replaced) {
        const int directory = open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        replaced = directory >= 0 && fsync(directory) == 0;
        if (directory >= 0) close(directory);
    }
#endif
    if (!replaced) throw std::runtime_error("usage_cache_write_failed");
}

std::optional<File_record> refresh_file(const fs::path& path, const std::optional<File_record>& previous,
    std::string_view mode, const Json& workspaces, const fs::path& temporary,
    std::ofstream& events, addon::Invocation_context& context)
{
    const auto session = path.parent_path().parent_path().parent_path();
    bool missing_state = false;
    const auto identity = fork_session_identity(path, missing_state);
    // state.json can briefly disappear during Kimi's Windows replacement.
    // Retention is safe; treating that interval as a new root is not.
    if (!identity) {
        if (previous) return previous;
        if (missing_state) throw Usage_observation_unsettled();
        throw Usage_ownership_unavailable();
    }
    if (previous && previous->fork_evidence &&
        addon::serialize_json_canonical(json_util::member(*previous->fork_evidence, "identity")) !=
            addon::serialize_json_canonical(*identity)) throw Usage_ownership_unavailable();
    const auto cwd = recorded_cwd(session, workspaces);
    const bool refresh_temporal = previous && previous->usage && !previous->temporal_carrier;
    if (mode == "incremental" && previous && previous->fork_evidence &&
        !previous->stale_breakdown && !refresh_temporal)
    {
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (!error) {
            const auto modified = fs::last_write_time(path, error);
            if (!error && previous->size == size && previous->modified_ms == file_modified_ms(modified)) {
                auto current = *previous;
                current.cwd = cwd;
                current.retained_prefix_matches = true;
                current.accepted_event_count = count(json_util::member(*current.fork_evidence, "event_count"));
                if (current.usage) attribute(*current.usage, cwd);
                return current;
            }
        }
    }
    File_record current;
    current.cwd = cwd;
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    require_io(static_cast<bool>(output));
    Wire_usage wire(session), prefix(session);
    addon::Sha256 native_digest, prefix_digest, event_digest;
    std::uint64_t bytes = 0, line_end = 0, event_count = 0, boundary = 0;
    bool forked = false;
    std::string creator_session_id;
    auto read = Wire_read::UNAVAILABLE;
    try {
        read = read_native_usage_lines(path, current, context,
            [&](const std::optional<Json>& event, std::uint64_t physical_bytes) {
                line_end += physical_bytes;
                if (!event) {
                    return;
                }
                const auto type = json_util::text(*event, "type");
                if (type == "metadata" && json_util::member(*event, "protocol_version").is_string() &&
                    json_util::member(*event, "created_at").is_number())
                {
                    creator_session_id = json_util::text(*event, "creator_session_id");
                }
                if (type == "forked") {
                    if (json_util::text(*event, "agentId") != json_util::text(*identity, "agent_id")) {
                        throw Usage_ownership_unavailable();
                    }
                    boundary = event_count;
                    forked = true;
                }
                if (type != "usage.record") {
                    return;
                }
                if (event->find("agentId") && json_util::text(*event, "agentId") != json_util::text(*identity, "agent_id")) {
                    throw Usage_ownership_unavailable();
                }
                wire.consume_event(*event, context);
                if (previous && line_end <= previous->size) {
                    prefix.consume_event(*event, context);
                    ++current.accepted_event_count;
                }
                const auto canonical = addon::serialize_json_canonical(*event);
                write_line(output, canonical);
                event_digest.update(canonical);
                event_digest.update("\n");
                ++event_count;
        }, [&](std::string_view chunk) {
            native_digest.update(chunk);
            if (previous && bytes < previous->size) {
                prefix_digest.update(chunk.substr(0, static_cast<std::size_t>(
                    std::min<std::uint64_t>(chunk.size(), previous->size - bytes))));
            }
            bytes += chunk.size();
        });
    }
    catch (...) {
        check_cancel(context);
        if (!previous || (!previous->stale_breakdown && !refresh_temporal) || previous->size != current.size ||
            previous->modified_ms != current.modified_ms) throw;
        return previous;
    }
    output.close();
    require_io(static_cast<bool>(output));
    const auto after_identity = fork_session_identity(path, missing_state);
    const bool identity_changed = after_identity && addon::serialize_json_canonical(*after_identity) !=
        addon::serialize_json_canonical(*identity);
    if (read != Wire_read::STABLE || !after_identity || identity_changed) {
        if (previous) return previous;
        if ((!after_identity && !missing_state) || read == Wire_read::UNAVAILABLE) {
            throw Usage_ownership_unavailable();
        }
        throw Usage_observation_unsettled();
    }
    current.retained_prefix_matches = previous && previous->fork_evidence && bytes >= previous->size &&
        addon::to_hex(prefix_digest.finish()) == json_util::text(*previous->fork_evidence, "native_prefix_sha256_hex");
    current.usage = wire.finish(cwd, previous && previous->usage ? &*previous->usage : nullptr);
    if (previous && previous->usage && current.size == previous->size &&
        current.modified_ms == previous->modified_ms)
    {
        auto observed = prefix.finish(previous->cwd);
        if (observed) {
            // This is the legacy scanner's numeric correlation, not an event
            // identity proof. The correction operation decides its authority.
            current.legacy_prefix_matches = same_counted_usage(*observed, *previous->usage);
        }
    }
    current.fork_evidence = json_util::object({{"identity", *identity},
        {"native_prefix_sha256_hex", Json(addon::to_hex(native_digest.finish()))},
        {"event_sha256_hex", Json(addon::to_hex(event_digest.finish()))},
        {"event_count", Json(event_count)}, {"fork_event_count", Json(boundary)},
        {"forked", Json(forked)}, {"accounting", Json("raw")}});
    if (!creator_session_id.empty()) current.fork_evidence->set("creator_session_id", Json(creator_session_id));
    if (previous && ((previous->fork_evidence && json_util::text(*previous->fork_evidence, "accounting") == "retained") ||
        (!previous->fork_evidence && previous->fork_carrier)))
    {
        current.fork_evidence->set("accounting", Json("retained"));
        current.fork_evidence->set("unresolved_event_count", current.retained_prefix_matches && previous->fork_evidence ?
            json_util::member(*previous->fork_evidence, "unresolved_event_count") : Json(event_count));
    }
    current.event_begin = static_cast<std::uint64_t>(events.tellp());
    std::ifstream input(temporary, std::ios::binary);
    require_io(static_cast<bool>(input));
    std::string line;
    while (read_line(input, line)) {
        check_cancel(context);
        write_line(events, line);
    }
    current.event_end = static_cast<std::uint64_t>(events.tellp());
    return current;
}

Json observation(std::string_view path, const File_record& file,
    const std::optional<File_record>& previous = std::nullopt)
{
    auto row = json_util::object({{"file", encode_file(path, file)},
        {"event_begin", Json(file.event_begin)}, {"event_end", Json(file.event_end)},
        {"accepted_event_count", Json(file.accepted_event_count)},
        {"legacy_prefix_matches", Json(file.legacy_prefix_matches)},
        {"retained_prefix_matches", Json(file.retained_prefix_matches)},
        {"previous", previous ? encode_file(path, *previous) : Json{}},
        {"previous_event_begin", Json(previous ? previous->event_begin : 0)},
        {"previous_event_end", Json(previous ? previous->event_end : 0)}});
    if (file.usage && file.usage->temporal && file.usage->temporal->retained_from) {
        const auto& retained = *file.usage->temporal->retained_from;
        row.set("retained_from", json_util::object({{"snapshot_id", Json(retained.snapshot_id)},
            {"manifest_sha256_hex", Json(retained.manifest_sha256_hex)}}));
    }
    return row;
}

File_record observed_file(const Json& row)
{
    auto file = decode_file(json_util::member(row, "file"));
    file.event_begin = count(json_util::member(row, "event_begin"));
    file.event_end = count(json_util::member(row, "event_end"));
    file.accepted_event_count = count(json_util::member(row, "accepted_event_count"));
    file.legacy_prefix_matches = json_util::member(row, "legacy_prefix_matches").as_boolean();
    file.retained_prefix_matches = json_util::member(row, "retained_prefix_matches").as_boolean();
    if (file.usage && file.usage->temporal && row.find("retained_from")) {
        const auto& retained = json_util::member(row, "retained_from");
        file.usage->temporal->retained_from = addon::Usage_temporal_retained_from{
            json_util::text(retained, "snapshot_id"), json_util::text(retained, "manifest_sha256_hex")};
    }
    return file;
}

// A copied occurrence is suppressed only through its direct parent, whose
// own chain must close at an original counted in this same candidate. Keeping
// both status and the traversal stack on disk also bounds a deeply nested fork.
void validate_fork_chains(Row_lookup& files, const fs::path& events, const fs::path& work,
    Ownership_origins& origins, addon::Invocation_context& context)
{
    const auto status_path = work / "fork-status";
    {
        std::ofstream output(status_path, std::ios::binary);
        require_io(static_cast<bool>(output));
        const std::array<char, 8192> zeros{};
        for (std::uint64_t remaining = files.size(); remaining;) {
            check_cancel(context);
            const auto bytes = std::min<std::uint64_t>(remaining, zeros.size());
            output.write(zeros.data(), static_cast<std::streamsize>(bytes));
            remaining -= bytes;
        }
        output.close();
        require_io(static_cast<bool>(output));
    }
    std::fstream status(status_path, std::ios::binary | std::ios::in | std::ios::out);
    std::fstream stack(work / "fork-stack", std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    require_io(status && stack);
    const auto get_status = [&](std::uint64_t index) {
        status.clear();
        status.seekg(static_cast<std::streamoff>(index));
        const auto result = status.get();
        require_io(result != EOF);
        return result;
    };
    const auto set_status = [&](std::uint64_t index, char value) {
        status.clear();
        status.seekp(static_cast<std::streamoff>(index));
        status.put(value);
        require_io(static_cast<bool>(status));
    };
    const auto write_origins = [&](std::uint64_t index, bool proved) {
        const auto row = files.at(index).second;
        const auto file = observed_file(row);
        if (!file.fork_evidence) return;
        const auto& evidence = *file.fork_evidence;
        const auto& identity = json_util::member(evidence, "identity");
        const auto path = addon::path_from_utf8(json_util::text(json_util::member(row, "file"), "path"));
        const bool retained = json_util::text(evidence, "accounting") == "retained";
        const auto boundary = proved ? count(json_util::member(evidence, "fork_event_count")) : 0;
        const auto unresolved = retained ? count(json_util::member(evidence, "unresolved_event_count")) :
            (proved ? 0 : count(json_util::member(evidence, "event_count")));
        std::optional<Event_stream> parent_events, parent_origins;
        if (boundary) {
            const auto parent_path = path.parent_path().parent_path().parent_path().parent_path() /
                addon::path_from_utf8(json_util::text(identity, "parent_session_id")) /
                "agents" / path.parent_path().filename() / "wire.jsonl";
            const auto parent_index = files.find(path_key(parent_path));
            require_io(parent_index.has_value());
            const auto parent = observed_file(files.at(*parent_index).second);
            parent_events.emplace(events, parent.event_begin, parent.event_end);
            parent_origins.emplace(origins.read(*parent_index));
            if (json_util::text(*parent.fork_evidence, "accounting") == "retained") {
                std::string unused;
                for (std::uint64_t skipped = 0;
                    skipped < count(json_util::member(*parent.fork_evidence, "unresolved_event_count")); ++skipped)
                {
                    check_cancel(context);
                    require_io(parent_events->next(unused) && parent_origins->next(unused));
                }
            }
        }
        origins.begin();
        Event_stream input(events, file.event_begin, file.event_end);
        std::string copied, original, owner;
        std::uint64_t occurrence = 0;
        while (input.next(copied)) {
            check_cancel(context);
            if (occurrence < boundary) {
                do {
                    check_cancel(context);
                    require_io(parent_events->next(original) && parent_origins->next(owner));
                } while (original != copied);
                const auto parsed = json_util::parse(owner);
                require_io(parsed && parsed->is_string());
                origins.append(*parsed);
            }
            else origins.append(occurrence < unresolved ? Json{} : Json("kimi:" + json_util::text(identity, "session_id")));
            ++occurrence;
        }
        origins.finish(index);
    };
    for (std::uint64_t first = 0; first < files.size(); ++first) {
        check_cancel(context);
        if (get_status(first) >= 2) continue;
        if (!observed_file(files.at(first).second).fork_evidence) { set_status(first, 3); continue; }
        std::uint64_t current = first, depth = 0;
        bool proved = true;
        for (;;) {
            check_cancel(context);
            const auto state = get_status(current);
            if (state == 2) break;
            if (state == 1 || state == 3) { proved = false; break; }
            set_status(current, 1);
            stack.clear();
            stack.seekp(static_cast<std::streamoff>(depth++ * sizeof(current)));
            stack.write(reinterpret_cast<const char*>(&current), sizeof(current));
            require_io(static_cast<bool>(stack));
            const auto row = files.at(current).second;
            const auto file = observed_file(row);
            if (!file.fork_evidence) { proved = false; break; }
            const auto& evidence = *file.fork_evidence;
            if (json_util::text(evidence, "accounting") == "retained") { proved = false; break; }
            const auto& identity = json_util::member(evidence, "identity");
            const auto parent_id = json_util::text(identity, "parent_session_id");
            const bool forked = json_util::member(evidence, "forked").as_boolean();
            if (json_util::text(evidence, "creator_session_id") == json_util::text(identity, "session_id")) {
                if (forked) proved = false;
                break;
            }
            if (parent_id.empty()) {
                if (forked) proved = false;
                break;
            }
            if (!forked) { proved = false; break; }
            const auto path = addon::path_from_utf8(json_util::text(json_util::member(row, "file"), "path"));
            const auto session = path.parent_path().parent_path().parent_path();
            const auto parent_path = session.parent_path() / addon::path_from_utf8(parent_id) /
                "agents" / path.parent_path().filename() / "wire.jsonl";
            const auto parent_index = files.find(path_key(parent_path));
            if (!parent_index) { proved = false; break; }
            const auto parent = observed_file(files.at(*parent_index).second);
            if (!parent.fork_evidence) { proved = false; break; }
            Event_stream child_events(events, file.event_begin, file.event_end);
            Event_stream parent_events(events, parent.event_begin, parent.event_end);
            std::string copied, original;
            const bool retained_parent = json_util::text(*parent.fork_evidence, "accounting") == "retained";
            if (retained_parent) {
                const auto unresolved = count(json_util::member(*parent.fork_evidence, "unresolved_event_count"));
                for (std::uint64_t occurrence = 0; occurrence < unresolved; ++occurrence) {
                    check_cancel(context);
                    if (!parent_events.next(original)) throw std::runtime_error("usage_cache_record_invalid");
                }
            }
            const auto boundary = count(json_util::member(evidence, "fork_event_count"));
            for (std::uint64_t occurrence = 0; occurrence < boundary; ++occurrence) {
                check_cancel(context);
                if (!child_events.next(copied)) throw std::runtime_error("usage_cache_record_invalid");
                bool found = false;
                while (parent_events.next(original)) {
                    check_cancel(context);
                    if (original == copied) { found = true; break; }
                }
                if (!found) { proved = false; break; }
            }
            if (retained_parent) write_origins(*parent_index, false);
            if (!proved || retained_parent) break;
            current = *parent_index;
        }
        while (depth) {
            check_cancel(context);
            stack.clear();
            stack.seekg(static_cast<std::streamoff>(--depth * sizeof(current)));
            stack.read(reinterpret_cast<char*>(&current), sizeof(current));
            require_io(static_cast<bool>(stack));
            write_origins(current, proved);
            set_status(current, proved ? 2 : 3);
        }
    }
}

void retain_owned_usage(File_record& file, const fs::path& path, const fs::path& events,
    addon::Invocation_context& context)
{
    if (!file.fork_evidence || json_util::text(*file.fork_evidence, "accounting") == "owned") return;
    Wire_usage owned(path.parent_path().parent_path().parent_path());
    const auto boundary = count(json_util::member(*file.fork_evidence, "fork_event_count"));
    Event_stream input(events, file.event_begin, file.event_end);
    std::string line;
    std::uint64_t occurrence = 0;
    while (input.next(line)) {
        check_cancel(context);
        if (occurrence++ < boundary) continue;
        const auto event = json_util::parse(line);
        if (!event) throw std::runtime_error("usage_cache_record_invalid");
        owned.consume_event(*event, context);
    }
    file.usage = owned.finish(file.cwd, file.usage ? &*file.usage : nullptr);
    file.temporal_carrier.reset();
    file.fork_evidence->set("accounting", Json("owned"));
}

void merge_usage(addon::Usage_record& accumulated, const addon::Usage_record& record,
    bool& mixed_coverage, addon::Invocation_context& context)
{
    if (!record.source_path || !accumulated.source_path ||
        path_key(addon::path_from_utf8(*record.source_path)) != path_key(addon::path_from_utf8(*accumulated.source_path)))
    {
        throw std::runtime_error("usage_session_path_conflict");
    }
    merge_temporal(accumulated, record, context);
    add(accumulated.total_tokens, record.total_tokens);
    accumulated.completed_date = std::max(accumulated.completed_date, record.completed_date);
    if (!accumulated.model || accumulated.model->empty()) accumulated.model = record.model;
    else if (record.model && !record.model->empty() && record.model != accumulated.model) accumulated.model = "multiple";
    if (accumulated.coverage_unit != record.coverage_unit) mixed_coverage = true;
    auto target = amounts(accumulated);
    auto source_copy = record;
    const auto source = amounts(source_copy);
    for (std::size_t index = 0; index < target.size(); ++index) {
        const bool overflow = (source[index]->coverage && source[index]->coverage->overflow) ||
            (target[index]->coverage && target[index]->coverage->overflow);
        if (overflow) target[index]->value.reset();
        else if (source[index]->value) {
            if (!target[index]->value) target[index]->value = 0;
            add(*target[index]->value, *source[index]->value);
        }
        if (source[index]->coverage && target[index]->coverage) {
            target[index]->coverage->overflow = overflow;
            add(target[index]->coverage->covered_records, source[index]->coverage->covered_records);
            add(target[index]->coverage->total_records, source[index]->coverage->total_records);
            auto& support = target[index]->coverage->support;
            const auto incoming = source[index]->coverage->support;
            if (support == addon::Usage_support::SUPPORTED || incoming == addon::Usage_support::SUPPORTED) support = addon::Usage_support::SUPPORTED;
            else if (support != incoming) support = addon::Usage_support::UNKNOWN;
            target[index]->applicable = support != addon::Usage_support::UNSUPPORTED;
            if (!target[index]->applicable) target[index]->value.reset();
            else if (!overflow && !target[index]->value) target[index]->value = 0;
        }
    }
    auto& allocation = *accumulated.repository_usage;
    add(allocation.unattributed_tokens, record.repository_usage->unattributed_tokens);
    for (const auto& item : record.repository_usage->allocations) {
        check_cancel(context);
        const auto same = std::find_if(allocation.allocations.begin(), allocation.allocations.end(), [&](const auto& previous) {
            return path_key(addon::path_from_utf8(previous.repository_path)) == path_key(addon::path_from_utf8(item.repository_path));
        });
        if (same == allocation.allocations.end()) allocation.allocations.push_back(item);
        else add(same->total_tokens, item.total_tokens);
    }
    // The SDK's per-record contract also bounds allocations in a session
    // spanning many files; it is not a bound on the number of sessions.
    addon::Contract_failure failure;
    if (!addon::decode_stored_usage_accounting(addon::encode_stored_usage_accounting(accumulated), &failure)) {
        throw std::runtime_error("usage_result_unrepresentable");
    }
}

struct Scan_result
{
    std::unique_ptr<Scan_workspace> workspace;
    fs::path records;
};

std::optional<File_record> previous_file(const Json& row)
{
    if (json_util::member(row, "previous").is_null()) return std::nullopt;
    auto previous = decode_file(json_util::member(row, "previous"));
    previous.event_begin = count(json_util::member(row, "previous_event_begin"));
    previous.event_end = count(json_util::member(row, "previous_event_end"));
    return previous;
}

std::optional<addon::Usage_record> event_usage(const File_record& file, const fs::path& path,
    const fs::path& events, std::uint64_t first, std::uint64_t last, addon::Invocation_context& context)
{
    Wire_usage usage(path.parent_path().parent_path().parent_path());
    Event_stream input(events, file.event_begin, file.event_end);
    std::string line;
    std::uint64_t occurrence = 0;
    while (occurrence < last && input.next(line)) {
        check_cancel(context);
        if (occurrence++ >= first) usage.consume(line, context);
    }
    return usage.finish(file.cwd);
}

void accumulate_usage(std::optional<addon::Usage_record>& target,
    const std::optional<addon::Usage_record>& incoming, addon::Invocation_context& context,
    std::uint64_t files = 1)
{
    if (!incoming) return;
    if (!target) target = incoming;
    else {
        bool mixed = false;
        merge_usage(*target, *incoming, mixed, context);
        if (mixed) {
            target->coverage_unit = addon::Usage_coverage_unit::SESSIONS;
            for (auto* field : amounts(*target)) {
                field->applicable = true;
                field->value = 0;
                field->coverage = addon::Usage_field_coverage{0, files, false, addon::Usage_support::UNKNOWN};
            }
        }
    }
}

struct File_adjustment
{
    File_record ordinary;
    File_record corrected;
    std::optional<addon::Usage_record> removed;
    std::optional<addon::Usage_record> growth;
    bool raw_establishment = false;
};

std::string record_key(std::string_view id)
{
    return addon::to_hex(reinterpret_cast<const std::uint8_t*>(id.data()), id.size());
}

void validate_predecessor_path(const fs::path& path, const fs::path& storage)
{
    const auto relative = path.lexically_relative(storage);
    if (!path.is_absolute() || path != path.lexically_normal() || relative.empty() ||
        relative == "." || relative.is_absolute() || !fs::is_regular_file(cache_io_path(path)) ||
        std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }))
    {
        throw Usage_ownership_unavailable();
    }
    for (auto current = path; !current.empty(); current = current.parent_path()) {
        std::error_code error;
        if (fs::is_symlink(fs::symlink_status(cache_io_path(current), error)) || error) {
            throw Usage_ownership_unavailable();
        }
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(cache_io_path(current).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw Usage_ownership_unavailable();
        }
#endif
        if (current == current.root_path()) break;
    }
}

fs::path read_predecessors(const addon::Usage_owned_event_correction_request& request,
    const fs::path& storage, const fs::path& work, addon::Invocation_context& context)
{
    const auto path = addon::path_from_utf8(request.predecessors.path);
    validate_predecessor_path(path, storage);
    Row_sorter rows(work, "host-predecessors", context);
    addon::Sha256 digest;
    File_record fingerprint;
    bool header = false;
    char last_byte = 0;
    std::string previous_id;
    const addon::Usage_owned_event_predecessor_header expected{"logonomic.cli.kimi",
        request.scan.context.account_context, request.scan.context.profile_storage->root,
        request.scan.cache_selection->accepted};
    auto read = Wire_read::UNAVAILABLE;
    try {
        read = read_stable_lines(path, fingerprint, context, [&](std::string_view line) {
            const auto value = json_util::parse(line);
            if (!value) throw Usage_ownership_unavailable();
            if (!header) {
                const auto decoded = addon::decode_usage_owned_event_predecessor_header(*value, nullptr);
                if (!decoded || addon::serialize_json_canonical(addon::encode_usage_owned_event_predecessor_header(*decoded)) !=
                    addon::serialize_json_canonical(addon::encode_usage_owned_event_predecessor_header(expected)))
                {
                    throw Usage_ownership_unavailable();
                }
                header = true;
                return;
            }
            const auto predecessor = addon::decode_usage_owned_event_predecessor(*value, nullptr);
            if (!predecessor || predecessor->record.record_id <= previous_id) throw Usage_ownership_unavailable();
            previous_id = predecessor->record.record_id;
            rows.add(record_key(previous_id), *value);
        }, [&](std::string_view bytes) {
            digest.update(bytes);
            if (!bytes.empty()) last_byte = bytes.back();
        });
    }
    catch (const std::exception&) {
        check_cancel(context);
        throw Usage_ownership_unavailable();
    }
    validate_predecessor_path(path, storage);
    if (read != Wire_read::STABLE || !header || last_byte != '\n' ||
        addon::to_hex(digest.finish()) != request.predecessors.sha256_hex) throw Usage_ownership_unavailable();
    return rows.finish();
}

File_adjustment prepare_file_adjustment(const Json& row, bool proved, bool accepted_selection,
    const fs::path& events, addon::Invocation_context& context)
{
    const auto path = addon::path_from_utf8(json_util::text(json_util::member(row, "file"), "path"));
    const auto previous = previous_file(row);
    File_adjustment result;
    result.ordinary = observed_file(row);
    auto& file = result.ordinary;
    if (!previous || !previous->usage) {
        if (!proved && file.usage) throw Usage_ownership_unavailable();
        retain_owned_usage(file, path, events, context);
        result.corrected = file;
        result.growth = file.usage;
        return result;
    }
    const auto previous_mode = previous->fork_evidence ? json_util::text(*previous->fork_evidence, "accounting") :
        (previous->fork_carrier ? "retained" : "raw");
    const bool bound = file.retained_prefix_matches ||
        (accepted_selection && !previous->fork_carrier && file.legacy_prefix_matches);
    const auto total = file.fork_evidence ? count(json_util::member(*file.fork_evidence, "event_count")) : 0;
    if (bound && file.fork_evidence) {
        result.growth = event_usage(file, path, events, file.accepted_event_count, total, context);
    }
    if (!proved || previous_mode == "retained") {
        file.usage = previous->usage;
        file.temporal_carrier = previous->temporal_carrier;
        accumulate_usage(file.usage, result.growth, context);
        if (file.fork_evidence) {
            file.fork_evidence->set("accounting", Json("retained"));
            const auto unresolved = bound ? (previous_mode == "retained" ?
                count(json_util::member(*previous->fork_evidence, "unresolved_event_count")) : file.accepted_event_count) : total;
            file.fork_evidence->set("unresolved_event_count", Json(unresolved));
        }
        result.corrected = file;
        return result;
    }
    if (previous_mode == "owned") {
        retain_owned_usage(file, path, events, context);
        result.raw_establishment = !bound;
        result.corrected = file;
        return result;
    }
    result.corrected = file;
    if (!bound) {
        // A rewritten legacy prefix is an ordinary raw observation first.
        // Acceptance binds that population; it does not claim old-event identity.
        result.raw_establishment = true;
        return result;
    }
    const auto boundary = count(json_util::member(*file.fork_evidence, "fork_event_count"));
    const auto removed_count = std::min(boundary, file.accepted_event_count);
    result.removed = event_usage(file, path, events, 0, removed_count, context);
    if (result.removed) {
        const auto& allocation = previous->usage->repository_usage;
        if (!allocation || allocation->allocations.size() > 1 ||
            (!allocation->allocations.empty() && allocation->unattributed_tokens))
        {
            result.removed.reset();
            return result;
        }
        addon::Usage_repository removed_repository;
        if (result.removed->total_tokens) {
            if (allocation->allocations.empty()) removed_repository.unattributed_tokens = result.removed->total_tokens;
            else removed_repository.allocations.push_back({allocation->allocations.front().repository_path,
                result.removed->total_tokens});
        }
        result.removed->repository_usage = std::move(removed_repository);
        retain_owned_usage(result.corrected, path, events, context);
    }
    return result;
}

fs::path prepare_corrected_sessions(Row_lookup& observed, Ownership_origins& origins,
    const addon::Usage_owned_event_correction_request& request, const fs::path& storage,
    const fs::path& work, const fs::path& events, const fs::path& records,
    addon::Invocation_context& context)
{
    Row_lookup predecessors(read_predecessors(request, storage, work, context), work / "predecessor-offsets", context);
    Row_sorter membership(work, "session-files", context), planned(work, "planned-files", context);
    for (std::uint64_t index = 0; index < observed.size(); ++index) {
        check_cancel(context);
        const auto row = observed.at(index);
        const auto file = observed_file(row.second);
        if (file.usage) membership.add(record_key(file.usage->record_id), Json(index));
        else planned.add(row.first, row.second);
    }
    Row_lookup members(membership.finish(), work / "member-offsets", context);
    std::ifstream status(work / "fork-status", std::ios::binary);
    std::ofstream output(records, std::ios::binary);
    require_io(status && output);
    const auto proved = [&](std::uint64_t index) {
        status.clear();
        status.seekg(static_cast<std::streamoff>(index));
        const auto value = status.get();
        require_io(value == 2 || value == 3);
        return value == 2;
    };
    for (std::uint64_t first = 0; first < members.size();) {
        check_cancel(context);
        const auto key = members.at(first).first;
        std::uint64_t last = first;
        while (last < members.size() && members.at(last).first == key) ++last;
        const auto predecessor_index = predecessors.find(key);
        const auto predecessor = predecessor_index ?
            addon::decode_usage_owned_event_predecessor(predecessors.at(*predecessor_index).second, nullptr) : std::nullopt;
        std::optional<addon::Usage_record> accepted, removed, growth, ordinary;
        std::set<std::string> original_ids;
        std::size_t owner_bytes = 0;
        std::uint64_t accepted_files = 0, ordinary_files = 0;
        bool establishment = false;
        for (auto position = first; position < last; ++position) {
            check_cancel(context);
            const auto index = count(members.at(position).second);
            const auto row = observed.at(index).second;
            const auto previous = previous_file(row);
            auto plan = prepare_file_adjustment(row, proved(index), request.scan.cache_selection->accepted.has_value(), events, context);
            if (!predecessor && !proved(index)) throw Usage_ownership_unavailable();
            if (previous && previous->usage) accumulate_usage(accepted, previous->usage, context, ++accepted_files);
            accumulate_usage(removed, plan.removed, context);
            accumulate_usage(growth, plan.growth, context);
            if (plan.ordinary.usage) accumulate_usage(ordinary, plan.ordinary.usage, context, ++ordinary_files);
            establishment = establishment || plan.raw_establishment;
            if (plan.removed) {
                auto owners = origins.read(index);
                std::string line;
                for (std::uint64_t occurrence = 0; occurrence < plan.removed->input.coverage->total_records; ++occurrence) {
                    check_cancel(context);
                    require_io(owners.next(line));
                    const auto owner = json_util::parse(line);
                    if (!owner || !owner->is_string() || owner->as_string() == plan.removed->record_id) {
                        throw Usage_ownership_unavailable();
                    }
                    if (original_ids.insert(owner->as_string()).second) owner_bytes += line.size() + 1;
                    if (owner_bytes > addon::limits::k_max_exchange_payload_bytes) throw Usage_ownership_unavailable();
                }
            }
        }
        // An accepted host row without selected per-file accounting first uses
        // ordinary raw admission to establish the observed counted population.
        const bool establish_population = predecessor && !accepted;
        establishment = establishment || establish_population;
        bool correct = false;
        std::optional<addon::Usage_record> candidate;
        if (predecessor && !establishment) {
            if (removed) {
                candidate = adjust_owned_usage(*predecessor, accepted, removed, growth,
                    std::vector<std::string>(original_ids.begin(), original_ids.end()));
                if (candidate &&
                    (candidate->source_path              != predecessor->record.source_path   ||
                     candidate->model                    != predecessor->record.model         ||
                     candidate->counter_field            != predecessor->record.counter_field ||
                     candidate->source_counter_semantics != predecessor->record.source_counter_semantics))
                {
                    // Ordinary admission first binds evolving descriptive metadata;
                    // removal authority remains tied to the accepted provenance.
                    candidate.reset();
                }
                correct = candidate.has_value();
            }
            if (!candidate && growth) {
                candidate = adjust_owned_usage(*predecessor, accepted, std::nullopt, growth, {});
                if (!candidate) throw Usage_ownership_unavailable();
            }
            if (!candidate) candidate = retain_usage_predecessor(*predecessor);
        }
        else if (!establish_population) candidate = ordinary;
        std::optional<addon::Usage_record> established, fresh;
        std::uint64_t established_files = 0, fresh_files = 0;
        for (auto position = first; position < last; ++position) {
            check_cancel(context);
            const auto index = count(members.at(position).second);
            const auto row = observed.at(index);
            auto plan = prepare_file_adjustment(row.second, proved(index),
                request.scan.cache_selection->accepted.has_value(), events, context);
            auto file = correct ? std::move(plan.corrected) : std::move(plan.ordinary);
            if (establish_population) {
                file = observed_file(row.second);
                if (!proved(index) || !file.fork_evidence) throw Usage_ownership_unavailable();
                file.fork_evidence->set("accounting", Json("raw"));
                accumulate_usage(established, file.usage, context, ++established_files);
            }
            else if (!predecessor) {
                retain_owned_usage(file, addon::path_from_utf8(json_util::text(json_util::member(row.second, "file"), "path")), events, context);
                accumulate_usage(fresh, file.usage, context, ++fresh_files);
            }
            const auto path = json_util::text(json_util::member(row.second, "file"), "path");
            planned.add(row.first, observation(path, file));
        }
        if (establish_population) candidate = std::move(established);
        else if (!predecessor) candidate = std::move(fresh);
        if (!candidate) throw Usage_ownership_unavailable();
        const auto encoded = addon::serialize_json(spool_record(*candidate));
        if (!json_util::parse(encoded)) throw Usage_ownership_unavailable();
        write_line(output, encoded);
        first = last;
    }
    output.close();
    require_io(static_cast<bool>(output));
    return planned.finish();
}

Scan_result scan_to_disk(const fs::path& profile, const fs::path& storage,
    std::string_view mode, const std::string& owner,
    addon::Invocation_context& context, const addon::Usage_cache_selection& selection,
    const addon::Usage_owned_event_correction_request* correction = nullptr)
{
    if (!fs::is_directory(profile)) throw std::runtime_error("usage_profile_unavailable");
    const auto sessions_directory = profile / "sessions";
    const bool sessions_present = fs::exists(sessions_directory);
    if (sessions_present && !fs::is_directory(sessions_directory)) throw std::runtime_error("usage_sessions_not_directory");
    if (!storage.is_absolute() || !fs::is_directory(storage)) throw std::runtime_error("usage_storage_unavailable");
    const auto destination = cache_io_path(addon::path_from_utf8(selection.candidate_root) / "files.json");
    const auto input_file = selection.accepted
        ? cache_io_path(addon::path_from_utf8(selection.accepted->root) / "files.json")
        : destination;
    auto workspace = std::make_unique<Scan_workspace>(storage);
    const auto& work = workspace->path();
    Row_sorter cached(work, "cache", context), live(work, "live", context);
    Row_sorter receipts(work, "receipts", context), sessions(work, "sessions", context);
    Row_sorter observations(work, "observations", context);
    const auto events_path = work / "events.jsonl";
    std::ofstream events(events_path, std::ios::binary);
    require_io(static_cast<bool>(events));
    check_cancel(context);
    context.report_progress(0, std::nullopt, "Reading usage cache");
    if (mode != "reset" && fs::exists(input_file)) {
        if (fs::is_symlink(fs::symlink_status(input_file))) throw std::runtime_error("usage_cache_unreadable");
        std::optional<File_record> pending;
        std::string pending_path;
        std::uint64_t event_count = 0;
        addon::Sha256 event_digest;
        const auto finish_pending = [&] {
            if (!pending) return;
            pending->event_end = static_cast<std::uint64_t>(events.tellp());
            if (pending->fork_evidence &&
                (count(json_util::member(*pending->fork_evidence, "event_count")) != event_count ||
                    json_util::text(*pending->fork_evidence, "event_sha256_hex") != addon::to_hex(event_digest.finish())))
            {
                throw Usage_ownership_unavailable();
            }
            if (pending->fork_evidence) {
                pending->accepted_event_count = event_count;
                pending->retained_prefix_matches = true;
            }
            cached.add(path_key(addon::path_from_utf8(pending_path)), observation(pending_path, *pending));
            pending.reset();
        };
        Cache_reader(input_file, context).read(owner, [&](const Json& row) {
            check_cancel(context);
            finish_pending();
            pending = decode_file(row);
            pending_path = json_util::text(row, "path");
            pending->event_begin = static_cast<std::uint64_t>(events.tellp());
            event_count = 0;
            event_digest = addon::Sha256{};
        }, [&](const Json& receipt) {
            if (!receipt.is_string() || !addon::sha256_from_hex(receipt.as_string())) throw std::runtime_error("usage_cache_invalid");
            receipts.add(receipt.as_string(), receipt);
        }, [&](const Json& event) {
            if (!pending || !pending->fork_carrier || !event.is_object() || json_util::text(event, "type") != "usage.record") {
                throw std::runtime_error("usage_cache_invalid");
            }
            const auto canonical = addon::serialize_json_canonical(event);
            write_line(events, canonical);
            event_digest.update(canonical);
            event_digest.update("\n");
            ++event_count;
        });
        finish_pending();
    }
    auto workspaces = optional_document(profile / "workspaces.json");
    if (!json_util::member(workspaces, "version").is_integer() || json_util::member(workspaces, "version").as_integer() != 1) workspaces = Json::object();
    std::uint64_t total = 0;
    const auto entries = sessions_present
        ? fs::recursive_directory_iterator(sessions_directory)
        : fs::recursive_directory_iterator{};
    for (const auto& entry : entries) {
        check_cancel(context);
        if (entry.path().filename() != "wire.jsonl" || !entry.is_regular_file() ||
            entry.path().parent_path().parent_path().filename() != "agents") continue;
        const auto path = fs::absolute(entry.path()).lexically_normal();
        live.add(path_key(path), json_util::object({{"path", Json(path_string(path))}}));
        ++total;
        if (total % 256 == 0) context.report_progress(0, std::nullopt, "Discovering usage logs");
    }
    context.report_progress(0, total, "Sorting usage logs");
    Row_stream cached_rows(cached.finish()), live_rows(live.finish());
    const auto staged_cache = work / "files.jsonl";
    std::ofstream cache_output(staged_cache, std::ios::binary);
    require_io(static_cast<bool>(cache_output));
    write_line(cache_output, addon::serialize_json(json_util::object({{"version", Json(1)}, {"profile", Json(owner)}, {"format", Json("jsonl")}})));
    {
        Row_stream receipt_rows(receipts.finish());
        std::string previous;
        while (receipt_rows.available()) {
            check_cancel(context);
            if (previous == receipt_rows.key()) throw std::runtime_error("usage_cache_invalid");
            previous = receipt_rows.key();
            write_line(cache_output, addon::serialize_json(json_util::object({{"receipt", receipt_rows.value()}})));
            receipt_rows.next();
        }
    }
    std::uint64_t processed = 0;
    while (cached_rows.available() || live_rows.available()) {
        check_cancel(context);
        std::string key;
        for (const auto* rows : {&cached_rows, &live_rows}) {
            if (rows->available() && (key.empty() || rows->key() < key)) key = rows->key();
        }
        std::optional<File_record> file;
        std::string path;
        if (cached_rows.available() && cached_rows.key() == key) {
            const auto row = cached_rows.value();
            path = json_util::text(json_util::member(row, "file"), "path");
            file = observed_file(row);
            if (file->usage && file->usage->temporal && selection.accepted) {
                file->usage->temporal->retained_from = addon::Usage_temporal_retained_from{
                    selection.accepted->snapshot_id, selection.accepted->manifest_sha256_hex};
            }
            cached_rows.next();
            if (cached_rows.available() && cached_rows.key() == key) throw std::runtime_error("usage_cache_duplicate");
        }
        const auto previous = file;
        if (live_rows.available() && live_rows.key() == key) {
            path = json_util::text(live_rows.value(), "path");
            live_rows.next();
            ++processed;
            // Different spellings of one platform path identify one log.
            while (live_rows.available() && live_rows.key() == key) {
                path = std::min(path, json_util::text(live_rows.value(), "path"));
                live_rows.next();
                ++processed;
            }
            file = refresh_file(addon::path_from_utf8(path), file, mode, workspaces,
                work / "live-events.jsonl", events, context);
            context.report_progress(processed, total, {});
        }
        if (!file) continue;
        observations.add(key, observation(path, *file, previous));
    }
    events.close();
    require_io(static_cast<bool>(events));
    Row_lookup observed(observations.finish(), work / "observation-offsets", context);
    Ownership_origins origins(work, observed.size(), context);
    validate_fork_chains(observed, events_path, work, origins, context);
    const auto records = work / "records.jsonl";
    std::unique_ptr<Row_lookup> corrected_files;
    if (correction) {
        corrected_files = std::make_unique<Row_lookup>(prepare_corrected_sessions(observed, origins,
            *correction, storage, work, events_path, records, context), work / "planned-offsets", context);
    }
    auto& final_files = corrected_files ? *corrected_files : observed;
    std::ifstream chain_status(work / "fork-status", std::ios::binary);
    require_io(static_cast<bool>(chain_status));
    for (std::uint64_t index = 0; index < final_files.size(); ++index) {
        check_cancel(context);
        const auto status = chain_status.get();
        require_io(status == 2 || status == 3);
        const auto row = final_files.at(index).second;
        const auto path = json_util::text(json_util::member(row, "file"), "path");
        auto file = observed_file(row);
        if (!correction) file = prepare_file_adjustment(row, status == 2, false, events_path, context).ordinary;
        write_line(cache_output, addon::serialize_json(encode_file(path, file)));
        Event_stream retained(events_path, file.event_begin, file.event_end);
        std::string event;
        while (retained.next(event)) {
            check_cancel(context);
            if (event.size() + 15 > addon::Json_parse_limits{}.max_bytes) throw Usage_ownership_unavailable();
            write_line(cache_output, "{\"fork_event\":" + event + '}');
        }
        if (file.usage && !correction) {
            // Hex-encoded UTF-8 preserves the record-ID ordering without
            // allowing an ID to introduce the spool's tab delimiter.
            const auto& id = file.usage->record_id;
            const auto sort_key = addon::to_hex(reinterpret_cast<const std::uint8_t*>(id.data()), id.size());
            sessions.add(sort_key, spool_record(*file.usage));
        }
    }
    cache_output.close();
    require_io(static_cast<bool>(cache_output));
    if (correction) {
        check_cancel(context);
        store_cache(destination, staged_cache);
        return {std::move(workspace), records};
    }
    std::ofstream output(records, std::ios::binary);
    require_io(static_cast<bool>(output));
    Row_stream session_rows(sessions.finish());
    while (session_rows.available()) {
        check_cancel(context);
        const auto key = session_rows.key();
        addon::Contract_failure failure;
        auto record = spool_record(session_rows.value());
        if (!record) throw std::runtime_error("usage_result_unrepresentable");
        session_rows.next();
        std::uint64_t session_files = 1;
        bool mixed_coverage = false;
        while (session_rows.available() && session_rows.key() == key) {
            check_cancel(context);
            const auto incoming = spool_record(session_rows.value());
            if (!incoming) throw std::runtime_error("usage_result_unrepresentable");
            ++session_files;
            merge_usage(*record, *incoming, mixed_coverage, context);
            session_rows.next();
        }
        if (mixed_coverage) {
            record->coverage_unit = addon::Usage_coverage_unit::SESSIONS;
            for (auto* field : amounts(*record)) {
                field->applicable = true;
                field->value = 0;
                field->coverage = addon::Usage_field_coverage{0, session_files, false, addon::Usage_support::UNKNOWN};
            }
        }
        if (!addon::decode_stored_usage_accounting(addon::encode_stored_usage_accounting(*record), &failure)) throw std::runtime_error("usage_result_unrepresentable");
        write_line(output, addon::serialize_json(spool_record(*record)));
    }
    output.close();
    require_io(static_cast<bool>(output));
    check_cancel(context);
    store_cache(destination, staged_cache);
    return {std::move(workspace), records};
}

} // namespace

std::vector<addon::Usage_record> scan_usage(const fs::path& profile, const fs::path& storage,
    std::string_view mode,
    addon::Invocation_context& context, const addon::Usage_cache_selection& selection)
{
    const auto scan = scan_to_disk(profile, storage, mode, account_key(profile), context, selection);
    std::ifstream input(scan.records, std::ios::binary);
    require_io(static_cast<bool>(input));
    std::vector<addon::Usage_record> result;
    std::string line;
    while (read_line(input, line)) {
        check_cancel(context);
        const auto value = json_util::parse(line);
        const auto record = value ? spool_record(*value) : std::nullopt;
        if (!record) throw std::runtime_error("usage_result_unrepresentable");
        result.push_back(*record);
    }
    return result;
}

void handle_usage(addon::Invocation_context& context)
{
    if (context.request().operation != "scan") { context.reject_unknown_operation(); return; }
    addon::Contract_failure failure;
    const bool owned_operation = context.request().capability.capability_id == addon::k_usage_owned_event_correction_capability_id;
    const auto correction = owned_operation ?
        addon::decode_usage_owned_event_correction_request(context.request().payload, &failure) : std::nullopt;
    const auto input = owned_operation ? (correction ? std::optional<addon::Usage_scan_request>(correction->scan) : std::nullopt) :
        addon::decode_usage_scan_request(context.request().payload, &failure, 3);
    if (!input) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    if (!input->context.profile_storage) throw std::runtime_error("usage_storage_unavailable");
    const auto profile = profile_for_context(input->context);
    const auto storage = addon::path_from_utf8(input->context.profile_storage->root);
    auto identity = *input;
    identity.snapshot_id.reset();
    identity.cursor.reset();
    auto binding = addon::encode_usage_scan_request(identity, 3);
    if (correction) {
        auto request = *correction;
        request.scan = identity;
        binding = json_util::object({{"capability", Json(addon::k_usage_owned_event_correction_capability_id)},
            {"operation", Json(context.request().operation)},
            {"request", addon::encode_usage_owned_event_correction_request(request)}});
    }
    const auto owner = addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(binding)));
    const auto& selection = *input->cache_selection;
    if (!addon::validate_usage_cache_root(selection.candidate_root, path_string(storage), &failure)) {
        throw std::runtime_error(failure.reason_code);
    }
    const auto outside_workspace = [&](const std::string& selected) {
        const auto relative = addon::path_from_utf8(selected).lexically_relative(storage);
        auto name = path_string(*relative.begin());
#ifdef _WIN32
        name = addon::ascii_lowercase(name);
#endif
        return !(name.starts_with("usage-") && name.ends_with(".tmp"));
    };
    if (!outside_workspace(selection.candidate_root) ||
        (selection.accepted && !outside_workspace(selection.accepted->root)))
    {
        throw std::runtime_error("usage_cache_namespace_conflict");
    }
    std::unique_lock lock(s_mutex, std::defer_lock);
    while (!lock.try_lock()) {
        // The SDK wakes this request on cancellation; mutex acquisition alone cannot.
        if (context.wait_for_cancel(50)) throw std::runtime_error("cancelled");
    }
    check_cancel(context);
    std::erase_if(s_snapshots, [](const auto& entry) { return now_ms() - entry.second.touched_ms > 600000; });
    std::size_t begin = 0;
    std::string snapshot_id;
    if (!input->snapshot_id) {
        if (s_snapshots.size() >= 8) throw std::runtime_error("usage_snapshot_capacity");
        Snapshot snapshot;
        snapshot.owner = owner;
        const auto& selection = *input->cache_selection;
        if (!addon::validate_usage_cache_allocation(selection.candidate_root, path_string(storage), &failure)) {
            throw std::runtime_error(failure.reason_code);
        }
        if (selection.accepted) {
            const auto manifest = addon::validate_usage_cache_candidate(
                *selection.accepted, path_string(storage), input->context.account_context,
                [&context] { return context.cancel_requested(); }, &failure);
            if (!manifest) throw std::runtime_error(failure.reason_code);
            if (manifest->format != "kimi-wire-cache@2" ||
                manifest->files.size() != 1 ||
                manifest->files.front().path != "files.json")
            {
                throw std::runtime_error("usage_cache_format_unsupported");
            }
        }
        // Keep a path-verified historical grant bound to its existing cache and counters.
        const auto cache_owner = input->context.account_context
            ? input->context.account_context->adapter_account_key : account_key(profile);
        auto scan = scan_to_disk(profile, storage, input->mode, cache_owner, context,
            *input->cache_selection, correction ? &*correction : nullptr);
        snapshot.workspace = std::move(scan.workspace);
        snapshot.records = std::move(scan.records);
        snapshot.bytes = fs::file_size(snapshot.records);
        snapshot.scope.observed_at_ms = now_ms();
        snapshot.scope.fresh_for_ms = 60000;
        if (input->context.account_context) snapshot.scope.source_account_key = input->context.account_context->adapter_account_key;
        snapshot.touched_ms = now_ms();
        snapshot_id = addon::random_nonce_hex();
        snapshot.cache_candidate = addon::seal_usage_cache_candidate(
            input->cache_selection->candidate_root, path_string(storage), input->context.account_context,
            snapshot_id, "kimi-wire-cache@2", [&context] { return context.cancel_requested(); }, &failure);
        if (!snapshot.cache_candidate) throw std::runtime_error(failure.reason_code);
        s_snapshots.emplace(snapshot_id, std::move(snapshot));
    }
    else {
        snapshot_id = *input->snapshot_id;
        if (!input->cursor) throw std::runtime_error("usage_cursor_invalid");
        const auto& cursor = *input->cursor;
        const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), begin);
        if (parsed.ec != std::errc{} || parsed.ptr != cursor.data() + cursor.size() || !begin || std::to_string(begin) != cursor) throw std::runtime_error("usage_cursor_invalid");
    }
    const auto found = s_snapshots.find(snapshot_id);
    if (found == s_snapshots.end() || found->second.owner != owner || begin > found->second.bytes) throw std::runtime_error("usage_snapshot_unavailable");
    auto& snapshot = found->second;
    snapshot.touched_ms = now_ms();
    addon::Usage_scan_page page;
    page.incomplete_historical_coverage = true;
    page.scope = snapshot.scope;
    page.snapshot_id = snapshot_id;
    page.cache_candidate = snapshot.cache_candidate;
    page.account_context = input->context.account_context;
    page.profile_storage = input->context.profile_storage;
    std::ifstream records(snapshot.records, std::ios::binary);
    require_io(static_cast<bool>(records));
    if (begin) {
        records.seekg(static_cast<std::streamoff>(begin - 1));
        if (records.get() != '\n') throw std::runtime_error("usage_cursor_invalid");
    }
    else records.seekg(0);
    std::uint64_t end = begin;
    std::size_t encoded_records_bytes = 0;
    const auto append = [&](const addon::Usage_record& record) {
        constexpr auto reserved_bytes = addon::limits::k_max_exchange_payload_bytes -
            (addon::limits::k_max_frame_bytes - 65536);
        return addon::append_usage_page_record(page, record, encoded_records_bytes, 3, reserved_bytes);
    };
    std::string line;
    while (page.records.size() < addon::k_max_usage_page_records && read_line(records, line)) {
        check_cancel(context);
        const auto value = json_util::parse(line);
        auto record = value ? spool_record(*value) : std::nullopt;
        if (!record) throw std::runtime_error("usage_result_unrepresentable");
        const auto* envelope = record->capabilities.find(addon::k_usage_owned_event_correction_capability_id);
        const bool retaining = correction && envelope && json_util::text(*envelope, "disposition") == "retain";
        const bool correcting = correction && envelope && json_util::text(*envelope, "disposition") == "correct";
        const bool opaque_temporal = !record->temporal && record->capabilities.find(addon::k_usage_temporal_capability_id);
        if (!correction) record->capabilities.erase(addon::k_usage_owned_event_correction_capability_id);
        // A surviving wire log can be only a subset of a session's events.
        if (!retaining) {
            record->aggregate_evidence = "partial";
            if (!opaque_temporal) {
                auto evidence = record->temporal.value_or(unavailable_temporal(*record, "missing-evidence"));
                evidence.parent = {record->record_id, record->total_tokens, snapshot_id, snapshot.scope.source_account_key};
                if (correcting) {
                    record->temporal = std::move(evidence);
                    const auto encoded = addon::encode_usage_record(*record, 3);
                    if (!addon::decode_usage_record(encoded, nullptr, 3) ||
                        !addon::parse_json(addon::serialize_json(encoded), {}, nullptr)) throw Usage_ownership_unavailable();
                }
                else publish_temporal(*record, std::move(evidence));
            }
        }
        const auto next = static_cast<std::uint64_t>(records.tellg());
        page.next_cursor = std::to_string(next);
        if (!append(*record)) {
            if (!page.records.empty()) break;
            if (correcting || retaining) throw Usage_ownership_unavailable();
            if (record->temporal) {
                auto evidence = *record->temporal;
                evidence.portions.clear();
                evidence.unavailable_tokens = record->total_tokens;
                evidence.coverage_reason = "capacity";
                publish_temporal(*record, std::move(evidence));
                if (append(*record)) { end = next; continue; }
                record->temporal.reset();
            }
            if (!append(*record)) throw std::runtime_error("usage_record_frame_capacity");
        }
        end = next;
    }
    records.close();
    page.complete = end == snapshot.bytes;
    if (!page.complete) page.next_cursor = std::to_string(end);
    else { page.next_cursor.reset(); s_snapshots.erase(found); }
    check_cancel(context);
    context.succeed(addon::encode_usage_scan_result(page, 3));
}

std::optional<addon::Usage_session_fact> describe_usage_session(const addon::Usage_session_identity& identity)
{
    if (!identity.session_key || identity.record_id != *identity.session_key ||
        canonical_usage_session_id(*identity.session_key) != *identity.session_key)
    {
        return std::nullopt;
    }
    return addon::Usage_session_fact{identity.session_key->substr(5), false};
}

void handle_usage_session(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_session_describe_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_session_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    std::vector<addon::Usage_session_description> result;
    for (const auto& identity : *request) {
        result.push_back({identity, describe_usage_session(identity)});
    }
    context.succeed(addon::encode_usage_session_result(result));
}

} // namespace kimi
