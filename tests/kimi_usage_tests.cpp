#include "kimi_accounts.h"
#include "kimi_json.h"
#include "kimi_launcher.h"
#include "kimi_usage.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/sha256.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

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
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    bool finished() const override { return done; }
    void succeed(addon::Json_value result) override { payload = std::move(result); done = true; }
    void fail(addon::Result_status, std::string, std::string) override { throw std::runtime_error("unexpected_failure"); }
    addon::Invoke_request invocation;
    addon::Json_value payload;
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
    kimi::handle_accounts(context);
    addon::Contract_failure failure;
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

} // namespace

int main()
{
    accounts_contract();
    scan_contract();
    missing_sessions_contract();
    return 0;
}
