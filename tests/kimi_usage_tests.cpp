#include "kimi_accounts.h"
#include "kimi_json.h"
#include "kimi_launcher.h"
#include "kimi_usage.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/sha256.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

class Context : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return invocation; }
    bool cancel_requested() const override { return cancelled; }
    bool wait_for_cancel(int) override { return cancelled; }
    void report_progress(std::uint64_t current, std::optional<std::uint64_t> total, std::string) override
    {
        progress_current = current;
        progress_total = total;
    }
    bool finished() const override { return done; }
    void succeed(addon::Json_value result) override { payload = std::move(result); done = true; }
    void fail(addon::Result_status, std::string reason, std::string detail) override
    {
        throw std::runtime_error(reason + ": " + detail);
    }
    addon::Invoke_request invocation;
    addon::Json_value payload;
    std::uint64_t progress_current = 0;
    std::optional<std::uint64_t> progress_total;
    bool cancelled = false;
    bool done = false;
};

class Workspace
{
public:
    Workspace() : path(fs::temp_directory_path() / ("kimi-usage-" + addon::random_nonce_hex()))
    {
        fs::create_directories(path);
    }
    ~Workspace() { std::error_code error; fs::remove_all(path, error); }
    fs::path path;
};

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    require(static_cast<bool>(stream), "fixture write failed");
}

void accounts_contract()
{
    Workspace workspace;
    const auto selected = workspace.path / ".kimi-code-selected";
    const auto sibling = workspace.path / ".kimi-code-other";
    write(selected / "credentials" / "kimi-code.json", "{}");
    write(sibling / "credentials" / "kimi-code.json", "{}");
    addon::Accounts_launcher_request request;
    request.launcher.script_body = "kimi";
    request.launcher.runner_key = addon::k_runner_direct;
#ifdef _WIN32
    request.launch_environment.platform_id = "windows-x86_64";
#else
    request.launch_environment.platform_id = "linux-x86_64";
#endif
    request.launch_environment.effective_environment = kimi::object({
        {"KIMI_CODE_HOME", addon::Json_value(kimi::path_string(selected))}});
    request.normalization_directory = kimi::path_string(workspace.path);
    Context context;
    context.invocation.operation = "discover";
    context.invocation.payload = addon::encode_accounts_launcher_request(request);
    addon::Contract_failure failure;
    require(addon::decode_accounts_launcher_request(context.invocation.payload, &failure).has_value(),
        "account discovery fixture must satisfy the launcher request contract");
    kimi::handle_accounts(context);
    const auto accounts = addon::decode_accounts_result(context.payload, &failure);
    require(accounts && accounts->size() == 2 && accounts->front().adapter_account_key == kimi::account_key(selected),
        "frozen selected home and credential-backed siblings retain distinct accounts");
    addon::Session_launch_context launch;
    launch.account_context = addon::Account_context{2, kimi::account_key(selected), kimi::path_string(selected)};
    require(kimi::profile_for_context(launch) == selected, "exact selected account profile is used without ambient home");
    launch.account_context->profile_path = kimi::path_string(sibling);
    bool rejected = false;
    try { (void)kimi::profile_for_context(launch); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "account key cannot silently select another profile");
}

void scan_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    fs::create_directories(storage);
    const auto session = profile / "sessions" / "Workspace-A" / "Session-A";
    const auto wire = session / "agents" / "Agent-A" / "wire.jsonl";
    const auto repository = workspace.path / "repository";
    fs::create_directories(repository / ".git");
    write(session / "state.json", addon::serialize_json(kimi::object({{"cwd", addon::Json_value(kimi::path_string(repository))}})));
    write(wire,
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"model\":\"one\",\"usage\":{\"inputOther\":3,\"output\":4,\"inputCacheRead\":0}}\n"
        "{\"type\":\"usage.record\",\"time\":1789200001000,\"model\":\"two\",\"usage\":{\"output\":10}}\n"
        "{\"type\":\"usage.record\"");
    Context context;
    auto records = kimi::scan_usage(profile, storage, "incremental", std::nullopt, context);
    require(records.size() == 1 && records.front().total_tokens == 17, "partial last record must not be counted");
    require(records.front().record_id == "kimi:Session-A" && records.front().model == "multiple", "canonical session and mixed model preserved");
    require(records.front().input.coverage->covered_records == 1 && records.front().input.coverage->total_records == 2,
        "partial category coverage must remain partial");
    require(records.front().cache_read.value == 0 && records.front().cache_read.coverage->covered_records == 1 &&
        records.front().cache_creation.coverage->covered_records == 0,
        "observed zero and absent counter remain distinct");
    require(records.front().repository_usage->allocations.size() == 1, "recorded cwd attributes repository");
    fs::remove(wire);
    records = kimi::scan_usage(profile, storage, "incremental", std::nullopt, context);
    require(records.front().total_tokens == 17, "disappeared agent log retains cached usage");
    write(session / "agents" / "Agent-B" / "wire.jsonl",
        "{\"type\":\"usage.record\",\"time\":1789200002000,\"usage\":{\"output\":5}}\n");
    records = kimi::scan_usage(profile, storage, "incremental", std::nullopt, context);
    require(records.size() == 1 && records.front().total_tokens == 22, "all agent logs contribute to the same session");
    const auto legacy_session = profile / "sessions" / "Workspace-B" / "Session-B";
    const auto legacy_wire = legacy_session / "agents" / "Agent-A" / "wire.jsonl";
    const auto identity = kimi::object({{"version", addon::Json_value(1)},
        {"record_id", addon::Json_value("kimi:Session-B")},
        {"session_key", addon::Json_value("kimi:Session-B")},
        {"legacy_record_id", addon::Json_value("kimi:Session-B:old-agent")},
        {"legacy_session_key", addon::Json_value("kimi:Session-B:old-agent")}});
    const auto record = kimi::object({{"session_id", addon::Json_value("6b696d693a53657373696f6e2d42")},
        {"capabilities", kimi::object({{"usage-record-identity", identity}})},
        {"completed_date", addon::Json_value("2026-09-12")}, {"total_tokens", addon::Json_value(11)},
        {"source_path", addon::Json_value(kimi::path_string(legacy_session))},
        {"model", addon::Json_value("old")}, {"counter_field", addon::Json_value("usage")},
        {"counter_semantics", addon::Json_value("per_event_sum")}, {"recorded_cwd", addon::Json_value("")}});
    auto rows = addon::Json_value::array(); rows.push_back(record);
    const auto seed_row = kimi::object({{"path", addon::Json_value(kimi::path_string(legacy_wire))},
        {"size", addon::Json_value(99)}, {"modified_msecs", addon::Json_value(123)}, {"records", rows}});
    const auto bytes = addon::serialize_json(seed_row) + "\n";
    const auto seed_path = storage / "native-seed.jsonl";
    write(seed_path, bytes);
    addon::Usage_native_file_cache seed{kimi::path_string(seed_path),
        addon::to_hex(addon::Sha256::digest(bytes)), kimi::account_key(profile), true};
    records = kimi::scan_usage(profile, storage, "incremental", seed, context);
    require(records.size() == 2 && records.back().record_id == "kimi:Session-B" &&
        records.back().total_tokens == 11 && records.back().input.coverage->support == addon::Usage_support::UNKNOWN,
        "mapped wrapper recovers native session identity and missing source totals without inventing stale breakdown");
    records = kimi::scan_usage(profile, storage, "incremental", seed, context);
    require(records.size() == 2 && records.back().total_tokens == 11, "reoffered seed is idempotent");
    seed.sha256_hex[0] = seed.sha256_hex[0] == 'a' ? 'b' : 'a';
    bool rejected = false;
    try { (void)kimi::scan_usage(profile, storage, "incremental", seed, context); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "wrong seed digest must fail before durable adoption");
    records = kimi::scan_usage(profile, storage, "reset", std::nullopt, context);
    require(records.size() == 1 && records.front().total_tokens == 5, "explicit reset discards retained cached history");
    require(kimi::canonical_usage_session_id("kimi:Session-A:legacy") == "kimi:Session-A" &&
        kimi::canonical_usage_session_id("foreign:Session-A").empty(), "legacy identity converter is provider scoped");
}

std::int64_t cached_modified_ms(const fs::path& cache)
{
    std::ifstream stream(cache, std::ios::binary);
    require(static_cast<bool>(stream), "usage cache must be readable");
    auto files = addon::Json_value::array();
    std::string line;
    while (std::getline(stream, line)) {
        const auto document = kimi::parse(line);
        require(document.has_value(), "usage cache records must remain valid JSON");
        if (const auto* rows = document->find("files")) {
            require(rows->is_array(), "single-document cache must contain a file array");
            for (const auto& row : rows->as_array()) files.push_back(row);
        }
        else
        if (document->find("path")) {
            files.push_back(*document);
        }
    }
    require(stream.eof() && files.as_array().size() == 1, "fixture must cache exactly one wire file");
    const auto& modified = kimi::member(files.as_array().front(), "modified_msecs");
    require(modified.is_integer(), "cached modification time must be integer Unix milliseconds");
    return modified.as_integer();
}

void timestamp_cache_identity_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    const auto wire    = profile / "sessions" / "workspace" / "session" / "agents" / "main" / "wire.jsonl";
    const auto cache   = storage / (kimi::account_key(profile) + "-usage-v1.json");
    fs::create_directories(storage);
    const std::string original =
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":7}}\n";
    const std::string replacement =
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":9}}\n";
    require(original.size() == replacement.size(), "cache identity fixture keeps the file size constant");
    write(wire, original);

    constexpr std::int64_t k_modified_ms = 1704067200123;
    const std::chrono::sys_time<std::chrono::milliseconds> modified{std::chrono::milliseconds{k_modified_ms}};
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    const auto file_modified = std::chrono::clock_cast<fs::file_time_type::clock>(modified);
#else
    const auto file_modified = fs::file_time_type::clock::from_sys(modified);
#endif
    fs::last_write_time(wire, file_modified);
    require(fs::last_write_time(wire) == file_modified, "fixture preserves an exact millisecond timestamp");
    Context context;
    auto records = kimi::scan_usage(profile, storage, "repair", std::nullopt, context);
    require(records.size() == 1 && records.front().total_tokens == 7,
        "initial stable read records the source usage");
    require(cached_modified_ms(cache) == k_modified_ms,
        "stable read persists Unix milliseconds without a file-clock epoch shift");

    // Preserving both metadata fields deliberately exercises the cache identity:
    // a fresh parse would return the replacement's different token total.
    write(wire, replacement);
    fs::last_write_time(wire, file_modified);
    for (int scan = 0; scan < 2; ++scan) {
        records = kimi::scan_usage(profile, storage, "incremental", std::nullopt, context);
        require(records.size() == 1 && records.front().total_tokens == 7,
            "equal size and timestamp reuse the retained usage across repeated scans");
        require(cached_modified_ms(cache) == k_modified_ms,
            "incremental fingerprint uses the same Unix milliseconds as stable reads");
    }

    fs::last_write_time(wire, file_modified + std::chrono::seconds{2});
    records = kimi::scan_usage(profile, storage, "incremental", std::nullopt, context);
    require(records.size() == 1 && records.front().total_tokens == 9,
        "changed timestamp invalidates a same-size file's retained usage");
    require(cached_modified_ms(cache) == k_modified_ms + 2000,
        "refreshed fingerprint preserves the exact timestamp change");
}

void missing_sessions_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    fs::create_directories(profile);
    fs::create_directories(storage);
    Context context;
    require(kimi::scan_usage(profile, storage, "incremental", std::nullopt, context).empty(),
        "existing new profile without sessions has an empty successful scan");
    const auto sessions = profile / "sessions";
    write(sessions / "workspace" / "session" / "agents" / "main" / "wire.jsonl",
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":7}}\n");
    const auto recorded = kimi::scan_usage(profile, storage, "incremental", std::nullopt, context);
    require(recorded.size() == 1 && recorded.front().total_tokens == 7,
        "fixture retains usage before source subtree disappears");
    fs::remove_all(sessions);
    for (const auto mode : {"incremental", "repair"}) {
        const auto records = kimi::scan_usage(profile, storage, mode, std::nullopt, context);
        require(records.size() == 1 && records.front().total_tokens == 7,
            "absent sessions subtree must retain previously accepted cached history");
    }
    write(sessions, "not a directory");
    bool rejected = false;
    try { (void)kimi::scan_usage(profile, storage, "incremental", std::nullopt, context); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "existing non-directory sessions path is not an empty profile");
    fs::remove(sessions);
    const auto retained = kimi::scan_usage(profile, storage, "incremental", std::nullopt, context);
    require(retained.size() == 1 && retained.front().total_tokens == 7,
        "rejected non-directory source cannot replace retained cache");
    rejected = false;
    try { (void)kimi::scan_usage(workspace.path / "missing-profile", storage, "incremental", std::nullopt, context); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "missing profile is unavailable even though missing sessions alone is valid");
    const auto cache = storage / (kimi::account_key(profile) + "-usage-v1.json");
    write(cache, "corrupt retained cache");
    rejected = false;
    try { (void)kimi::scan_usage(profile, storage, "repair", std::nullopt, context); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "empty live inventory cannot bypass retained cache integrity checks");
    std::ifstream preserved(cache);
    std::string bytes((std::istreambuf_iterator<char>(preserved)), {});
    require(bytes == "corrupt retained cache", "failed cache validation leaves original bytes untouched");
}

void large_history_contract()
{
    // A wire log is read whole, so one file is bounded; the history is not.
    // Every mode that reads all logs must accept a history past 512 MiB.
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    fs::create_directories(storage);
    const auto filler = "{\"type\":\"content.part\",\"text\":\"" + std::string(1000 * 1000, 'x') + "\"}\n";
    std::uint64_t history_bytes = 0;
    for (unsigned session = 0; session < 5; ++session) {
        const auto wire = profile / "sessions" / "Workspace-A" / ("Session-" + std::to_string(session)) /
            "agents" / "main" / "wire.jsonl";
        fs::create_directories(wire.parent_path());
        std::ofstream stream(wire, std::ios::binary);
        for (unsigned line = 0; line < 110; ++line) {
            stream << filler;
        }
        stream << "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":" << 100 + session << "}}\n";
        stream.close();
        require(static_cast<bool>(stream), "large wire fixture written");
        history_bytes += fs::file_size(wire);
    }
    require(history_bytes > 512ull * 1024 * 1024, "fixture exceeds the former whole-scan budget");
    Context context;
    for (const auto mode : {"incremental", "repair", "reset"}) {
        const auto started = std::chrono::steady_clock::now();
        const auto records = kimi::scan_usage(profile, storage, mode, std::nullopt, context);
        std::cout << mode << " scan of " << history_bytes << " history bytes: " <<
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() <<
            " ms\n";
        std::uint64_t total = 0;
        for (const auto& record : records) {
            total += record.total_tokens;
        }
        require(records.size() == 5 && total == 510, "a history larger than 512 MiB scans completely in every mode");
    }
}

void large_log_contract()
{
    // A wire log is read one line at a time: one log may grow past 512 MiB,
    // while each line keeps the parser's document bound.
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    fs::create_directories(storage);
    const auto wire = profile / "sessions" / "Workspace-A" / "Session-Large" / "agents" / "main" / "wire.jsonl";
    fs::create_directories(wire.parent_path());
    const auto filler = "{\"type\":\"content.part\",\"text\":\"" + std::string(1000 * 1000, 'x') + "\"}\n";
    {
        std::ofstream stream(wire, std::ios::binary);
        stream << "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":11}}\n";
        for (unsigned line = 0; line < 540; ++line) {
            stream << filler;
        }
        stream << "{\"type\":\"usage.record\",\"time\":1789200001000,\"usage\":{\"output\":13}}\n";
        stream.close();
        require(static_cast<bool>(stream), "large wire fixture written");
    }
    require(fs::file_size(wire) > 512ull * 1024 * 1024, "one log exceeds the former per-log bound");
    Context context;
    for (const auto mode : {"incremental", "repair"}) {
        const auto records = kimi::scan_usage(profile, storage, mode, std::nullopt, context);
        require(records.size() == 1 && records.front().total_tokens == 24, "a log larger than 512 MiB scans completely");
    }

    const auto oversized = profile / "sessions" / "Workspace-A" / "Session-Oversized" / "agents" / "main" / "wire.jsonl";
    write(oversized, "{\"type\":\"content.part\",\"text\":\"" + std::string(5 * 1000 * 1000, 'x') + "\"}\n");
    bool rejected = false;
    try { (void)kimi::scan_usage(profile, storage, "repair", std::nullopt, context); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "a line past the parser's document bound still fails the scan");
}

std::vector<std::string> lines_of(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) lines.push_back(std::move(line));
    return lines;
}

void line_cache_contract()
{
    // The cache holds one line per wire log and per seed receipt, so neither
    // the number of logs nor the number of imported seeds is bounded. A cache
    // written as one document is still read and rewritten line by line.
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    fs::create_directories(storage);
    for (unsigned session = 0; session < 3; ++session) {
        write(profile / "sessions" / "Workspace-A" / ("Session-" + std::to_string(session)) / "agents" / "main" / "wire.jsonl",
            "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":" + std::to_string(20 + session) + "}}\n");
    }
    const auto cache = storage / (kimi::account_key(profile) + "-usage-v1.json");
    Context context;
    const auto total = [&](const char* mode, const std::optional<addon::Usage_native_file_cache>& seed) {
        std::uint64_t sum = 0;
        for (const auto& record : kimi::scan_usage(profile, storage, mode, seed, context)) sum += record.total_tokens;
        return sum;
    };
    require(total("incremental", std::nullopt) == 63, "first scan counts every log");
    require(lines_of(cache).size() == 4, "the cache holds a header and one line per wire log");

    // Rewrite the cache as the single document earlier versions wrote, with
    // 1024 seed receipts, the most that format admitted.
    auto rows = addon::Json_value::array();
    for (const auto& line : lines_of(cache)) {
        const auto value = kimi::parse(line);
        require(value && value->is_object(), "cache line parses");
        if (value->find("path")) rows.push_back(*value);
    }
    auto receipts = addon::Json_value::array();
    for (unsigned index = 0; index < 1024; ++index) {
        receipts.push_back(addon::Json_value(addon::to_hex(addon::Sha256::digest("receipt-" + std::to_string(index)))));
    }
    write(cache, addon::serialize_json(kimi::object({{"version", addon::Json_value(1)},
        {"profile", addon::Json_value(kimi::account_key(profile))}, {"files", rows}, {"receipts", receipts}})));
    require(total("incremental", std::nullopt) == 63, "a single-document cache upgrades with exact totals");
    require(lines_of(cache).size() == 1 + 1024 + 3, "the upgraded cache keeps every receipt and log on its own line");

    const auto seed_path = storage / "native-file-cache.jsonl";
    write(seed_path, "");
    const addon::Usage_native_file_cache seed{kimi::path_string(seed_path), addon::to_hex(addon::Sha256::digest("")),
        kimi::account_key(profile), false};
    require(total("incremental", seed) == 63, "a seed beyond 1024 earlier imports is still accepted");
    require(lines_of(cache).size() == 1 + 1025 + 3, "the new receipt is retained");
}

void file_count_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    const auto cache   = storage / (kimi::account_key(profile) + "-usage-v1.json");
    fs::create_directories(profile);
    fs::create_directories(storage);
    constexpr unsigned k_file_count = 100001;
    {
        std::ofstream stream(cache, std::ios::binary);
        stream << "{\"version\":1,\"profile\":" << addon::serialize_json(addon::Json_value(kimi::account_key(profile))) <<
            ",\"receipts\":[],\"files\":[";
        for (unsigned index = 0; index < k_file_count; ++index) {
            const auto path = profile / "sessions" / "workspace" / "session" / "agents" /
                ("agent-" + std::to_string(index)) / "wire.jsonl";
            if (index != 0) stream << ',';
            stream << addon::serialize_json(kimi::object({
                {"path", addon::Json_value(kimi::path_string(path))}, {"size", addon::Json_value(0)},
                {"modified_msecs", addon::Json_value(0)}, {"recorded_cwd", addon::Json_value("")},
                {"stale_breakdown", addon::Json_value(false)}, {"usage", addon::Json_value{}}}));
        }
        stream << "]}";
        stream.close();
        require(static_cast<bool>(stream), "large retained-file fixture written");
    }
    Context context;
    require(kimi::scan_usage(profile, storage, "incremental", std::nullopt, context).empty(),
        "more than 100000 retained wire files are accepted without inventing usage");
    std::ifstream stream(cache, std::ios::binary);
    std::size_t rows = 0;
    std::string line;
    while (std::getline(stream, line)) ++rows;
    require(stream.eof() && rows == 1 + k_file_count,
        "migration retains every file past the former history count limit");
}

void live_file_count_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    const auto agents  = profile / "sessions" / "workspace" / "session" / "agents";
    fs::create_directories(storage);
    constexpr unsigned k_file_count = 100001;
    const auto started = std::chrono::steady_clock::now();
    for (unsigned index = 0; index < k_file_count; ++index) {
        const auto wire = agents / ("agent-" + std::to_string(index)) / "wire.jsonl";
        write(wire, index == k_file_count - 1
            ? "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":7}}\n"
            : "");
    }
    std::cout << "Created " << k_file_count << " live wire files in " <<
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() <<
        " ms" << std::endl;
    Context context;
    const auto records = kimi::scan_usage(profile, storage, "incremental", std::nullopt, context);
    require(records.size() == 1 && records.front().total_tokens == 7,
        "a live history beyond 100000 files includes every agent's usage");
    require(context.progress_current == k_file_count && context.progress_total == k_file_count,
        "large live histories report progress through the complete file inventory");
}

void native_aliases_contract()
{
    Workspace workspace;
    const auto profile   = workspace.path / ".kimi-code";
    const auto storage   = workspace.path / "package-profile";
    const auto cache     = storage / (kimi::account_key(profile) + "-usage-v1.json");
    const auto seed_path = storage / "native-aliases.jsonl";
    fs::create_directories(profile);
    fs::create_directories(storage);
    const auto canonical = profile / "sessions" / "workspace" / "session" / "agents" / "main" / "wire.jsonl";
    const auto alias     = profile / "sessions" / "." / "workspace" / "session" / "agents" / "main" / "wire.jsonl";
    const auto seed_row = [](const fs::path& path) {
        return addon::serialize_json(kimi::object({
            {"path", addon::Json_value(kimi::path_string(path))}, {"size", addon::Json_value(0)},
            {"modified_msecs", addon::Json_value(0)}, {"records", addon::Json_value::array()}})) + "\n";
    };
    const auto canonical_row = seed_row(canonical);
    const auto alias_row     = seed_row(alias);
    require(canonical_row != alias_row && canonical.lexically_normal() == alias.lexically_normal(),
        "native alias fixture uses distinct raw paths naming the same wire log");
    const auto bytes = canonical_row + alias_row;
    write(seed_path, bytes);
    addon::Usage_native_file_cache seed{kimi::path_string(seed_path),
        addon::to_hex(addon::Sha256::digest(bytes)), kimi::account_key(profile), false};
    const auto verify_cache = [&] {
        std::size_t files = 0, receipts = 0;
        for (const auto& line : lines_of(cache)) {
            const auto value = kimi::parse(line);
            require(value && value->is_object(), "native alias cache records remain valid JSON");
            if (const auto* rows = value->find("files")) {
                require(rows->is_array() && rows->as_array().size() == 1,
                    "native aliases retain exactly one file in a document cache");
                files += rows->as_array().size();
                require(kimi::text(rows->as_array().front(), "path") == kimi::path_string(alias),
                    "native aliases retain the first lexical raw path");
                const auto& imported = kimi::member(*value, "receipts");
                require(imported.is_array(), "document cache retains import receipts");
                receipts += imported.as_array().size();
            }
            else
            if (value->find("path")) {
                ++files;
                require(kimi::text(*value, "path") == kimi::path_string(alias),
                    "native aliases retain the first lexical raw path");
            }
            else
            if (value->find("receipt")) {
                ++receipts;
            }
        }
        require(files == 1 && receipts == 1, "native aliases retain one file and one import receipt");
    };
    Context context;
    require(kimi::scan_usage(profile, storage, "incremental", seed, context).empty(),
        "distinct native paths naming the same wire log are accepted");
    verify_cache();
    require(kimi::scan_usage(profile, storage, "incremental", seed, context).empty(),
        "reoffering native aliases remains idempotent");
    verify_cache();
    const auto accepted = lines_of(cache);
    const auto duplicate_bytes = canonical_row + canonical_row;
    write(seed_path, duplicate_bytes);
    seed.sha256_hex = addon::to_hex(addon::Sha256::digest(duplicate_bytes));
    bool rejected = false;
    try { (void)kimi::scan_usage(profile, storage, "incremental", seed, context); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "exact duplicate raw native paths remain invalid");
    require(lines_of(cache) == accepted, "rejected duplicate import preserves the accepted cache");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2) {
            const std::string_view selected = argv[1];
            if (selected == "--native-aliases") { native_aliases_contract(); return 0; }
            if (selected == "--line-cache" || selected == "--history-limits") line_cache_contract();
            if (selected == "--file-count" || selected == "--history-limits") file_count_contract();
            if (selected == "--live-file-count" || selected == "--history-limits") live_file_count_contract();
            require(selected == "--line-cache" || selected == "--file-count" ||
                selected == "--live-file-count" || selected == "--history-limits",
                "unknown usage test selection");
            return 0;
        }
        native_aliases_contract();
        accounts_contract();
        scan_contract();
        timestamp_cache_identity_contract();
        missing_sessions_contract();
        large_history_contract();
        large_log_contract();
        line_cache_contract();
        file_count_contract();
        live_file_count_contract();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
