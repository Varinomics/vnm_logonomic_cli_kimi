#include "kimi_accounts.h"
#include "kimi_json.h"
#include "kimi_launcher.h"
#include "kimi_usage.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/stored_usage_accounting.h>

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

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
        path = fs::canonical(path);
    }
    ~Workspace() { std::error_code error; fs::remove_all(path, error); }
    fs::path path;
};

class Selected_cache
{
public:
    explicit Selected_cache(fs::path storage) : m_storage(std::move(storage)) {}

    std::vector<addon::Usage_record> scan(const fs::path& profile,
        std::string_view mode, Context& context)
    {
        const auto accepted = mode == "reset" ? std::nullopt : m_accepted;
        addon::Contract_failure failure;
        if (accepted && !addon::validate_usage_cache_candidate(*accepted,
            kimi::path_string(m_storage), std::nullopt,
            [&context] { return context.cancel_requested(); }, &failure))
        {
            throw std::runtime_error(failure.reason_code);
        }
        const auto candidate = m_storage / addon::random_nonce_hex();
        require(fs::create_directory(candidate), "allocate current collector cache");
        const addon::Usage_cache_selection selection{accepted, kimi::path_string(candidate)};
        auto records = kimi::scan_usage(profile, m_storage, mode, context, selection);
        accept(candidate);
        return records;
    }

    void accept(const fs::path& root)
    {
        addon::Contract_failure failure;
        const auto sealed = addon::seal_usage_cache_candidate(kimi::path_string(root),
            kimi::path_string(m_storage), std::nullopt, addon::random_nonce_hex(),
            "kimi-wire-cache@2", [] { return false; }, &failure);
        require(sealed.has_value(), "seal current collector cache fixture");
        m_accepted = *sealed;
    }

    void reseal()
    {
        const auto root = path().parent_path();
        require(fs::remove(root / "cache-manifest.json"), "replace fixture cache manifest");
        accept(root);
    }

    fs::path path() const
    {
        require(m_accepted.has_value(), "collector cache must have an accepted selection");
        return addon::path_from_utf8(m_accepted->root) / "files.json";
    }

private:
    fs::path m_storage;
    std::optional<addon::Usage_cache_reference> m_accepted;
};

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    require(static_cast<bool>(stream), "fixture write failed");
}

void native_session(const fs::path& session, std::string_view parent = {}, std::string_view cwd = {})
{
    auto state = json_util::object({{"id", addon::Json_value(kimi::path_string(session.filename()))},
        {"version", addon::Json_value(2)}, {"createdAt", addon::Json_value(std::int64_t{1789199999000})},
        {"updatedAt", addon::Json_value(std::int64_t{1789200000000})}, {"archived", addon::Json_value(false)},
        {"agents", json_util::object({{"main", json_util::object({{"type", addon::Json_value("main")}})}})},
        {"custom", addon::Json_value::object()}});
    if (!parent.empty()) state.set("forkedFrom", addon::Json_value(parent));
    if (!cwd.empty()) state.set("cwd", addon::Json_value(cwd));
    write(session / "state.json", addon::serialize_json(state));
}

void legacy_metadata_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / "native-profile";
    const auto storage = workspace.path / "cache";
    const auto repository = workspace.path / "repository";
    const auto session = profile / "sessions/workspace/legacy";
    const auto modern = profile / "sessions/workspace/modern";
    fs::create_directories(storage);
    fs::create_directories(repository / ".git");
    auto state = json_util::object({{"createdAt", addon::Json_value("2026-09-12T07:59:59.000Z")},
        {"updatedAt", addon::Json_value("2026-09-12T08:00:00.000Z")},
        {"agents", addon::Json_value::object()}, {"custom", addon::Json_value::object()},
        {"workDir", addon::Json_value(kimi::path_string(repository))}});
    write(session / "state.json", addon::serialize_json(state));
    write(session / "agents/main/wire.jsonl",
        "{\"type\":\"metadata\",\"protocol_version\":\"1.4\",\"created_at\":1789199999000}\n"
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":70}}\n");
    native_session(modern);
    write(modern / "agents/main/wire.jsonl",
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":30}}\n");
    Selected_cache selected(storage);
    Context context;
    const auto records = selected.scan(profile, "incremental", context);
    require(records.size() == 2, "legacy and current session metadata share one first scan");
    const auto legacy = std::find_if(records.begin(), records.end(), [](const auto& record) {
        return record.record_id == "kimi:legacy";
    });
    require(legacy != records.end() && legacy->total_tokens == 70 && legacy->repository_usage &&
        legacy->repository_usage->allocations.size() == 1 &&
        legacy->repository_usage->allocations.front().repository_path == kimi::path_string(repository),
        "legacy identity and workDir preserve billable history and repository attribution");

    native_session(session, {}, kimi::path_string(repository));
    const auto migrated = selected.scan(profile, "incremental", context);
    require(migrated.size() == 2 && migrated[0].total_tokens + migrated[1].total_tokens == 100,
        "native metadata migration preserves the accepted session identity and counts");

    for (const auto& [field, value] : std::vector<std::pair<std::string, addon::Json_value>>{
             {"id", addon::Json_value("another-session")}, {"version", addon::Json_value(2)},
             {"version", addon::Json_value(3)}, {"createdAt", addon::Json_value("2026-02-30T07:59:59.000Z")},
             {"createdAt", addon::Json_value("2026-09-12T07:59:59")},
             {"forkedFrom", addon::Json_value("../outside")}, {"forkedFrom", addon::Json_value("missing")}})
    {
        auto invalid = state;
        invalid.set(field, value);
        write(session / "state.json", addon::serialize_json(invalid));
        bool rejected = false;
        try { selected.scan(profile, "reset", context); }
        catch (const kimi::Usage_ownership_unavailable&) { rejected = true; }
        require(rejected, "legacy normalization cannot invent identity, time, or ancestry");
    }
}

int observe_history(const fs::path& profile)
{
    // This opt-in diagnostic reads native history and writes only its disposable cache.
    // Never include native paths, session identities, or exception text in its output.
    try {
        Workspace workspace;
        Selected_cache selected(workspace.path);
        Context context;
        const auto records = selected.scan(profile, "incremental", context);
        std::uint64_t total = 0;
        for (const auto& record : records) total += record.total_tokens;
        std::cout << "history observation: files=" << context.progress_current << ", records=" << records.size()
            << ", total_tokens=" << total << '\n';
        return 0;
    }
    catch (const kimi::Usage_ownership_unavailable&) { std::cerr << "usage_ownership_unavailable\n"; }
    catch (const kimi::Usage_observation_unsettled&) { std::cerr << "usage_observation_unsettled\n"; }
    catch (const std::exception&) { std::cerr << "history observation failed\n"; }
    return 1;
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
    request.launch_environment.effective_environment = json_util::object({
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

void fork_ownership_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    Selected_cache selected(storage);
    const auto parent  = profile / "sessions" / "workspace" / "parent-session";
    const auto child   = profile / "sessions" / "workspace" / "child-session";
    fs::create_directories(storage);
    const std::string inherited =
        "{\"type\":\"metadata\",\"protocol_version\":\"1.5\",\"created_at\":1789199999000}\n"
        "{\"type\":\"usage.record\",\"agentId\":\"main\",\"time\":1789200000000,"
        "\"model\":\"kimi\",\"usageScope\":\"turn\",\"usage\":{\"inputOther\":1000,\"output\":200}}\n";
    write(parent / "state.json",
        "{\"id\":\"parent-session\",\"version\":2,\"createdAt\":1789199999000,"
        "\"updatedAt\":1789200000000,\"archived\":false,\"agents\":{\"main\":{\"type\":\"main\"}},\"custom\":{}}");
    write(parent / "agents" / "main" / "wire.jsonl", inherited);
    write(child / "state.json",
        "{\"id\":\"child-session\",\"version\":2,\"createdAt\":1789200001000,"
        "\"updatedAt\":1789200002000,\"archived\":false,\"forkedFrom\":\"parent-session\","
        "\"agents\":{\"main\":{\"type\":\"main\"}},\"custom\":{}}");
    write(child / "agents" / "main" / "wire.jsonl", inherited +
        "{\"type\":\"forked\",\"agentId\":\"main\",\"time\":1789200001000}\n"
        "{\"type\":\"usage.record\",\"agentId\":\"main\",\"time\":1789200002000,"
        "\"model\":\"kimi\",\"usageScope\":\"turn\",\"usage\":{\"output\":300}}\n");
    Context context;
    const auto records = selected.scan(profile, "incremental", context);
    std::uint64_t total = 0, parent_total = 0, child_total = 0;
    for (const auto& record : records) {
        std::cout << record.record_id << ": " << record.total_tokens << '\n';
        total += record.total_tokens;
        if (record.record_id == "kimi:parent-session") parent_total += record.total_tokens;
        if (record.record_id == "kimi:child-session") child_total += record.total_tokens;
    }
    std::cout << "actual_new_usage=1500, scanned_total=" << total << '\n';
    require(records.size() == 2 && parent_total == 1200, "inherited usage must retain its original session owner");
    require(child_total == 300, "a fork must receive only its newly incurred usage");
    require(total == 1500, "a native fork must not count its copied usage again");
}

void fork_chains_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    Selected_cache selected(storage);
    const auto directory = profile / "sessions" / "workspace";
    fs::create_directories(storage);
    const std::string metadata = "{\"type\":\"metadata\",\"protocol_version\":\"1.5\",\"created_at\":1789199999000}\n";
    const std::string marker = "{\"type\":\"forked\",\"agentId\":\"main\",\"time\":1789200001000}\n";
    const auto usage = [](int tokens) {
        return "{\"type\":\"usage.record\",\"agentId\":\"main\",\"time\":1789200000000,"
            "\"model\":\"kimi\",\"usageScope\":\"turn\",\"usage\":{\"output\":" + std::to_string(tokens) + "}}\n";
    };
    const auto add_session = [&](std::string_view id, std::string_view parent, const std::string& events) {
        const auto session = directory / addon::path_from_utf8(id);
        native_session(session, parent);
        write(session / "agents" / "main" / "wire.jsonl", metadata + events);
    };
    // A turn fork's active chain omits reverted spend and can omit an older
    // fork marker. Creator usage still includes the raw reverted occurrence.
    add_session("root", {}, usage(100) + usage(200) +
        "{\"type\":\"agent.switched\",\"agentId\":\"main\",\"branch\":\"after-undo\","
        "\"base\":{\"branch\":\"main\",\"line\":2},\"time\":1789200000500}\n" + usage(300));
    add_session("child", "root", usage(100) + usage(300) + marker + usage(400));
    add_session("nested", "child", usage(100) + usage(300) + usage(400) + marker + usage(500));
    add_session("sibling", "root", usage(100) + usage(300) + marker + usage(600));
    add_session("independent", {}, usage(100));
    add_session("zero", "root", usage(100) + usage(300) + marker);
    // Future native first-creation witness, preserved on full/turn copies.
    // This fixture does not claim that the installed producer emits it yet.
    native_session(directory / "created-here", "unavailable-ancestor");
    write(directory / "created-here/agents/main/wire.jsonl",
        "{\"type\":\"metadata\",\"protocol_version\":\"1.5\",\"created_at\":1789199999000,"
        "\"creator_session_id\":\"created-here\"}\n" + usage(80));
    Context context;
    auto records = selected.scan(profile, "incremental", context);
    const auto verify = [&](std::uint64_t sibling) {
        std::uint64_t total = 0;
        for (const auto& record : records) {
            total += record.total_tokens;
            const auto expected = record.record_id == "kimi:root" ? 600 :
                record.record_id == "kimi:child" ? 400 : record.record_id == "kimi:nested" ? 500 :
                record.record_id == "kimi:sibling" ? sibling : record.record_id == "kimi:independent" ? 100 :
                record.record_id == "kimi:created-here" ? 80 : 0;
            require(record.total_tokens == expected, "each original keeps its own spend through full and turn-specific chains");
            if (record.record_id == "kimi:zero") {
                require(record.output.value == 0 && record.output.coverage->covered_records == 0 &&
                    record.output.coverage->total_records == 0 &&
                    record.output.coverage->support == addon::Usage_support::UNKNOWN,
                    "a zero-own fork remains an explicit stable row with empty occurrence coverage");
            }
        }
        require(records.size() == 7 && total == 1680 + sibling,
            "sibling copies are removed independently and byte-identical unrelated originals survive");
    };
    verify(600);
    {
        std::ofstream append(directory / "sibling" / "agents" / "main" / "wire.jsonl", std::ios::binary | std::ios::app);
        append << usage(70);
        require(static_cast<bool>(append), "native own append fixture written");
    }
    records = selected.scan(profile, "incremental", context);
    verify(670);
    fs::remove_all(directory / "root");
    fs::remove_all(directory / "child");
    records = selected.scan(profile, "repair", context);
    verify(670);

    const auto cache = selected.path();
    const auto accepted_bytes = [&] {
        std::ifstream input(cache, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), {});
    }();
    const auto unavailable = [&](bool unsettled = false) {
        bool rejected = false;
        try { (void)selected.scan(profile, "incremental", context); }
        catch (const kimi::Usage_ownership_unavailable&) { rejected = !unsettled; }
        catch (const kimi::Usage_observation_unsettled&) { rejected = unsettled; }
        require(rejected, "unproved ownership must be explicitly unavailable");
        std::ifstream input(cache, std::ios::binary);
        require(std::string((std::istreambuf_iterator<char>(input)), {}) == accepted_bytes,
            "unavailable ownership cannot replace the retained generation");
    };
    add_session("orphan", "missing", usage(100) + marker + usage(25));
    unavailable();
    fs::remove_all(directory / "orphan");
    add_session("markerless", "independent", usage(100));
    unavailable();
    fs::remove_all(directory / "markerless");
    native_session(directory / "foreign-witness", "independent");
    write(directory / "foreign-witness/agents/main/wire.jsonl",
        "{\"type\":\"metadata\",\"protocol_version\":\"1.5\",\"created_at\":1789199999000,"
        "\"creator_session_id\":\"independent\"}\n" + usage(100));
    unavailable();
    fs::remove_all(directory / "foreign-witness");
    native_session(directory / "invalid-witness", "independent");
    for (const auto& invalid : {
        std::string("{\"type\":\"metadata\",\"creator_session_id\":\"invalid-witness\"}\n"),
        std::string("{\"type\":\"metadata\",\"protocol_version\":\"1.5\",\"created_at\":1789199999000,"
            "\"creator_session_id\":12}\n"),
        metadata + "{\"type\":\"metadata\",\"protocol_version\":\"1.5\",\"created_at\":1789199999000,"
            "\"creator_session_id\":\"invalid-witness\"}\n"})
    {
        write(directory / "invalid-witness/agents/main/wire.jsonl", invalid + usage(100));
        unavailable();
    }
    fs::remove_all(directory / "invalid-witness");
    add_session("cycle-a", "cycle-b", marker + usage(5));
    add_session("cycle-b", "cycle-a", marker + usage(6));
    unavailable();
    fs::remove_all(directory / "cycle-a");
    fs::remove_all(directory / "cycle-b");
    // One copied occurrence cannot stand in for two identical originals.
    add_session("excess-copy", "independent", usage(100) + usage(100) + marker);
    unavailable();
    fs::remove_all(directory / "excess-copy");
    add_session("unpublished", {}, usage(17));
    fs::remove(directory / "unpublished" / "state.json");
    unavailable(true);
}

void raw_fork_mode_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    Selected_cache selected(storage);
    const auto parent = profile / "sessions/workspace/parent";
    const auto child = profile / "sessions/workspace/child";
    fs::create_directories(storage);
    native_session(parent);
    native_session(child, "parent");
    const std::string inherited = "{\"type\":\"usage.record\",\"agentId\":\"main\","
        "\"time\":1789200000000,\"usage\":{\"output\":100}}\n";
    const auto wire = child / "agents/main/wire.jsonl";
    write(parent / "agents/main/wire.jsonl", inherited);
    write(wire, inherited + "{\"type\":\"forked\",\"agentId\":\"main\",\"time\":1789200001000}\n"
        "{\"type\":\"usage.record\",\"agentId\":\"main\",\"time\":1789200002000,\"usage\":{\"output\":30}}\n");
    Context context;
    (void)selected.scan(profile, "incremental", context);
    const auto cache = selected.path();
    std::string raw;
    {
        std::ifstream input(cache, std::ios::binary);
        std::string line;
        while (std::getline(input, line)) {
            auto row = json_util::parse(line);
            require(row.has_value(), "scanner cache fixture parses");
            if (row->find("fork_event")) continue;
            row->erase("usage-fork-ownership");
            if (json_util::text(*row, "path") == kimi::path_string(wire)) {
                auto record = addon::decode_stored_usage_accounting(json_util::member(*row, "usage"), nullptr);
                require(record.has_value(), "raw accounting fixture has a child record");
                record->total_tokens = 130;
                record->output.value = 130;
                for (auto* field : {&record->input, &record->output, &record->cache_read, &record->cache_creation}) {
                    field->coverage->total_records = 2;
                }
                record->output.coverage->covered_records = 2;
                record->repository_usage->unattributed_tokens = 130;
                row->set("usage", addon::encode_stored_usage_accounting(*record));
            }
            raw += addon::serialize_json(*row) + '\n';
        }
    }
    // Current cache@2 permits raw accounting without optional ownership
    // evidence. Ordinary scans must preserve that accepted baseline.
    write(cache, raw);
    selected.reseal();
    for (const auto mode : {"incremental", "repair"}) {
        const auto records = selected.scan(profile, mode, context);
        const auto child_record = std::find_if(records.begin(), records.end(), [](const auto& record) {
            return record.record_id == "kimi:child";
        });
        require(child_record != records.end() && child_record->total_tokens == 130,
            "ordinary reparses preserve accepted raw mode until an accepted correction");
    }
    {
        std::ofstream append(wire, std::ios::binary | std::ios::app);
        append << "{\"type\":\"usage.record\",\"agentId\":\"main\",\"time\":1789200003000,\"usage\":{\"output\":7}}\n";
        require(static_cast<bool>(append), "raw own growth fixture written");
    }
    const auto records = selected.scan(profile, "incremental", context);
    const auto child_record = std::find_if(records.begin(), records.end(), [](const auto& record) {
        return record.record_id == "kimi:child";
    });
    require(child_record != records.end() && child_record->total_tokens == 137,
        "ordinary raw append retains its baseline and accounts new own spend");
}

void scan_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    Selected_cache selected(storage);
    fs::create_directories(storage);
    const auto session = profile / "sessions" / "Workspace-A" / "Session-A";
    const auto wire = session / "agents" / "Agent-A" / "wire.jsonl";
    const auto repository = workspace.path / "repository";
    fs::create_directories(repository / ".git");
    native_session(session, {}, kimi::path_string(repository));
    write(wire,
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"model\":\"one\",\"usage\":{\"inputOther\":3,\"output\":4,\"inputCacheRead\":0}}\n"
        "{\"type\":\"usage.record\",\"time\":1789200001000,\"model\":\"two\",\"usage\":{\"output\":10}}\n"
        "{\"type\":\"usage.record\"");
    Context context;
    auto records = selected.scan(profile, "incremental", context);
    require(records.size() == 1 && records.front().total_tokens == 17, "partial last record must not be counted");
    require(records.front().record_id == "kimi:Session-A" && records.front().model == "multiple", "canonical session and mixed model preserved");
    require(records.front().input.coverage->covered_records == 1 && records.front().input.coverage->total_records == 2,
        "partial category coverage must remain partial");
    require(records.front().cache_read.value == 0 && records.front().cache_read.coverage->covered_records == 1 &&
        records.front().cache_creation.coverage->covered_records == 0,
        "observed zero and absent counter remain distinct");
    require(records.front().repository_usage->allocations.size() == 1, "recorded cwd attributes repository");
    fs::remove(wire);
    records = selected.scan(profile, "incremental", context);
    require(records.front().total_tokens == 17, "disappeared agent log retains cached usage");
    write(session / "agents" / "Agent-B" / "wire.jsonl",
        "{\"type\":\"usage.record\",\"time\":1789200002000,\"usage\":{\"output\":5}}\n");
    records = selected.scan(profile, "incremental", context);
    require(records.size() == 1 && records.front().total_tokens == 22, "all agent logs contribute to the same session");
    records = selected.scan(profile, "reset", context);
    require(records.size() == 1 && records.front().total_tokens == 5, "explicit reset discards retained cached history");
    const auto session_fact = kimi::describe_usage_session({records.front().record_id, records.front().session_key});
    require(session_fact && session_fact->provider_session_id == "Session-A" && !session_fact->excludes_turn_in_flight,
        "current usage identity correlates with the native session without excluding an active turn");
}

std::int64_t cached_modified_ms(const fs::path& cache)
{
    std::ifstream stream(cache, std::ios::binary);
    require(static_cast<bool>(stream), "usage cache must be readable");
    auto files = addon::Json_value::array();
    std::string line;
    while (std::getline(stream, line)) {
        const auto document = json_util::parse(line);
        require(document.has_value(), "usage cache records must remain valid JSON");
        if (document->find("path")) {
            files.push_back(*document);
        }
    }
    require(stream.eof() && files.as_array().size() == 1, "fixture must cache exactly one wire file");
    const auto& modified = json_util::member(files.as_array().front(), "modified_msecs");
    require(modified.is_integer(), "cached modification time must be integer Unix milliseconds");
    return modified.as_integer();
}

void timestamp_cache_identity_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    Selected_cache selected(storage);
    const auto wire    = profile / "sessions" / "workspace" / "session" / "agents" / "main" / "wire.jsonl";
    fs::create_directories(storage);
    const std::string original =
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":7}}\n";
    const std::string replacement =
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":9}}\n";
    require(original.size() == replacement.size(), "cache identity fixture keeps the file size constant");
    native_session(wire.parent_path().parent_path().parent_path());
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
    auto records = selected.scan(profile, "repair", context);
    require(records.size() == 1 && records.front().total_tokens == 7,
        "initial stable read records the source usage");
    require(cached_modified_ms(selected.path()) == k_modified_ms,
        "stable read persists Unix milliseconds without a file-clock epoch shift");

    // Preserving both metadata fields deliberately exercises the cache identity:
    // a fresh parse would return the replacement's different token total.
    write(wire, replacement);
    fs::last_write_time(wire, file_modified);
    for (int scan = 0; scan < 2; ++scan) {
        records = selected.scan(profile, "incremental", context);
        require(records.size() == 1 && records.front().total_tokens == 7,
            "equal size and timestamp reuse the retained usage across repeated scans");
        require(cached_modified_ms(selected.path()) == k_modified_ms,
            "incremental fingerprint uses the same Unix milliseconds as stable reads");
    }

    fs::last_write_time(wire, file_modified + std::chrono::seconds{2});
    records = selected.scan(profile, "incremental", context);
    require(records.size() == 1 && records.front().total_tokens == 9,
        "changed timestamp invalidates a same-size file's retained usage");
    require(cached_modified_ms(selected.path()) == k_modified_ms + 2000,
        "refreshed fingerprint preserves the exact timestamp change");
}

void missing_sessions_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    Selected_cache selected(storage);
    fs::create_directories(profile);
    fs::create_directories(storage);
    Context context;
    require(selected.scan(profile, "incremental", context).empty(),
        "existing new profile without sessions has an empty successful scan");
    const auto sessions = profile / "sessions";
    native_session(sessions / "workspace" / "session");
    write(sessions / "workspace" / "session" / "agents" / "main" / "wire.jsonl",
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":7}}\n");
    const auto recorded = selected.scan(profile, "incremental", context);
    require(recorded.size() == 1 && recorded.front().total_tokens == 7,
        "fixture retains usage before source subtree disappears");
    fs::remove_all(sessions);
    for (const auto mode : {"incremental", "repair"}) {
        const auto records = selected.scan(profile, mode, context);
        require(records.size() == 1 && records.front().total_tokens == 7,
            "absent sessions subtree must retain previously accepted cached history");
    }
    write(sessions, "not a directory");
    bool rejected = false;
    try { (void)selected.scan(profile, "incremental", context); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "existing non-directory sessions path is not an empty profile");
    fs::remove(sessions);
    const auto retained = selected.scan(profile, "incremental", context);
    require(retained.size() == 1 && retained.front().total_tokens == 7,
        "rejected non-directory source cannot replace retained cache");
    rejected = false;
    try { (void)selected.scan(workspace.path / "missing-profile", "incremental", context); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "missing profile is unavailable even though missing sessions alone is valid");
    const auto cache = selected.path();
    write(cache, "corrupt retained cache");
    rejected = false;
    try { (void)selected.scan(profile, "repair", context); }
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
    Selected_cache selected(storage);
    fs::create_directories(storage);
    const auto filler = "{\"type\":\"content.part\",\"text\":\"" + std::string(1000 * 1000, 'x') + "\"}\n";
    std::uint64_t history_bytes = 0;
    for (unsigned session = 0; session < 5; ++session) {
        const auto wire = profile / "sessions" / "Workspace-A" / ("Session-" + std::to_string(session)) /
            "agents" / "main" / "wire.jsonl";
        native_session(wire.parent_path().parent_path().parent_path());
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
        const auto records = selected.scan(profile, mode, context);
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
    Selected_cache selected(storage);
    fs::create_directories(storage);
    const auto wire = profile / "sessions" / "Workspace-A" / "Session-Large" / "agents" / "main" / "wire.jsonl";
    native_session(wire.parent_path().parent_path().parent_path());
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
        const auto records = selected.scan(profile, mode, context);
        require(records.size() == 1 && records.front().total_tokens == 24, "a log larger than 512 MiB scans completely");
    }

    const auto oversized = profile / "sessions" / "Workspace-A" / "Session-Oversized" / "agents" / "main" / "wire.jsonl";
    native_session(oversized.parent_path().parent_path().parent_path());
    write(oversized, "{\"type\":\"content.part\",\"text\":\"" + std::string(5 * 1000 * 1000, 'x') + "\"}\n");
    bool rejected = false;
    try { (void)selected.scan(profile, "repair", context); }
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

int wire_envelope_probe()
{
    // Investigation only: the native llm.tools_snapshot schema permits arbitrary
    // descriptions and parameter values. These generated extremes are not an
    // observation of an installed producer's output.
    const addon::Json_parse_limits limits;
    const std::string long_text((std::size_t)limits.max_string_bytes + 1, 'x');
    const std::string large_text((std::size_t)limits.max_bytes + 1, 'x');
    const auto depth = limits.max_depth + 8;
    const auto nested = std::string(depth, '[') + "0" + std::string(depth, ']');
    const auto snapshot = [](
            std::string_view description,
            const std::string& parameters,
            bool type_last = false)
        {
            const std::string type = "\"type\":\"llm.tools_snapshot\"";
            std::string record = "{";
            if (!type_last) {
                record += type + ',';
            }
            record += "\"agentId\":\"main\",\"hash\":\"probe-tools\",\"tools\":[{"
                "\"name\":\"probe\",\"description\":\"" + std::string(description) +
                "\",\"parameters\":" + parameters + "}],\"time\":1789200000500";
            if (type_last) {
                record += ',' + type;
            }
            return record + "}\n";
        };
    const std::string metadata =
        "{\"type\":\"metadata\",\"protocol_version\":\"1.5\",\"created_at\":1789199999000}\n";
    const std::string before =
        "{\"type\":\"usage.record\",\"agentId\":\"main\",\"time\":1789200000000,"
        "\"usage\":{\"output\":7}}\n";
    const std::string after =
        "{\"type\":\"usage.record\",\"agentId\":\"main\",\"time\":1789200001000,"
        "\"usage\":{\"output\":11}}\n";
    for (const auto& [name, record] : std::vector<std::pair<std::string, std::string>>{
            {"small-irrelevant", snapshot("ordinary description", "{}")},
            {"string-bound", snapshot(long_text, "{}")},
            {"document-bound", snapshot(large_text, "{}")},
            {"document-bound-type-last", snapshot(large_text, "{}", true)},
            {"depth-bound-type-last", snapshot("ordinary description", "{\"nested\":" + nested + "}", true)}})
    {
        auto fixture_limits = limits;
        fixture_limits.max_bytes        = record.size();
        fixture_limits.max_string_bytes = record.size();
        fixture_limits.max_depth        = depth + 5;
        const auto fixture = addon::parse_json(record, fixture_limits, nullptr);
        require(fixture && json_util::text(*fixture, "type") == "llm.tools_snapshot",
            "probe record must be valid nonusage JSON independent of the production parser bound");

        Workspace workspace;
        const auto profile = workspace.path / "profile";
        const auto storage = workspace.path / "storage";
        const auto session = profile / "sessions/workspace/probed-session";
        const auto wire    = session / "agents/main/wire.jsonl";
        fs::create_directories(storage);
        native_session(session);
        write(wire, metadata + before);
        Selected_cache selected(storage);
        Context context;
        const auto baseline = selected.scan(profile, "incremental", context);
        require(baseline.size() == 1 && baseline.front().total_tokens == 7,
            "native metadata and usage baseline must be admitted before probing limits");
        const auto accepted = selected.path();
        const auto retained = lines_of(accepted);
        write(wire, metadata + before + record + after);
        std::optional<std::vector<addon::Usage_record>> observed;
        try {
            observed = selected.scan(profile, "incremental", context);
        }
        catch (const std::runtime_error& error) {
            require(selected.path() == accepted && lines_of(accepted) == retained,
                "a failed probe must preserve the previously accepted cache bytes");
            std::cout << name << ": " << error.what() << ", accepted_total_tokens=7 retained\n";
        }
        if (observed) {
            require(observed->size() == 1 && observed->front().total_tokens == 18,
                "a successful probe must retain both required usage records");
            std::cout << name << ": success, total_tokens=18\n";
        }
    }
    return 0;
}

void line_cache_contract()
{
    // Current cache records and retained opaque metadata are streamed.
    // Keeping the metadata must not alter accepted file or occurrence evidence.
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    Selected_cache selected(storage);
    fs::create_directories(storage);
    for (unsigned session = 0; session < 3; ++session) {
        native_session(profile / "sessions" / "Workspace-A" / ("Session-" + std::to_string(session)));
        write(profile / "sessions" / "Workspace-A" / ("Session-" + std::to_string(session)) / "agents" / "main" / "wire.jsonl",
            "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"output\":" + std::to_string(20 + session) + "}}\n");
    }
    Context context;
    const auto total = [&](const char* mode) {
        std::uint64_t sum = 0;
        for (const auto& record : selected.scan(profile, mode, context)) sum += record.total_tokens;
        return sum;
    };
    require(total("incremental") == 63, "first scan counts every log");
    require(lines_of(selected.path()).size() == 7, "the cache streams each file and its retained occurrence separately");

    const auto original = lines_of(selected.path());
    std::string retained = original.front() + '\n';
    for (unsigned index = 0; index < 1024; ++index) {
        const auto receipt = addon::to_hex(addon::Sha256::digest("receipt-" + std::to_string(index)));
        retained += addon::serialize_json(json_util::object({{"receipt", addon::Json_value(receipt)}})) + '\n';
    }
    for (std::size_t index = 1; index < original.size(); ++index) retained += original[index] + '\n';
    write(selected.path(), retained);
    selected.reseal();
    fs::remove_all(profile / "sessions");
    require(total("incremental") == 63, "current cache retains exact totals without source logs");
    require(lines_of(selected.path()).size() == 1 + 1024 + 6,
        "current cache preserves every retained metadata row, file and occurrence");
}

void file_count_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    Selected_cache selected(storage);
    fs::create_directories(profile);
    fs::create_directories(storage);
    const auto retained_root = storage / "retained";
    require(fs::create_directory(retained_root), "allocate retained-file fixture");
    const auto cache = retained_root / "files.json";
    constexpr unsigned k_file_count = 100001;
    {
        std::ofstream stream(cache, std::ios::binary);
        stream << "{\"version\":1,\"profile\":" << addon::serialize_json(addon::Json_value(kimi::account_key(profile))) <<
            ",\"format\":\"jsonl\"}\n";
        for (unsigned index = 0; index < k_file_count; ++index) {
            const auto path = profile / "sessions" / "workspace" / "session" / "agents" /
                ("agent-" + std::to_string(index)) / "wire.jsonl";
            stream << addon::serialize_json(json_util::object({
                {"path", addon::Json_value(kimi::path_string(path))}, {"size", addon::Json_value(0)},
                {"modified_msecs", addon::Json_value(0)}, {"recorded_cwd", addon::Json_value("")},
                {"stale_breakdown", addon::Json_value(false)}, {"usage", addon::Json_value{}}})) << '\n';
        }
        stream.close();
        require(static_cast<bool>(stream), "large retained-file fixture written");
    }
    selected.accept(retained_root);
    Context context;
    require(selected.scan(profile, "incremental", context).empty(),
        "more than 100000 retained wire files are accepted without inventing usage");
    std::ifstream stream(selected.path(), std::ios::binary);
    std::size_t rows = 0;
    std::string line;
    while (std::getline(stream, line)) ++rows;
    require(stream.eof() && rows == 1 + k_file_count,
        "current streamed cache retains every file past the former history count limit");
}

void live_file_count_contract()
{
    Workspace workspace;
    const auto profile = workspace.path / ".kimi-code";
    const auto storage = workspace.path / "package-profile";
    Selected_cache selected(storage);
    const auto agents  = profile / "sessions" / "workspace" / "session" / "agents";
    native_session(agents.parent_path());
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
    const auto records = selected.scan(profile, "incremental", context);
    require(records.size() == 1 && records.front().total_tokens == 7,
        "a live history beyond 100000 files includes every agent's usage");
    require(context.progress_current == k_file_count && context.progress_total == k_file_count,
        "large live histories report progress through the complete file inventory");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--observe-history") {
            return observe_history(addon::path_from_utf8(argv[2]));
        }
        if (argc == 2) {
            const std::string_view selected = argv[1];
            if (selected == "--wire-envelope-probe") {
                return wire_envelope_probe();
            }
            if (selected == "--large-data") {
                large_history_contract();
                large_log_contract();
                file_count_contract();
                live_file_count_contract();
                return 0;
            }
            if (selected == "--fork-ownership") { fork_ownership_contract(); fork_chains_contract(); raw_fork_mode_contract(); return 0; }
            if (selected == "--line-cache" || selected == "--history-limits") line_cache_contract();
            if (selected == "--file-count" || selected == "--history-limits") file_count_contract();
            if (selected == "--live-file-count" || selected == "--history-limits") live_file_count_contract();
            require(selected == "--line-cache" || selected == "--file-count" ||
                selected == "--live-file-count" || selected == "--history-limits",
                "unknown usage test selection");
            return 0;
        }
        fork_ownership_contract();
        fork_chains_contract();
        raw_fork_mode_contract();
        legacy_metadata_contract();
        accounts_contract();
        scan_contract();
        timestamp_cache_identity_contract();
        missing_sessions_contract();
        line_cache_contract();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
