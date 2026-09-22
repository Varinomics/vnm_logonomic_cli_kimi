#include "kimi_usage.h"
#include "kimi_accounts.h"
#include "kimi_json.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/package_source.h>

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
#include <stdexcept>
#include <utility>

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
constexpr std::uint64_t k_max_exact = 9007199254740991;
// Bounds each document read into memory whole: the package cache and a native
// seed. Wire logs are read one line at a time, each line within the parser's
// document bound, so neither one log nor the history is bounded.
constexpr std::uint64_t k_max_bytes = 512 * 1024 * 1024;
constexpr std::size_t k_read_bytes = 1024 * 1024;
constexpr std::size_t k_max_files = 100000;

struct File_record
{
    std::uint64_t size = 0;
    std::int64_t modified_ms = 0;
    std::string cwd;
    bool stale_breakdown = false;
    std::optional<addon::Usage_record> usage;
};

struct Snapshot
{
    std::string owner;
    addon::Observation_scope scope;
    std::vector<addon::Usage_record> records;
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
    if (const auto root = repository(cwd)) allocation.allocations.push_back({*root, record.total_tokens});
    else allocation.unattributed_tokens = record.total_tokens;
    record.repository_usage = std::move(allocation);
}

// Passes each complete line of a wire log to `consume`, buffering at most one
// line of the parser's document bound; a final unterminated line is still being
// written and is skipped. False when the log was unreadable or changed while it
// was read, in which case the caller keeps the log's previous evidence.
bool read_stable_lines(const fs::path& path, File_record& fingerprint, addon::Invocation_context& context,
    const std::function<void(std::string_view line)>& consume)
{
    std::error_code error;
    const auto before_size = fs::file_size(path, error);
    if (error) return false;
    const auto before_time = fs::last_write_time(path, error);
    if (error) return false;
    // A failed line blames the log only if the log was at rest.
    fingerprint.size = before_size;
    fingerprint.modified_ms = file_modified_ms(before_time);
    const auto unchanged = [&] {
        std::error_code stat_error;
        const auto size = fs::file_size(path, stat_error);
        if (stat_error || size != before_size) return false;
        const auto time = fs::last_write_time(path, stat_error);
        return !stat_error && time == before_time;
    };
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    const auto line_limit = static_cast<std::size_t>(addon::Json_parse_limits{}.max_bytes);
    std::string buffer;
    std::size_t begin = 0;
    std::uint64_t consumed = 0;
    try {
        for (;;) {
            const auto newline = buffer.find('\n', begin);
            if (newline != std::string::npos) {
                consume(std::string_view(buffer).substr(begin, newline - begin));
                consumed += newline + 1 - begin;
                begin = newline + 1;
                continue;
            }
            if (buffer.size() - begin > line_limit) throw std::runtime_error("usage_record_invalid");
            buffer.erase(0, begin);
            begin = 0;
            check_cancel(context);
            const auto kept = buffer.size();
            buffer.resize(kept + k_read_bytes);
            stream.read(buffer.data() + kept, static_cast<std::streamsize>(k_read_bytes));
            const auto received = static_cast<std::size_t>(stream.gcount());
            buffer.resize(kept + received);
            if (received == 0) break;
        }
    }
    catch (...) {
        if (!unchanged()) return false;
        throw;
    }
    if (stream.bad() || consumed + buffer.size() != before_size || !unchanged()) return false;
    return true;
}

Json optional_document(const fs::path& path)
{
    std::error_code error;
    if (fs::file_size(path, error) > 2 * 1024 * 1024 || error) return Json::object();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Json::object();
    std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    const auto result = parse(bytes);
    return result && result->is_object() ? *result : Json::object();
}

std::string recorded_cwd(const fs::path& session, const Json& workspaces)
{
    const auto state = text(optional_document(session / "state.json"), "cwd");
    const auto mapped = text(member(member(workspaces, "workspaces"), path_string(session.parent_path().filename())), "root");
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
        const auto event = parse(line);
        if (!event || !event->is_object()) throw std::runtime_error("usage_record_invalid");
        if (text(*event, "type") != "usage.record") return;
        const auto date = local_date(count(member(*event, "time")));
        const auto& usage = member(*event, "usage");
        if (!usage.is_object()) throw std::runtime_error("usage_record_invalid");
        bool recognized = false;
        constexpr std::array<std::string_view, 4> names{"inputOther", "output", "inputCacheRead", "inputCacheCreation"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (const auto* value = usage.find(names[index])) {
                const auto tokens = count(*value);
                add(m_record.total_tokens, tokens);
                add(m_values[index], tokens);
                ++m_coverage[index];
                recognized = true;
            }
        }
        if (!recognized) throw std::runtime_error("usage_counters_missing");
        const auto model = text(*event, "model");
        if (!m_record.model || m_record.model->empty()) m_record.model = model;
        else if (!model.empty() && *m_record.model != model) m_record.model = "multiple";
        m_record.completed_date = std::max(m_record.completed_date, date);
        ++m_records;
    }

    std::optional<addon::Usage_record> finish(std::string_view cwd)
    {
        if (!m_records) return std::nullopt;
        auto fields = amounts(m_record);
        for (std::size_t index = 0; index < fields.size(); ++index) {
            fields[index]->applicable = true;
            fields[index]->value = m_values[index];
            fields[index]->coverage = addon::Usage_field_coverage{m_coverage[index], m_records, false,
                m_coverage[index] ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN};
        }
        attribute(m_record, cwd);
        return std::move(m_record);
    }

private:
    addon::Usage_record          m_record;
    std::array<std::uint64_t, 4> m_values{};
    std::array<std::uint64_t, 4> m_coverage{};
    std::uint64_t                m_records = 0;
};

Json encode_cache(const std::map<std::string, File_record>& files, std::string_view profile, const std::set<std::string>& receipts)
{
    auto rows = Json::array();
    for (const auto& [path, file] : files) {
        rows.push_back(object({{"path", Json(path)}, {"size", Json(file.size)},
            {"modified_msecs", Json(file.modified_ms)}, {"recorded_cwd", Json(file.cwd)},
            {"stale_breakdown", Json(file.stale_breakdown)},
            {"usage", file.usage ? addon::encode_usage_record(*file.usage, 2) : Json{}}}));
    }
    auto imported = Json::array();
    for (const auto& receipt : receipts) imported.push_back(Json(receipt));
    return object({{"version", Json(1)}, {"profile", Json(profile)}, {"files", std::move(rows)}, {"receipts", std::move(imported)}});
}

std::map<std::string, File_record> decode_cache(const Json& value, std::string_view profile)
{
    if (!exact_keys(value, {"version", "profile", "files", "receipts"}) || count(member(value, "version")) != 1 ||
        text(value, "profile") != profile || !member(value, "files").is_array() ||
        member(value, "files").as_array().size() > k_max_files)
    {
        throw std::runtime_error("usage_cache_invalid");
    }
    std::map<std::string, File_record> files;
    for (const auto& row : member(value, "files").as_array()) {
        File_record file;
        const auto path = text(row, "path");
        if (!exact_keys(row, {"path", "size", "modified_msecs", "recorded_cwd", "usage", "stale_breakdown"}) ||
            !addon::path_from_utf8(path).is_absolute() || !member(row, "modified_msecs").is_integer())
        {
            throw std::runtime_error("usage_cache_invalid");
        }
        if (!member(row, "stale_breakdown").is_boolean()) throw std::runtime_error("usage_cache_invalid");
        file.stale_breakdown = member(row, "stale_breakdown").as_boolean();
        file.size = count(member(row, "size"));
        file.modified_ms = member(row, "modified_msecs").as_integer();
        file.cwd = text(row, "recorded_cwd");
        if (!member(row, "usage").is_null()) {
            addon::Contract_failure failure;
            file.usage = addon::decode_usage_record(member(row, "usage"), &failure, 2);
            if (!file.usage) throw std::runtime_error("usage_cache_record_invalid");
        }
        if (!files.emplace(path, std::move(file)).second) throw std::runtime_error("usage_cache_duplicate");
    }
    return files;
}

File_record native_file_record(const Json& row)
{
    File_record file;
    file.size = count(member(row, "size"));
    if (!member(row, "modified_msecs").is_integer()) throw std::runtime_error("usage_native_cache_invalid");
    file.modified_ms = member(row, "modified_msecs").as_integer();
    const auto& records = member(row, "records");
    if (!records.is_array() || records.as_array().size() > 1) throw std::runtime_error("usage_native_cache_invalid");
    const auto& marker = member(member(row, "capabilities"), "token_breakdown_cache");
    file.stale_breakdown = !member(marker, "version").is_integer() ||
        member(marker, "version").as_integer() != 2 ||
        !member(marker, "parser_revision").is_integer() || member(marker, "parser_revision").as_integer() != 2;
    if (records.as_array().empty()) return file;
    const auto& source = records.as_array().front();
    addon::Usage_record record;
    auto native_session = text(source, "session_id");
    if (const auto* identity = member(source, "capabilities").find("usage-record-identity")) {
        // The host rewrites the outer session_id to the mapped record's hex
        // identity. Its preserved native key still names the owning directory.
        if (!identity->is_object() || !member(*identity, "version").is_integer() ||
            member(*identity, "version").as_integer() != 1 ||
            !bounded_string(member(*identity, "legacy_session_key"), 4096))
        {
            throw std::runtime_error("usage_native_session_invalid");
        }
        native_session = text(*identity, "legacy_session_key");
    }
    record.record_id = canonical_usage_session_id(native_session);
    if (record.record_id.empty()) throw std::runtime_error("usage_native_session_invalid");
    record.session_key = record.record_id;
    record.completed_date = text(source, "completed_date");
    record.total_tokens = count(member(source, "total_tokens"));
    record.source_path = text(source, "source_path");
    record.model = text(source, "model");
    record.counter_field = text(source, "counter_field");
    record.source_counter_semantics = text(source, "counter_semantics");
    file.cwd = text(source, "recorded_cwd");
    auto fields = amounts(record);
    const auto& breakdown = member(member(source, "capabilities"), "token_breakdown");
    constexpr std::array<std::string_view, 4> names{"input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"};
    const bool current = !file.stale_breakdown && member(breakdown, "version").is_integer() &&
        member(breakdown, "version").as_integer() == 2;
    record.coverage_unit = current && text(breakdown, "coverage_unit") == "records"
        ? addon::Usage_coverage_unit::RECORDS : addon::Usage_coverage_unit::SESSIONS;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        auto& destination = *fields[index];
        destination.applicable = true;
        destination.value = 0;
        destination.coverage = addon::Usage_field_coverage{0, 1, false, addon::Usage_support::UNKNOWN};
        if (!current) continue;
        const auto& value = member(breakdown, names[index]);
        auto& coverage = *destination.coverage;
        coverage.covered_records = count(member(value, "covered_records"));
        coverage.total_records = count(member(value, "total_records"));
        if (const auto* overflow = value.find("overflow")) {
            if (!overflow->is_boolean()) throw std::runtime_error("usage_native_breakdown_invalid");
            coverage.overflow = overflow->as_boolean();
        }
        const auto support = text(value, "support");
        if (support == "supported") coverage.support = addon::Usage_support::SUPPORTED;
        else if (support == "unsupported") {
            coverage.support = addon::Usage_support::UNSUPPORTED;
            destination.applicable = false;
            destination.value.reset();
        }
        else if (support != "unknown") throw std::runtime_error("usage_native_breakdown_invalid");
        if (coverage.overflow) destination.value.reset();
        if (const auto* known = value.find("known_tokens")) destination.value = count(*known);
    }
    attribute(record, file.cwd);
    addon::Contract_failure failure;
    if (!addon::decode_usage_record(addon::encode_usage_record(record, 2), &failure, 2)) throw std::runtime_error("usage_native_record_unrepresentable");
    file.usage = std::move(record);
    return file;
}

void import_native_cache(std::map<std::string, File_record>& files,
    std::set<std::string>& receipts, const addon::Usage_native_file_cache& seed,
    const fs::path& storage, std::string_view owner, addon::Invocation_context& context)
{
    if (seed.source_account_key != owner) throw std::runtime_error("usage_native_account_mismatch");
    const auto path = addon::path_from_utf8(seed.path);
    if (!path.is_absolute() || path.parent_path() != storage) throw std::runtime_error("usage_native_cache_path_invalid");
    addon::Directory_package_source source(storage);
    addon::Source_error error;
    const auto bytes = source.read(path_string(path.filename()), k_max_bytes, &error);
    if (!bytes || addon::to_hex(addon::Sha256::digest(*bytes)) != seed.sha256_hex) throw std::runtime_error("usage_native_cache_digest_mismatch");
    if (seed.cache_required && bytes->empty() && files.empty()) throw std::runtime_error("usage_retained_cache_missing");
    if (receipts.contains(seed.sha256_hex)) return;
    if (receipts.size() >= 1024) throw std::runtime_error("usage_native_receipt_capacity");
    std::map<std::string, File_record> converted;
    std::size_t begin = 0;
    while (begin < bytes->size()) {
        check_cancel(context);
        const auto end = bytes->find('\n', begin);
        if (end == std::string::npos) throw std::runtime_error("usage_native_cache_truncated");
        const auto row = parse(std::string_view(*bytes).substr(begin, end - begin));
        begin = end + 1;
        if (!row || !row->is_object()) throw std::runtime_error("usage_native_cache_invalid");
        const auto path_text = text(*row, "path");
        const auto original = addon::path_from_utf8(path_text);
        if (!original.is_absolute() || original.filename() != "wire.jsonl" ||
            original.parent_path().parent_path().filename() != "agents") throw std::runtime_error("usage_native_file_invalid");
        auto file = native_file_record(*row);
        if (file.usage && (!file.usage->source_path ||
                path_key(addon::path_from_utf8(*file.usage->source_path)) != path_key(original.parent_path().parent_path().parent_path()) ||
                file.usage->record_id != "kimi:" + path_string(original.parent_path().parent_path().parent_path().filename())))
        {
            throw std::runtime_error("usage_native_session_path_conflict");
        }
        if (!converted.emplace(path_text, std::move(file)).second) throw std::runtime_error("usage_native_cache_duplicate");
        if (converted.size() > k_max_files) throw std::runtime_error("usage_native_cache_capacity");
    }
    // This is a pure seed conversion until every row has validated. Existing
    // package evidence wins; reoffering a seed cannot resurrect old counters.
    std::set<std::string> existing;
    for (const auto& [path, file] : files) { (void)file; existing.insert(path_key(addon::path_from_utf8(path))); }
    for (auto& [path, file] : converted) {
        if (existing.insert(path_key(addon::path_from_utf8(path))).second) files.emplace(path, std::move(file));
    }
    receipts.insert(seed.sha256_hex);
}

void store_cache(const fs::path& destination, const Json& value)
{
    const auto bytes = addon::serialize_json(value);
    if (bytes.size() > k_max_bytes) throw std::runtime_error("usage_cache_capacity");
    const auto temporary = destination.parent_path() / ("usage-" + addon::random_nonce_hex() + ".tmp");
#ifdef _WIN32
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("usage_cache_write_failed");
    DWORD written = 0;
    const bool saved = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    const bool replaced = saved && MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) throw std::runtime_error("usage_cache_write_failed");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
    const bool saved = offset == bytes.size() && fsync(descriptor) == 0;
    close(descriptor);
    bool replaced = saved && rename(temporary.c_str(), destination.c_str()) == 0;
    if (replaced) {
        const int directory = open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        replaced = directory >= 0 && fsync(directory) == 0;
        if (directory >= 0) close(directory);
    }
#endif
    if (!replaced) {
        std::error_code error;
        fs::remove(temporary, error);
        throw std::runtime_error("usage_cache_write_failed");
    }
}

} // namespace

std::vector<addon::Usage_record> scan_usage(const fs::path& profile, const fs::path& storage,
    std::string_view mode, const std::optional<addon::Usage_native_file_cache>& seed,
    addon::Invocation_context& context, const addon::Usage_cache_selection* selection)
{
    if (!fs::is_directory(profile)) throw std::runtime_error("usage_profile_unavailable");
    const auto sessions_directory = profile / "sessions";
    const bool sessions_present = fs::exists(sessions_directory);
    if (sessions_present && !fs::is_directory(sessions_directory)) throw std::runtime_error("usage_sessions_not_directory");
    if (!storage.is_absolute() || !fs::is_directory(storage)) throw std::runtime_error("usage_storage_unavailable");
    const auto owner = account_key(profile);
    const auto destination = cache_io_path(selection
        ? addon::path_from_utf8(selection->candidate_root) / "files.json"
        : storage / (owner + "-usage-v1.json"));
    const auto input_file = selection && selection->accepted
        ? cache_io_path(addon::path_from_utf8(selection->accepted->root) / "files.json")
        : destination;
    std::map<std::string, File_record> files;
    std::set<std::string> receipts;
    if (mode != "reset" && fs::exists(input_file)) {
        addon::Directory_package_source source(input_file.parent_path());
        addon::Source_error source_error;
        const auto bytes = source.read(path_string(input_file.filename()), k_max_bytes, &source_error);
        addon::Json_parse_limits limits;
        limits.max_bytes = k_max_bytes;
        const auto cache = bytes ? addon::parse_json(*bytes, limits, nullptr) : std::nullopt;
        if (!cache) throw std::runtime_error("usage_cache_unreadable");
        files = decode_cache(*cache, owner);
        const auto& previous = member(*cache, "receipts");
        if (!previous.is_array() || previous.as_array().size() > 1024) throw std::runtime_error("usage_cache_invalid");
        for (const auto& receipt : previous.as_array()) {
            if (!receipt.is_string() || receipt.as_string().size() != 64 || !receipts.insert(receipt.as_string()).second) throw std::runtime_error("usage_cache_invalid");
        }
    }
    if (mode != "reset" && seed) import_native_cache(files, receipts, *seed, storage, owner, context);
    std::map<std::string, std::string> cached_paths;
    for (const auto& [path, file] : files) { (void)file; cached_paths[path_key(addon::path_from_utf8(path))] = path; }
    auto workspaces = optional_document(profile / "workspaces.json");
    if (!member(workspaces, "version").is_integer() || member(workspaces, "version").as_integer() != 1) workspaces = Json::object();
    std::map<std::string, fs::path> live;
    const auto entries = sessions_present
        ? fs::recursive_directory_iterator(sessions_directory)
        : fs::recursive_directory_iterator{};
    for (const auto& entry : entries) {
        check_cancel(context);
        if (entry.path().filename() != "wire.jsonl" || !entry.is_regular_file() ||
            entry.path().parent_path().parent_path().filename() != "agents") continue;
        const auto path = fs::absolute(entry.path()).lexically_normal();
        const auto key = path_key(path);
        const auto previous = live.find(key);
        if (previous == live.end() || path_string(path) < path_string(previous->second)) live[key] = path;
        if (live.size() > k_max_files) throw std::runtime_error("usage_file_capacity");
    }
    std::uint64_t processed = 0;
    for (const auto& [key, path] : live) {
        (void)key;
        check_cancel(context);
        const auto path_text = path_string(path);
        const auto session = path.parent_path().parent_path().parent_path();
        const auto cwd = recorded_cwd(session, workspaces);
        const auto previous = cached_paths.find(key);
        auto found = previous == cached_paths.end() ? files.end() : files.find(previous->second);
        File_record current;
        current.cwd = cwd;
        if (mode == "incremental" && found != files.end() && !found->second.stale_breakdown) {
            std::error_code error;
            const auto size = fs::file_size(path, error);
            if (!error) {
                const auto modified = fs::last_write_time(path, error);
                if (!error && found->second.size == size && found->second.modified_ms == file_modified_ms(modified)) {
                    current = found->second;
                    current.cwd = cwd;
                    if (current.usage) attribute(*current.usage, cwd);
                    files.erase(found);
                    files[path_text] = std::move(current);
                    context.report_progress(++processed, live.size(), {});
                    continue;
                }
            }
        }
        std::optional<addon::Usage_record> parsed;
        bool stable = false;
        try {
            Wire_usage wire(session);
            stable = read_stable_lines(path, current, context, [&](std::string_view line) { wire.consume(line, context); });
            if (stable) parsed = wire.finish(cwd);
        }
        catch (...) {
            check_cancel(context);
            if (found == files.end() || !found->second.stale_breakdown ||
                found->second.size != current.size || found->second.modified_ms != current.modified_ms) throw;
            parsed = found->second.usage;
            current.stale_breakdown = true;
            stable = true;
        }
        if (stable) {
            if (!current.stale_breakdown && mode == "incremental" && found != files.end() && !found->second.stale_breakdown &&
                found->second.size == current.size && found->second.modified_ms == current.modified_ms)
            {
                parsed = found->second.usage;
            }
            current.usage = std::move(parsed);
            if (current.usage) attribute(*current.usage, cwd);
            if (found != files.end()) files.erase(found);
            files[path_text] = std::move(current);
        }
        else if (found != files.end()) {
            found->second.cwd = cwd;
            if (found->second.usage) attribute(*found->second.usage, cwd);
        }
        context.report_progress(++processed, live.size(), {});
    }
    std::map<std::string, addon::Usage_record> sessions;
    std::map<std::string, std::uint64_t> session_files;
    std::set<std::string> mixed_coverage;
    for (auto& [path, file] : files) {
        (void)path;
        if (!file.usage) continue;
        attribute(*file.usage, file.cwd);
        const auto& record = *file.usage;
        ++session_files[record.record_id];
        const auto [found, inserted] = sessions.emplace(record.record_id, record);
        if (inserted) continue;
        auto& accumulated = found->second;
        if (!record.source_path || !accumulated.source_path ||
            path_key(addon::path_from_utf8(*record.source_path)) != path_key(addon::path_from_utf8(*accumulated.source_path)))
        {
            throw std::runtime_error("usage_session_path_conflict");
        }
        add(accumulated.total_tokens, record.total_tokens);
        accumulated.completed_date = std::max(accumulated.completed_date, record.completed_date);
        if (!accumulated.model || accumulated.model->empty()) accumulated.model = record.model;
        else if (record.model && !record.model->empty() && record.model != accumulated.model) accumulated.model = "multiple";
        if (accumulated.coverage_unit != record.coverage_unit) mixed_coverage.insert(record.record_id);
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
            const auto same = std::find_if(allocation.allocations.begin(), allocation.allocations.end(), [&](const auto& previous) {
                return path_key(addon::path_from_utf8(previous.repository_path)) == path_key(addon::path_from_utf8(item.repository_path));
            });
            if (same == allocation.allocations.end()) allocation.allocations.push_back(item);
            else add(same->total_tokens, item.total_tokens);
        }
    }
    std::vector<addon::Usage_record> result;
    for (auto& [id, record] : sessions) {
        if (mixed_coverage.contains(id)) {
            record.coverage_unit = addon::Usage_coverage_unit::SESSIONS;
            for (auto* field : amounts(record)) {
                field->applicable = true;
                field->value = 0;
                field->coverage = addon::Usage_field_coverage{0, session_files[id], false, addon::Usage_support::UNKNOWN};
            }
        }
        addon::Contract_failure failure;
        if (!addon::decode_usage_record(addon::encode_usage_record(record, 2), &failure, 2)) throw std::runtime_error("usage_result_unrepresentable");
        result.push_back(std::move(record));
    }
    check_cancel(context);
    store_cache(destination, encode_cache(files, owner, receipts));
    return result;
}

void handle_usage(addon::Invocation_context& context)
{
    if (context.request().operation != "scan") { context.reject_unknown_operation(); return; }
    addon::Contract_failure failure;
    const auto version = context.request().capability.version;
    const auto input = addon::decode_usage_scan_request(context.request().payload, &failure, version);
    if (!input) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    if (!input->context.profile_storage) throw std::runtime_error("usage_storage_unavailable");
    const auto profile = profile_for_context(input->context);
    const auto storage = addon::path_from_utf8(input->context.profile_storage->root);
    auto owner = account_key(profile) + ":" + path_string(storage) + ":" +
        (input->native_file_cache ? input->native_file_cache->sha256_hex : std::string{});
    if (version == 3) {
        auto identity = *input;
        identity.snapshot_id.reset();
        identity.cursor.reset();
        owner = addon::to_hex(addon::Sha256::digest(
            addon::serialize_json_canonical(addon::encode_usage_scan_request(identity, 3))));
        const auto& selection = *input->cache_selection;
        if (!addon::validate_usage_cache_root(selection.candidate_root, path_string(storage), &failure)) {
            throw std::runtime_error(failure.reason_code);
        }
        const auto outside_mutable = [&](const std::string& selected) {
            const auto mutable_path = storage / (account_key(profile) + "-usage-v1.json");
            for (auto path = addon::path_from_utf8(selected); path.has_relative_path(); path = path.parent_path()) {
                std::error_code error;
                if (fs::equivalent(path, mutable_path, error)) {
                    return false;
                }
            }
            const auto relative = addon::path_from_utf8(selected).lexically_relative(storage);
            auto name = path_string(*relative.begin());
#ifdef _WIN32
            name = addon::ascii_lowercase(name);
#endif
            return !name.ends_with("-usage-v1.json") &&
                !(name.starts_with("usage-") && name.ends_with(".tmp"));
        };
        if (!outside_mutable(selection.candidate_root) ||
            (selection.accepted && !outside_mutable(selection.accepted->root)))
        {
            throw std::runtime_error("usage_cache_namespace_conflict");
        }
    }
    std::lock_guard lock(s_mutex);
    check_cancel(context);
    std::erase_if(s_snapshots, [](const auto& entry) { return now_ms() - entry.second.touched_ms > 600000; });
    std::size_t begin = 0;
    std::string snapshot_id;
    if (!input->snapshot_id) {
        if (s_snapshots.size() >= 8) throw std::runtime_error("usage_snapshot_capacity");
        Snapshot snapshot;
        snapshot.owner = owner;
        if (version == 3) {
            const auto& selection = *input->cache_selection;
            if (!addon::validate_usage_cache_allocation(selection.candidate_root, path_string(storage), &failure)) {
                throw std::runtime_error(failure.reason_code);
            }
            if (selection.accepted) {
                const auto manifest = addon::validate_usage_cache_candidate(
                    *selection.accepted, path_string(storage), input->context.account_context,
                    [&context] { return context.cancel_requested(); }, &failure);
                if (!manifest) throw std::runtime_error(failure.reason_code);
                if (manifest->format != "kimi-wire-cache@1" || manifest->files.size() != 1 ||
                    manifest->files.front().path != "files.json")
                {
                    throw std::runtime_error("usage_cache_format_unsupported");
                }
            }
        }
        snapshot.records = scan_usage(profile, storage, input->mode, input->native_file_cache, context,
            input->cache_selection ? &*input->cache_selection : nullptr);
        snapshot.scope.observed_at_ms = now_ms();
        snapshot.scope.fresh_for_ms = 60000;
        if (input->context.account_context) snapshot.scope.source_account_key = input->context.account_context->adapter_account_key;
        snapshot.touched_ms = now_ms();
        snapshot_id = addon::random_nonce_hex();
        if (version == 3) {
            // A surviving wire log can be only a subset of a session's events.
            for (auto& record : snapshot.records) record.aggregate_evidence = "partial";
            snapshot.cache_candidate = addon::seal_usage_cache_candidate(
                input->cache_selection->candidate_root, path_string(storage), input->context.account_context,
                snapshot_id, "kimi-wire-cache@1", [&context] { return context.cancel_requested(); }, &failure);
            if (!snapshot.cache_candidate) throw std::runtime_error(failure.reason_code);
        }
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
    if (found == s_snapshots.end() || found->second.owner != owner || begin > found->second.records.size()) throw std::runtime_error("usage_snapshot_unavailable");
    auto& snapshot = found->second;
    snapshot.touched_ms = now_ms();
    addon::Usage_scan_page page;
    page.incomplete_historical_coverage = true;
    page.scope = snapshot.scope;
    page.snapshot_id = snapshot_id;
    page.cache_candidate = snapshot.cache_candidate;
    if (version == 3) {
        page.account_context = input->context.account_context;
        page.profile_storage = input->context.profile_storage;
    }
    const auto limit = std::min(begin + addon::k_max_usage_page_records, snapshot.records.size());
    auto end = begin;
    while (end < limit) {
        page.records.push_back(snapshot.records[end]);
        page.next_cursor = std::to_string(end + 1);
        if (addon::serialize_json(addon::encode_usage_scan_result(page, version)).size() >
            addon::limits::k_max_frame_bytes - 65536)
        {
            page.records.pop_back();
            if (end == begin) throw std::runtime_error("usage_record_frame_capacity");
            break;
        }
        ++end;
    }
    page.complete = end == snapshot.records.size();
    if (!page.complete) page.next_cursor = std::to_string(end);
    else { page.next_cursor.reset(); s_snapshots.erase(found); }
    context.succeed(addon::encode_usage_scan_result(page, version));
}

} // namespace kimi
