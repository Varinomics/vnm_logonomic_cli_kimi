#include "kimi_accounts.h"
#include "kimi_usage.h"

#include <logonomic_cli_addon_test/invocation_context.h>
#include <logonomic_cli_addon_test/usage_scan.h>

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/usage_session.h>
#include <logonomic_cli_addon/usage_temporal.h>
#include <logonomic_cli_addon/usage_owned_event_correction.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/stored_usage_accounting.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace test = addon::test;
namespace fs = std::filesystem;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

using Cancelled_context = test::Cancelled_scan_context;

class Workspace
{
public:
    Workspace()
    :
        m_path(fs::temp_directory_path() / ("kimi-cache-contract-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_path);
        m_path = fs::canonical(m_path);
    }

    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    fs::path m_path;
};

void write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(output.good(), "local telemetry fixture write failed");
}

void native_session(const fs::path& session, const fs::path& cwd = {})
{
    auto state = addon::Json_value::object();
    state.set("id", addon::Json_value(addon::path_to_utf8(session.filename())));
    state.set("version", addon::Json_value(2));
    state.set("createdAt", addon::Json_value(std::int64_t{1789199999000}));
    state.set("updatedAt", addon::Json_value(std::int64_t{1789200000000}));
    state.set("archived", addon::Json_value(false));
    state.set("agents", addon::Json_value::object());
    state.set("custom", addon::Json_value::object());
    if (!cwd.empty()) state.set("cwd", addon::Json_value(addon::path_to_utf8(cwd)));
    write(session / "state.json", addon::serialize_json(state));
}

std::vector<fs::path> write_sources(const fs::path& profile, unsigned tokens)
{

    std::vector<fs::path> paths;
    for (unsigned index = 0; index < 270; ++index) {
        const auto path = profile / "sessions/workspace" / ("session-" + std::to_string(index)) /
            "agents/agent/wire.jsonl";
        native_session(path.parent_path().parent_path().parent_path());
        write(path, "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"inputOther\":" +
            std::to_string(tokens) + ",\"output\":0,\"inputCacheRead\":0,\"inputCacheCreation\":0}}\n");
        paths.push_back(path);
    }
    return paths;
}

void adapter_cache_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "native-profile";
    auto sources = write_sources(profile, 10);
    const auto native_profile = addon::path_to_utf8(profile);
    const auto account = kimi::account_key(profile);

    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "host-storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.kimi", &error, account);
    check(allocated.has_value(), error);
    const auto storage = addon::path_from_utf8(*allocated);

    addon::Usage_scan_request request;
    request.context.launcher = {"kimi", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"kimi"};
    request.context.profile_storage = addon::Profile_storage_context{1, *allocated};
    request.context.account_context = addon::Account_context{2, account, native_profile};
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(),
        addon::Environment_bootstrap_evidence{native_profile}};

    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = request.context.profile_storage;
    const auto acknowledge = host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledge, 6000);
    check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
        "packaged adapter acknowledges allocated storage");

    const auto select = [&](std::string name, std::optional<addon::Usage_cache_reference> accepted) {
        request.snapshot_id.reset();
        request.cursor.reset();
        request.mode = "incremental";
        const auto path = storage / name;
        check(fs::create_directory(path), "host exclusively allocates candidate directory");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(path)};
    };
    const auto validate = [&](const addon::Usage_cache_reference& reference) {
        check(addon::validate_usage_cache_candidate(reference, *allocated, request.context.account_context,
            [] { return false; }, &failure).has_value(), "sealed candidate remains intact: " + failure.detail);
    };

    const auto check_session_facts = [&](const addon::Usage_scan_page& page) {
        std::vector<addon::Usage_session_identity> identities;
        for (std::size_t index = 0; index < std::min(page.records.size(), addon::k_max_usage_page_records); ++index) {
            const auto& record = page.records[index];
            check(!record.capabilities.find(addon::k_usage_session_capability_id),
                "ordinary scan records remain unchanged for hosts without session-fact support");
            identities.push_back({record.record_id, record.session_key});
        }
        const auto id = host.invoke({addon::k_usage_session_capability_id, addon::k_usage_session_version},
            addon::k_usage_session_describe_operation, addon::encode_usage_session_request(identities), 5000);
        const auto outcome = host.wait_outcome(id, 6000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS, "packaged adapter describes usage sessions");
        const auto descriptions = addon::decode_usage_session_result(outcome->payload, &failure);
        check(descriptions && descriptions->size() == identities.size(), "session facts preserve page cardinality");
        for (std::size_t index = 0; index < identities.size(); ++index) {
            const auto& description = descriptions->at(index);
            check(description.identity == identities[index] && description.session &&
                description.session->provider_session_id == identities[index].record_id.substr(5) &&
                !description.session->excludes_turn_in_flight,
                "Kimi facts join prefixed usage to native lifecycle IDs without inventing a completed-turn cutoff");
        }
    };

    for (const auto& path : sources) {
        check(fs::remove(path), "remove fixture source");
    }
    select("empty-selection", std::nullopt);
    const auto empty = test::usage_scan_pages(host, request, 3);
    check(empty.records.empty() && empty.cache_candidate,
        "a new selected cache without live sources has no retained history");
    validate(*empty.cache_candidate);

    sources = write_sources(profile, 10);
    select("accepted", std::nullopt);
    const auto first_request = request;
    const auto first = test::usage_scan_page(host, request, 3);
    check(!first.complete && first.records.size() == 256 && first.cache_candidate,
        "packaged first page supplies a sealed bounded candidate");
    validate(*first.cache_candidate);
    check_session_facts(first);
    request.snapshot_id = first.snapshot_id;
    request.cursor = first.next_cursor;
    auto changed = request;
    changed.mode = "repair";
    check(test::usage_scan_outcome(host, changed, 3).status != addon::Host_status::SUCCESS,
        "continuation cannot change the frozen request");
    const auto last = test::usage_scan_page(host, request, 3);
    check(last.complete && last.records.size() == 14 && last.cache_candidate == first.cache_candidate,
        "packaged final page preserves candidate identity");
    for (const auto& record : first.records) {
        check(record.aggregate_evidence == "partial", "surviving native subsets make conservative evidence claims");
    }

    select("account-v1", std::nullopt);
    request.context.account_context->version = 1;
    request.context.account_context->profile_path.reset();
    // Version one has no profile locator; acknowledgment support must not
    // bypass the native provider's existing explicit-account admission.
    const auto version_one = test::usage_scan_outcome(host, request, 3);
    check(version_one.status != addon::Host_status::SUCCESS &&
        version_one.reason_code == "kimi_operation_failed",
        "version-one account without a profile locator retains its explicit refusal");
    request.context = first_request.context;
    validate(*first.cache_candidate);

    select("account-absent", std::nullopt);
    request.context.account_context.reset();
    request.context.launch_environment->effective_environment.set("KIMI_CODE_HOME", addon::Json_value(native_profile));
    const auto account_absent = test::usage_scan_pages(host, request, 3);
    check(account_absent.records.size() == 270 && test::usage_token_total(account_absent) == 2700,
        "environment-selected profile keeps account context absent on every page");
    request.context = first_request.context;

    sources = write_sources(profile, 1);
    select("unaccepted", first.cache_candidate);
    request.mode = "repair";
    const auto lower = test::usage_scan_pages(host, request, 3);
    check(test::usage_token_total(lower) == 270 && lower.cache_candidate,
        "lower native observation is a separate candidate");
    validate(*first.cache_candidate);
    for (const auto& path : sources) {
        check(fs::remove(path), "remove refreshed fixture source");
    }

    // The host rejects the lower partial aggregate and continues selecting the coherent original cache.
    select("recovered", first.cache_candidate);
    const auto recovered = test::usage_scan_pages(host, request, 3);
    check(test::usage_token_total(recovered) == 2700,
        "rejected candidate cannot become retained history after source deletion");
    check_session_facts(recovered);
    validate(*first.cache_candidate);
    validate(*lower.cache_candidate);

    select("cancelled", first.cache_candidate);
    Cancelled_context cancelled(request);
    bool saw_cancel = false;
    try {
        kimi::handle_usage(cancelled);
    }
    catch (const std::runtime_error& error) {
        saw_cancel = std::string(error.what()) == "cancelled";
    }
    check(saw_cancel, "cancelled scan cannot seal or promote a candidate");
    check(!fs::exists(addon::path_from_utf8(request.cache_selection->candidate_root) / "cache-manifest.json"),
        "cancelled candidate stays unsealed");
    validate(*first.cache_candidate);

    select("reset", std::nullopt);
    request.mode = "reset";
    const auto reset = test::usage_scan_pages(host, request, 3);
    check(reset.records.empty() && reset.cache_candidate, "reset reconstructs only current sources");
    validate(*reset.cache_candidate);
    validate(*first.cache_candidate);
    auto invalid_reset = request;
    invalid_reset.cache_selection->accepted = first.cache_candidate;
    check(test::usage_scan_outcome(host, invalid_reset, 3).status != addon::Host_status::SUCCESS,
        "reset cannot import a retained selection");

    select("occupied", first.cache_candidate);
    write(addon::path_from_utf8(request.cache_selection->candidate_root) / "foreign", "occupied");
    check(test::usage_scan_outcome(host, request, 3).status != addon::Host_status::SUCCESS,
        "provider refuses a nonempty host allocation");
    validate(*first.cache_candidate);

    select("wrong-account", first.cache_candidate);
    auto wrong_account = request;
    const auto other_profile = fixture / "other-native-profile";
    fs::create_directories(other_profile);
    wrong_account.context.account_context = addon::Account_context{
        2, kimi::account_key(other_profile), addon::path_to_utf8(other_profile)};
    wrong_account.context.launch_environment->bootstrap =
        addon::Environment_bootstrap_evidence{addon::path_to_utf8(other_profile)};
    check(test::usage_scan_outcome(host, wrong_account, 3).status != addon::Host_status::SUCCESS,
        "selected cache cannot cross native profile and account ownership");
    validate(*first.cache_candidate);

    select("usage-host.tmp", std::nullopt);
    check(test::usage_scan_outcome(host, request, 3).status != addon::Host_status::SUCCESS,
        "host allocation cannot alias a live scan workspace namespace");

    select("corrupt-input", first.cache_candidate);
    write(addon::path_from_utf8(first.cache_candidate->root) / "unlisted", "unexpected");
    check(test::usage_scan_outcome(host, request, 3).status != addon::Host_status::SUCCESS,
        "corrupt selected inventory fails without fallback");

    // The Stop button cancels a scan that is reading history; the host must
    // see that as cancelled, not as a failed scan. The small log's progress
    // shows the scan under way, and the large log keeps it reading far longer
    // than the cancel takes to arrive.
    const std::string usage_line =
        "{\"type\":\"usage.record\",\"time\":1789200000000,\"usage\":{\"inputOther\":1,"
        "\"output\":0,\"inputCacheRead\":0,\"inputCacheCreation\":0}}\n";
    std::string large_log;
    while (large_log.size() < 64 * 1024 * 1024) {
        large_log += usage_line;
    }
    native_session(profile / "sessions/workspace/before-cancel");
    native_session(profile / "sessions/workspace/during-cancel");
    write(profile / "sessions/workspace/before-cancel/agents/agent/wire.jsonl", usage_line);
    write(profile / "sessions/workspace/during-cancel/agents/agent/wire.jsonl", large_log);
    select("cancelled-while-reading", std::nullopt);
    const auto reading = host.invoke({"usage-scan", 3}, "scan", addon::encode_usage_scan_request(request, 3), 30000);
    std::optional<addon::Host_outcome> stopped;
    while (!stopped && host.progress(reading).empty()) {
        stopped = host.wait_outcome(reading, 10);
    }
    if (!stopped) {
        host.cancel(reading);
        stopped = host.wait_outcome(reading, 31000);
    }
    check(stopped.has_value(), "cancelled packaged scan must finish");
    check(stopped->status == addon::Host_status::CANCELLED,
        std::string("a scan cancelled while it reads reports cancelled, not ") +
            addon::host_status_name(stopped->status) + " (" + stopped->reason_code + ")");

    select("concurrent-first", std::nullopt);
    request.mode = "repair";
    const auto first_scan = host.invoke({"usage-scan", 3}, "scan",
        addon::encode_usage_scan_request(request, 3), 120000);
    const auto generation = host.generation_nonce();
    while (host.progress(first_scan).empty() && !host.outcome(first_scan)) {
        host.pump(10);
    }
    check(!host.progress(first_scan).empty() && !host.outcome(first_scan),
        "first account is scanning while a second account arrives");

    auto waiter = request;
    waiter.context.account_context = addon::Account_context{
        2, kimi::account_key(other_profile), addon::path_to_utf8(other_profile)};
    waiter.context.launch_environment->bootstrap =
        addon::Environment_bootstrap_evidence{addon::path_to_utf8(other_profile)};
    const auto waiter_root = storage / "concurrent-waiter";
    check(fs::create_directory(waiter_root), "host allocates second account candidate");
    waiter.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(waiter_root)};
    const auto waiting_scan = host.invoke({"usage-scan", 3}, "scan",
        addon::encode_usage_scan_request(waiter, 3), 120000);
    host.pump(100);
    check(!host.outcome(waiting_scan) && !host.outcome(first_scan),
        "second account waits while the first account scans");
    host.cancel(waiting_scan);
    const auto waiting_outcome = host.wait_outcome(waiting_scan, 1500);
    check(waiting_outcome && waiting_outcome->status == addon::Host_status::CANCELLED,
        "cancelled second account must settle before the generation's cancellation grace");
    check(!host.outcome(first_scan) && host.generation_nonce() == generation,
        "first account and its adapter generation remain live after waiter cancellation");
    check(!fs::exists(waiter_root / "cache-manifest.json"),
        "cancelled waiter cannot seal a cache candidate");

    const auto first_outcome = host.wait_outcome(first_scan, 120000);
    check(first_outcome && first_outcome->status == addon::Host_status::SUCCESS,
        "first account finishes after the second account cancels");
    const auto first_page = addon::decode_usage_scan_result(first_outcome->payload, &failure, 3);
    check(first_page && first_page->cache_candidate, "first account seals its candidate");
    validate(*first_page->cache_candidate);

    const auto next_root = storage / "concurrent-next";
    check(fs::create_directory(next_root), "host allocates later second-account candidate");
    waiter.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(next_root)};
    const auto next_outcome = test::usage_scan_outcome(host, waiter, 3);
    check(next_outcome.status == addon::Host_status::SUCCESS && host.generation_nonce() == generation,
        "second account can scan on the surviving generation");
    check(host.shutdown(1000, nullptr), "packaged adapter exits without contacting a model");
}

class Temporal_host final
{
public:
    Temporal_host(const fs::path& package, const fs::path& fixture)
        : profile(fixture / "profile"), host(options(package, fixture), nullptr)
    {
        fs::create_directories(profile);
        addon::Contract_failure failure;
        check(host.start(&failure), failure.detail);
        std::string error;
        const auto allocated = addon::allocate_profile_storage_root(host.profile_storage_root(),
            "logonomic.cli.kimi", &error, kimi::account_key(profile));
        check(allocated.has_value(), error);
        storage = addon::path_from_utf8(*allocated);
        request.context.launcher = {"kimi", addon::k_runner_direct, {}};
        request.context.launcher_argv = {"kimi"};
        request.context.profile_storage = addon::Profile_storage_context{1, *allocated};
        request.context.account_context = addon::Account_context{2, kimi::account_key(profile),
            addon::path_to_utf8(profile)};
        request.context.launch_environment = addon::Launch_environment{addon::host_platform_id(),
            addon::Json_value::object(), addon::Environment_bootstrap_evidence{addon::path_to_utf8(profile)}};
        addon::Simulator_invocation_options invocation;
        invocation.profile_storage = request.context.profile_storage;
        const auto id = host.invoke({"profile-storage", 1}, "acknowledge", addon::Json_value::object(), 5000, invocation);
        const auto outcome = host.wait_outcome(id, 6000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS, "temporal host storage acknowledged");
    }

    ~Temporal_host() { host.shutdown(1000, nullptr); }

    void select(const std::string& name, std::optional<addon::Usage_cache_reference> accepted = std::nullopt)
    {
        request.snapshot_id.reset();
        request.cursor.reset();
        request.mode = "incremental";
        const auto root = storage / name;
        check(fs::create_directory(root), "allocate temporal candidate");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(root)};
    }

    addon::Usage_scan_page scan(int deadline_ms = 30000)
    {
        auto input = request;
        addon::Usage_scan_page result;
        bool first = true;
        page_count = 0;
        do {
            const auto id = host.invoke({"usage-scan", 3}, "scan", addon::encode_usage_scan_request(input, 3), deadline_ms);
            const auto outcome = host.wait_outcome(id, deadline_ms + 1000);
            check(outcome && outcome->status == addon::Host_status::SUCCESS,
                "temporal scan succeeds: " + (outcome ? outcome->reason_code : "timeout"));
            addon::Contract_failure failure;
            const auto page = addon::decode_usage_scan_result(outcome->payload, &failure, 3);
            check(page.has_value(), "temporal page decodes: " + failure.detail);
            check(addon::serialize_json(outcome->payload).size() < 4 * 1024 * 1024 - 65536,
                "original Kimi page byte ceiling preserved");
            if (first) {
                result = *page;
                first = false;
            }
            else {
                check(page->snapshot_id == result.snapshot_id && page->cache_candidate == result.cache_candidate,
                    "temporal continuation preserves exact snapshot and candidate");
                result.records.insert(result.records.end(), page->records.begin(), page->records.end());
            }
            ++page_count;
            if (page->complete) break;
            input.snapshot_id = page->snapshot_id;
            input.cursor = page->next_cursor;
        } while (true);
        return result;
    }

    addon::Usage_cache_reference sealed(const std::string& name, const std::vector<addon::Json_value>& rows)
    {
        const auto root = storage / name;
        check(fs::create_directory(root), "allocate cache mutation fixture");
        std::string bytes;
        for (const auto& row : rows) bytes += addon::serialize_json(row) + '\n';
        write(root / "files.json", bytes);
        addon::Contract_failure failure;
        const auto reference = addon::seal_usage_cache_candidate(addon::path_to_utf8(root),
            request.context.profile_storage->root, request.context.account_context, "fixture-" + name,
            "kimi-wire-cache@2", [] { return false; }, &failure);
        check(reference.has_value(), "seal original-account cache fixture: " + failure.detail);
        return *reference;
    }

    fs::path profile;
    fs::path storage;
    addon::Usage_scan_request request;
    std::size_t page_count = 0;

private:
    static addon::Simulator_options options(const fs::path& package, const fs::path& fixture)
    {
        addon::Simulator_options result;
        result.package_directory = package;
        result.profile_storage_root = fixture / "storage";
        return result;
    }
    addon::Host_simulator host;
};

std::string native_event(std::uint64_t time, unsigned input, unsigned output = 0, unsigned read = 0, unsigned create = 0)
{
    return "{\"type\":\"usage.record\",\"time\":" + std::to_string(time) +
        ",\"usage\":{\"inputOther\":" + std::to_string(input) + ",\"output\":" + std::to_string(output) +
        ",\"inputCacheRead\":" + std::to_string(read) + ",\"inputCacheCreation\":" + std::to_string(create) + "}}\n";
}

std::vector<addon::Json_value> cache_rows(const addon::Usage_cache_reference& reference)
{
    std::ifstream input(addon::path_from_utf8(reference.root) / "files.json", std::ios::binary);
    check(static_cast<bool>(input), "open sealed temporal fixture");
    std::vector<addon::Json_value> rows;
    std::string line;
    while (std::getline(input, line)) {
        const auto parsed = addon::parse_json(line, {}, nullptr);
        check(parsed.has_value(), "read exact cache row");
        rows.push_back(*parsed);
    }
    return rows;
}

std::map<std::string, std::uint64_t> proved(const addon::Usage_record& record, const addon::Usage_scan_page& page,
    const Temporal_host& fixture, std::uint64_t unavailable)
{
    check(record.temporal.has_value(), "valid accounting parent has temporal envelope");
    const auto& evidence = *record.temporal;
    check(evidence.parent.record_id == record.record_id && evidence.parent.total_tokens == record.total_tokens &&
        evidence.parent.snapshot_id == page.snapshot_id &&
        evidence.parent.source_account_key == fixture.request.context.account_context->adapter_account_key,
        "temporal proof binds exact admitted ID, H, snapshot and source account");
    std::uint64_t total = evidence.unavailable_tokens;
    std::map<std::string, std::uint64_t> positions;
    for (const auto& portion : evidence.portions) {
        check(portion.basis == "instant" && portion.total_tokens > 0 && portion.date.empty() && portion.calendar.empty(),
            "only genuinely positive native instants are emitted");
        total += portion.total_tokens;
        positions[portion.instant] += portion.total_tokens;
    }
    check(total == record.total_tokens && evidence.unavailable_tokens == unavailable,
        "dated and unavailable portions conserve exact accounting H");
    return positions;
}

void temporal_cache_contract(const fs::path& package, const fs::path& fixture)
{
    Temporal_host host(package, fixture);
    const auto session = host.profile / "sessions/workspace/dated-session";
    const auto source_a = session / "agents/a/wire.jsonl";
    const auto source_b = session / "agents/b/wire.jsonl";
    const auto repository_a = fixture / "repository-a";
    const auto repository_b = fixture / "repository-b";
    fs::create_directories(repository_a / ".git");
    fs::create_directories(repository_b / ".git");
    const auto cwd = [&](const fs::path& path) {
        native_session(session, path);
    };
    cwd(repository_a);
    constexpr std::uint64_t early = 1791503999999ull;
    constexpr std::uint64_t late = 1791594000001ull;
    const auto repeated = native_event(early, 10, 4, 2, 3);
    write(source_a, repeated + repeated + native_event(late, 7, 5) + "{\"type\":\"other\"}\n" + native_event(late, 999).substr(0, native_event(late, 999).size() - 1));
    auto unsupported = native_event(late, 2);
    unsupported.insert(unsupported.find(",\"usage\""), ".0");
    write(source_b, native_event(early, 2) + native_event(0, 3) + unsupported + native_event(late + 1, 0));
    host.select("observed");
    const auto observed = host.scan();
    check(observed.records.size() == 1 && observed.cache_candidate, "two native agent files reduce to one original conversation");
    const auto& record = observed.records.front();
    check(record.record_id == "kimi:dated-session" && record.total_tokens == 57 && record.input.value == 34 &&
        record.output.value == 13 && record.cache_read.value == 4 && record.cache_creation.value == 6 &&
        record.repository_usage &&
        record.repository_usage->allocations.front().repository_path == addon::path_to_utf8(repository_a),
        "native categories, conversation and repository accounting stay exact");
    const std::map<std::string, std::uint64_t> expected{{"1970-01-01T00:00:00.000Z", 3},
        {"2026-10-08T23:59:59.999Z", 40}, {"2026-10-10T01:00:00.001Z", 12}};
    check(proved(record, observed, host, 2) == expected && record.completed_date > "2026-10-08",
        "raw epoch milliseconds survive reduction despite latest date outside an earlier day");
    check(!record.temporal->retained_from, "fresh native proof has no retained custody claim");

    cwd(repository_b);
    host.select("rebound", observed.cache_candidate);
    const auto rebound = host.scan();
    check(proved(rebound.records.front(), rebound, host, 2) == expected &&
        rebound.records.front().repository_usage->allocations.front().repository_path == addon::path_to_utf8(repository_b),
        "original canonical digest validates before unchanged-fingerprint cwd and repository rebinding");
    check(rebound.records.front().temporal->retained_from &&
        rebound.records.front().temporal->retained_from->snapshot_id == observed.cache_candidate->snapshot_id &&
        rebound.records.front().temporal->retained_from->manifest_sha256_hex == observed.cache_candidate->manifest_sha256_hex,
        "cache proof names exact selected snapshot and manifest custody");
    check(fs::remove(source_a) && fs::remove(source_b), "delete originals after accepted proof");
    host.select("retained", rebound.cache_candidate);
    const auto retained = host.scan();
    check(proved(retained.records.front(), retained, host, 2) == expected &&
        retained.records.front().temporal->retained_from->snapshot_id == rebound.cache_candidate->snapshot_id &&
        retained.records.front().temporal->retained_from->manifest_sha256_hex == rebound.cache_candidate->manifest_sha256_hex,
        "deleted originals preserve history under exact current accepted custody");
    const auto original_rows = cache_rows(*rebound.cache_candidate);
    const auto replay = [&](const std::string& name, const std::vector<addon::Json_value>& rows) {
        const auto accepted = host.sealed(name + "-input", rows);
        host.select(name + "-output", accepted);
        const auto result = host.scan();
        check(result.records.size() == 1 && result.cache_candidate, "optional carrier damage preserves accounting and sealed cache");
        addon::Contract_failure failure;
        check(addon::validate_usage_cache_candidate(accepted, host.request.context.profile_storage->root,
            host.request.context.account_context, [] { return false; }, &failure).has_value(),
            "accepted mutation fixture remains immutable");
        return result;
    };
    for (const auto& kind : {"absent", "unknown", "damaged", "digest", "oversize", "native-shape", "parent-account", "parent-snapshot"}) {
        auto rows = original_rows;
        for (auto& row : rows) {
            if (!row.find("usage")) continue;
            if (std::string(kind) == "absent") row.erase("usage-temporal");
            else if (std::string(kind) == "damaged") row.set("usage-temporal", addon::Json_value("damaged"));
            else {
                auto carrier = *row.find("usage-temporal");
                if (std::string(kind) == "unknown") carrier.set("version", addon::Json_value(99));
                if (std::string(kind) == "digest") carrier.set("parent_sha256_hex", addon::Json_value(std::string(64, '0')));
                if (std::string(kind) == "oversize") carrier.set("opaque", addon::Json_value(std::string(addon::k_max_usage_temporal_bytes + 1, 'x')));
                if (std::string(kind) == "native-shape" || std::string(kind) == "parent-account" ||
                    std::string(kind) == "parent-snapshot")
                {
                    auto evidence = addon::decode_usage_temporal(*carrier.find("evidence"), nullptr);
                    if (std::string(kind) == "native-shape") evidence->portions.front().instant = "2026-10-08T23:59:59.999000001Z";
                    if (std::string(kind) == "parent-account") evidence->parent.source_account_key = "foreign-account";
                    if (std::string(kind) == "parent-snapshot") evidence->parent.snapshot_id = "foreign-snapshot";
                    carrier.set("evidence", addon::encode_usage_temporal(*evidence));
                }
                row.set("usage-temporal", std::move(carrier));
            }
        }
        const auto result = replay(kind, rows);
        check(result.records.front().total_tokens == 57 && proved(result.records.front(), result, host, 57).empty(),
            std::string(kind) + " optional evidence withdraws locally, preserving H and older cache usability");
        if (std::string(kind) == "unknown") {
            const auto replayed = cache_rows(*result.cache_candidate);
            for (const auto& row : replayed) if (row.find("usage")) {
                check(row.find("usage-temporal") && row.find("usage-temporal")->find("version")->as_integer() == 99,
                    "unknown independently versioned carrier remains opaque during cache replay");
            }
        }
    }
    auto corrected_rows = original_rows;
    for (auto& row : corrected_rows) {
        if (!row.find("usage")) continue;
        auto accounting = addon::decode_stored_usage_accounting(*row.find("usage"), nullptr);
        check(accounting.has_value(), "original canonical accounting fixture decodes");
        if (accounting->total_tokens == 50) {
            ++accounting->total_tokens;
            ++*accounting->input.value;
            ++accounting->repository_usage->allocations.front().total_tokens;
            row.set("usage", addon::encode_stored_usage_accounting(*accounting));
        }
    }
    const auto corrected = replay("changed-accounting", corrected_rows);
    const std::map<std::string, std::uint64_t> remaining{{"1970-01-01T00:00:00.000Z", 3}, {"2026-10-08T23:59:59.999Z", 2}};
    check(corrected.records.front().total_tokens == 58 && proved(corrected.records.front(), corrected, host, 53) == remaining,
        "one changed canonical parent digest withdraws only that file's proof, never its accounting");

    auto control_rows = original_rows;
    const std::string control_id = "kimi:control-\x01-parent";
    for (auto& row : control_rows) if (row.find("usage")) {
        auto accounting = addon::decode_stored_usage_accounting(*row.find("usage"), nullptr);
        accounting->record_id = control_id;
        accounting->session_key = control_id;
        const auto encoded = addon::encode_stored_usage_accounting(*accounting);
        const auto original = addon::decode_stored_usage_accounting(encoded, nullptr);
        check(original && original->record_id == control_id,
            "current stored accounting admits the exact nonNUL control parent ID");
        row.set("usage", encoded);
    }
    const auto control = replay("control-parent", control_rows);
    check(control.records.front().record_id == control_id && control.records.front().total_tokens == 57 &&
        !control.records.front().temporal, "invalid optional parent grammar omits proof while preserving original admitted ID and H");

    auto wrong_snapshot = *rebound.cache_candidate;
    wrong_snapshot.snapshot_id = "different-snapshot";
    host.select("wrong-snapshot", wrong_snapshot);
    const auto id = host.request;
    bool refused = false;
    try { host.scan(); }
    catch (const std::runtime_error&) { refused = true; }
    check(refused, "selected snapshot mismatch cannot receive retained temporal custody");
    check(host.request.cache_selection->accepted == id.cache_selection->accepted, "refusal keeps exact accepted reference");
}

void temporal_refresh_contract(const fs::path& package, const fs::path& fixture)
{
    Temporal_host host(package, fixture);
    const auto source = host.profile / "sessions/workspace/refreshed-session/agents/main/wire.jsonl";
    native_session(source.parent_path().parent_path().parent_path());
    constexpr std::uint64_t early = 1791503999999ull;
    constexpr std::uint64_t late  = 1791594000001ull;
    auto unsupported = native_event(late, 2);
    unsupported.insert(unsupported.find(",\"usage\""), ".0");
    const auto native = native_event(early, 10, 4, 2, 3) + native_event(late, 7, 5) + unsupported;
    write(source, native);
    host.select("observed");
    const auto observed = host.scan();
    check(observed.records.size() == 1 && observed.cache_candidate, "native refresh fixture has one conversation");
    const auto accounting = addon::serialize_json_canonical(addon::encode_stored_usage_accounting(observed.records.front()));
    const auto original_rows = cache_rows(*observed.cache_candidate);
    auto absent_rows = original_rows;
    for (auto& row : absent_rows) row.erase("usage-temporal");
    const auto accepted = host.sealed("absent-input", absent_rows);
    host.select("refreshed", accepted);
    const auto refreshed = host.scan();
    const std::map<std::string, std::uint64_t> expected{
        {"2026-10-08T23:59:59.999Z", 19}, {"2026-10-10T01:00:00.001Z", 12}};
    check(refreshed.records.size() == 1 && refreshed.records.front().total_tokens == 33 &&
        proved(refreshed.records.front(), refreshed, host, 2) == expected,
        "unchanged surviving logs refresh absent temporal evidence from exact native increments");
    check(addon::serialize_json_canonical(addon::encode_stored_usage_accounting(refreshed.records.front())) == accounting &&
        !refreshed.records.front().temporal->retained_from,
        "native temporal enrichment preserves all accounting and does not claim retained proof");
    addon::Contract_failure failure;
    check(addon::validate_usage_cache_candidate(accepted, host.request.context.profile_storage->root,
        host.request.context.account_context, [] { return false; }, &failure).has_value(),
        "refresh leaves the selected accounting cache immutable");

    auto unknown_rows = original_rows;
    for (auto& row : unknown_rows) {
        if (const auto* value = row.find("usage-temporal")) {
            auto carrier = *value;
            carrier.set("version", addon::Json_value(99));
            row.set("usage-temporal", std::move(carrier));
        }
    }
    host.select("unknown-output", host.sealed("unknown-input", unknown_rows));
    const auto unknown = host.scan();
    check(proved(unknown.records.front(), unknown, host, 33).empty(),
        "unrecognized temporal evidence remains capability-local with a surviving source");
    for (const auto& row : cache_rows(*unknown.cache_candidate)) {
        if (row.find("usage")) {
            check(row.find("usage-temporal") && row.find("usage-temporal")->find("version")->as_integer() == 99,
                "unchanged replay preserves opaque unknown temporal evidence");
        }
    }

    const auto modified = fs::last_write_time(source);
    write(source, std::string(native.size() - 1, 'x') + '\n');
    fs::last_write_time(source, modified);
    host.select("invalid-source-output", accepted);
    const auto invalid_source = host.scan();
    check(addon::serialize_json_canonical(addon::encode_stored_usage_accounting(invalid_source.records.front())) == accounting &&
        proved(invalid_source.records.front(), invalid_source, host, 33).empty(),
        "failed optional enrichment preserves same-fingerprint accounting with unavailable temporal evidence");
    host.select("reused", refreshed.cache_candidate);
    const auto reused = host.scan();
    check(proved(reused.records.front(), reused, host, 2) == expected &&
        reused.records.front().temporal->retained_from.has_value(),
        "valid refreshed evidence remains eligible for ordinary fingerprint reuse");
    for (const auto& reason : {"missing-evidence", "capacity"}) {
        auto rows = original_rows;
        for (auto& row : rows) {
            if (const auto* value = row.find("usage-temporal")) {
                auto carrier = *value;
                auto evidence = addon::decode_usage_temporal(*carrier.find("evidence"), nullptr);
                evidence->portions.clear();
                evidence->unavailable_tokens = 33;
                evidence->coverage_reason = reason;
                carrier.set("evidence", addon::encode_usage_temporal(*evidence));
                row.set("usage-temporal", std::move(carrier));
            }
        }
        host.select(std::string(reason) + "-output", host.sealed(std::string(reason) + "-input", rows));
        const auto unavailable = host.scan();
        check(proved(unavailable.records.front(), unavailable, host, 33).empty() &&
            unavailable.records.front().temporal->retained_from.has_value(),
            "valid unavailable evidence remains eligible for unchanged-fingerprint reuse");
    }
    write(source, "invalid changed source\n");
    host.select("changed-source-output", accepted);
    bool refused = false;
    try { host.scan(); }
    catch (const std::runtime_error&) { refused = true; }
    check(refused, "changed native source remains subject to core accounting validation");
}

void temporal_capacity_contract(const fs::path& package, const fs::path& fixture)
{
    Temporal_host host(package, fixture);
    constexpr std::uint64_t start = 1791503999999ull;
    const auto source = [&](const std::string& session, const std::string& agent, unsigned begin, unsigned count, bool zero = false) {
        native_session(host.profile / "sessions/workspace" / session);
        std::string bytes;
        for (unsigned index = begin; index < begin + count; ++index) bytes += native_event(start + index, 1);
        if (zero) bytes += native_event(start + begin + count, 0);
        write(host.profile / "sessions/workspace" / session / "agents" / agent / "wire.jsonl", bytes);
    };
    source("file-overflow", "a", 0, 4097);
    source("merge-overflow", "a", 0, 2048);
    source("merge-overflow", "b", 2048, 2049);
    native_session(host.profile / "sessions/workspace/zero");
    write(host.profile / "sessions/workspace/zero/agents/a/wire.jsonl", native_event(start, 0));
    for (unsigned index = 0; index < 16; ++index) source("bounded-" + std::to_string(index), "a", 0, 4096, true);
    host.select("capacity");
    // The first page seals all ~70,000 events before returning. Give that
    // bounded workload the long-cache budget, within the phase's CTest limit.
    const auto page = host.scan(120000);
    check(page.records.size() == 19 && host.page_count > 1, "optional bytes trigger real bounded continuation before record-count limit");
    std::uint64_t total = 0;
    for (const auto& record : page.records) {
        total += record.total_tokens;
        if (record.record_id == "kimi:zero") {
            check(record.total_tokens == 0 && proved(record, page, host, 0).empty(), "H zero never fabricates a positive portion");
        }
        else if (record.record_id.find("overflow") != std::string::npos) {
            check(record.total_tokens == 4097 && proved(record, page, host, 4097).empty() &&
                record.temporal->coverage_reason == "capacity", "per-file and conversation position overflow withdraw only optional evidence");
        }
        else {
            check(record.total_tokens == 4096 && proved(record, page, host, 0).size() == 4096,
                "maximum positive positions survive and an additional zero event consumes no position");
        }
    }
    check(total == 16 * 4096 + 2 * 4097, "multi-page capacity refusal preserves every original accounting increment");
}

void owned_event_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "native-profile";
    const auto repository = fixture / "repository";
    fs::create_directories(repository / ".git");
    const auto parent = profile / "sessions/workspace/parent";
    const auto child = profile / "sessions/workspace/child";
    const auto zero = profile / "sessions/workspace/zero";
    const auto wire_path = [](const fs::path& session) { return session / "agents/root/wire.jsonl"; };
    const auto usage = [](std::uint64_t tokens, std::uint64_t time, const char* model = "model-a") {
        return "{\"type\":\"usage.record\",\"agentId\":\"root\",\"time\":" + std::to_string(time) +
            ",\"model\":\"" + model + "\",\"usage\":{\"inputOther\":" + std::to_string(tokens) +
            ",\"output\":0,\"inputCacheRead\":0,\"inputCacheCreation\":0}}\n";
    };
    const auto inherited = usage(1200, 1789200000000ull) + usage(0, 1789200000500ull);
    const std::string marker = "{\"type\":\"forked\",\"agentId\":\"root\",\"time\":1789200001000}\n";
    for (const auto& session : {parent, child, zero}) {
        native_session(session, repository / "");
        if (session != parent) {
            std::ifstream input(session / "state.json", std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(input)), {});
            input.close();
            auto state = *addon::parse_json(bytes, {}, nullptr);
            state.set("forkedFrom", addon::Json_value("parent"));
            write(session / "state.json", addon::serialize_json(state));
        }
    }
    write(wire_path(parent), inherited);
    write(wire_path(child), inherited + marker + usage(300, 1789200002000ull));
    write(wire_path(zero), inherited + marker);

    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "host-storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail);
    std::string error;
    const auto account = kimi::account_key(profile);
    const auto allocated = addon::allocate_profile_storage_root(host.profile_storage_root(), "logonomic.cli.kimi", &error, account);
    check(allocated.has_value(), error);
    const auto storage = addon::path_from_utf8(*allocated);
    const auto native_profile = addon::path_to_utf8(profile);
    addon::Usage_scan_request scan;
    scan.context.launcher = {"kimi", addon::k_runner_direct, {}};
    scan.context.launcher_argv = {"kimi"};
    scan.context.profile_storage = addon::Profile_storage_context{1, *allocated};
    scan.context.account_context = addon::Account_context{2, account, native_profile};
    scan.context.launch_environment = addon::Launch_environment{addon::host_platform_id(), addon::Json_value::object(),
        addon::Environment_bootstrap_evidence{native_profile}};
    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = scan.context.profile_storage;
    const auto acknowledged = host.wait_outcome(host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation), 6000);
    check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS, "owned-event storage acknowledgment");

    // These are explicit predecessor projections for the adapter boundary.
    // The host store's real row digest and atomic acceptance have separate tests.
    const auto baseline = [&](const fs::path& session, std::uint64_t tokens, std::uint64_t events) {
        addon::Usage_record record;
        record.record_id = "kimi:" + addon::path_to_utf8(session.filename());
        record.session_key = record.record_id;
        record.source_path = addon::path_to_utf8(session);
        record.model = "model-a";
        record.completed_date = "2026-09-12";
        record.total_tokens = tokens;
        record.counter_field = "usage";
        record.source_counter_semantics = "per_event_sum";
        record.aggregate_evidence = "partial";
        record.input = {true, tokens, addon::Usage_field_coverage{events, events, false, addon::Usage_support::SUPPORTED}};
        for (auto* amount : {&record.output, &record.cache_read, &record.cache_creation}) {
            *amount = {true, 0, addon::Usage_field_coverage{events, events, false, addon::Usage_support::SUPPORTED}};
        }
        record.repository_usage = addon::Usage_repository{{{addon::path_to_utf8(repository), tokens}}, 0};
        addon::Usage_temporal temporal;
        temporal.parent = {record.record_id, tokens, "host-baseline", account};
        temporal.unavailable_tokens = tokens;
        temporal.coverage_reason = "missing-evidence";
        record.temporal = temporal;
        return record;
    };
    std::vector<addon::Usage_record> accepted_records{
        baseline(child, 1500, 3), baseline(parent, 1200, 2), baseline(zero, 1200, 2)};
    const auto raw_records = accepted_records;
    const auto select = [&](const char* name, std::optional<addon::Usage_cache_reference> accepted) {
        const auto candidate = storage / name;
        check(fs::create_directory(candidate), "exclusive owned-event candidate");
        scan.snapshot_id.reset();
        scan.cursor.reset();
        scan.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(candidate)};
    };
    const auto request_for = [&] {
        addon::Usage_owned_event_correction_request request;
        request.scan = scan;
        const addon::Usage_owned_event_predecessor_header header{
            "logonomic.cli.kimi", scan.context.account_context, *allocated, scan.cache_selection->accepted};
        check(addon::decode_usage_owned_event_predecessor_header(
            addon::encode_usage_owned_event_predecessor_header(header), &failure).has_value(), failure.detail);
        std::string bytes = addon::serialize_json(addon::encode_usage_owned_event_predecessor_header(header)) + '\n';
        for (const auto& record : accepted_records) {
            addon::Usage_owned_event_predecessor predecessor;
            predecessor.record = record;
#ifdef _WIN32
            for (auto& allocation : predecessor.record.repository_usage->allocations) {
                std::replace(allocation.repository_path.begin(), allocation.repository_path.end(), '\\', '/');
                if (allocation.repository_path.back() == '/') allocation.repository_path.pop_back();
            }
#endif
            predecessor.predecessor_sha256_hex = addon::to_hex(addon::Sha256::digest(
                addon::serialize_json_canonical(addon::encode_usage_record(predecessor.record, 3))));
            check(addon::decode_usage_owned_event_predecessor(
                addon::encode_usage_owned_event_predecessor(predecessor), &failure).has_value(), failure.detail);
            bytes += addon::serialize_json(addon::encode_usage_owned_event_predecessor(predecessor)) + '\n';
        }
        const auto path = storage / "predecessors.jsonl";
        write(path, bytes);
        request.predecessors = {addon::path_to_utf8(path), addon::to_hex(addon::Sha256::digest(bytes))};
        return request;
    };
    const auto outcome_for = [&](const addon::Usage_owned_event_correction_request& request) {
        const auto id = host.invoke({addon::k_usage_owned_event_correction_capability_id, 1}, "scan",
            addon::encode_usage_owned_event_correction_request(request), 30000, invocation);
        const auto outcome = host.wait_outcome(id, 31000);
        check(outcome.has_value(), "owned-event adapter outcome");
        return *outcome;
    };
    const auto page_for = [&](const addon::Usage_owned_event_correction_request& request) {
        const auto outcome = outcome_for(request);
        check(outcome.status == addon::Host_status::SUCCESS, "owned-event scan " +
            request.scan.cache_selection->candidate_root + ": " + outcome.reason_code);
        const auto page = addon::decode_usage_scan_result(outcome.payload, &failure, 3);
        check(page.has_value(), failure.detail);
        check(page->complete && page->cache_candidate, "owned-event fixture fits one sealed page");
        return *page;
    };
    const auto find = [](const addon::Usage_scan_page& page, const char* id) -> const addon::Usage_record& {
        const auto found = std::find_if(page.records.begin(), page.records.end(), [&](const auto& record) { return record.record_id == id; });
        check(found != page.records.end(), "stable corrected identity remains present");
        return *found;
    };

    select("trusted-context-mismatch", std::nullopt);
    auto mismatched_request = request_for();
    auto mismatched_payload = addon::encode_usage_owned_event_correction_request(mismatched_request);
    auto foreign_account = *scan.context.account_context;
    foreign_account.adapter_account_key = "foreign-account";
    mismatched_payload.set("account_context", addon::encode_account_context(foreign_account));
    const auto mismatched_account = host.wait_outcome(host.invoke(
        {addon::k_usage_owned_event_correction_capability_id, 1}, "scan", mismatched_payload, 30000, invocation), 31000);
    check(mismatched_account && mismatched_account->status == addon::Host_status::FAILED,
        "trusted outer account must agree with the nested scan account");
    mismatched_request.scan.context.profile_storage->root = addon::path_to_utf8(storage / "foreign-profile");
    check(outcome_for(mismatched_request).status == addon::Host_status::FAILED &&
        !fs::exists(addon::path_from_utf8(scan.cache_selection->candidate_root) / "files.json"),
        "trusted injected profile root must agree with the nested scan before any cache publication");
    select("establish", std::nullopt);
    const auto established = page_for(request_for());
    check(find(established, "kimi:child").total_tokens == 1500 &&
        !find(established, "kimi:child").capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "ordinary raw establishment does not claim removal from an unbound old prefix");
    write(wire_path(child), inherited + marker + usage(300, 1789200002000ull) + usage(0, 1789286400000ull));
    select("zero-token-next-day", established.cache_candidate);
    const auto zero_growth = page_for(request_for());
    const auto& zero_growth_child = find(zero_growth, "kimi:child");
    std::cerr << "zero-token correction: total=" << zero_growth_child.total_tokens
        << ", date=" << zero_growth_child.completed_date
        << ", accepted-date=" << raw_records.front().completed_date << '\n';
    check(zero_growth_child.total_tokens == 300 && zero_growth_child.input.coverage->total_records == 2 &&
        zero_growth_child.completed_date == raw_records.front().completed_date,
        "copied-prefix correction with later-day own zero preserves the accepted completion date and counts the zero occurrence");
    write(wire_path(child), inherited + marker + usage(300, 1789200002000ull) + usage(100, 1789286400000ull, "model-b"));
    select("cancelled-model-growth", established.cache_candidate);
    Cancelled_context cancelled_model(request_for());
    bool model_cancelled = false;
    try { kimi::handle_usage(cancelled_model); }
    catch (const std::exception& failure) { model_cancelled = std::string(failure.what()) == "cancelled"; }
    check(model_cancelled && !fs::exists(addon::path_from_utf8(scan.cache_selection->candidate_root) / "files.json"),
        "cancelled model-changing growth cannot transition the accepted raw cache");
    select("model-changing-growth", established.cache_candidate);
    const auto model_growth = page_for(request_for());
    const auto& model_child = find(model_growth, "kimi:child");
    std::cerr << "model-changing growth: total=" << model_child.total_tokens
        << ", model=" << model_child.model.value_or("") << '\n';
    check(model_child.total_tokens == 1600 && model_child.model == "multiple" &&
        model_child.repository_usage->allocations.size() == 1 &&
        !model_child.capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "model-changing growth first admits all raw usage and truthful descriptive metadata without correction authority");
    select("rejected-model-growth", established.cache_candidate);
    const auto model_replay = page_for(request_for());
    check(find(model_replay, "kimi:child").total_tokens == 1600 &&
        !find(model_replay, "kimi:child").capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "unaccepted model-growth candidate cannot authorize an owned accounting transition");
    accepted_records = model_growth.records;
    for (auto& record : accepted_records) record.capabilities.erase(addon::k_usage_owned_event_correction_capability_id);
    select("mixed-temporal-model-retain", model_growth.cache_candidate);
    const auto model_retained = page_for(request_for());
    const auto& mixed_child = find(model_retained, "kimi:child");
    const auto* mixed_envelope = mixed_child.capabilities.find(addon::k_usage_owned_event_correction_capability_id);
    const auto mixed_authority = mixed_envelope ? addon::decode_usage_owned_event_correction(*mixed_envelope, &failure) : std::nullopt;
    check(mixed_child.total_tokens == 1600 && mixed_child.model == "multiple" &&
        mixed_authority && mixed_authority->disposition == "retain",
        "accepted model growth cannot authorize removal from an independently ambiguous temporal partition");
    write(wire_path(child), inherited + marker + usage(300, 1789200002000ull) +
        usage(100, 1789286400000ull, "model-b") + usage(50, 1789372800000ull, "model-b"));
    select("mixed-temporal-model-own-growth", model_retained.cache_candidate);
    const auto mixed_growth = page_for(request_for());
    check(find(mixed_growth, "kimi:child").total_tokens == 1650 &&
        !find(mixed_growth, "kimi:child").capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "ambiguous temporal removal still admits independently proved own growth after model acceptance");

    // A native predecessor supplies the exact temporal partition needed for
    // subtraction; the synthetic unavailable predecessor above deliberately does not.
    accepted_records = established.records;
    write(wire_path(child), inherited + marker + usage(300, 1789200002000ull) + usage(100, 1789286400000ull, "model-b"));
    select("native-model-growth", established.cache_candidate);
    const auto native_model_growth = page_for(request_for());
    check(find(native_model_growth, "kimi:child").total_tokens == 1600 &&
        find(native_model_growth, "kimi:child").model == "multiple" &&
        !find(native_model_growth, "kimi:child").capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "native model-changing growth first establishes ordinary raw accounting");
    accepted_records = native_model_growth.records;
    for (auto& record : accepted_records) record.capabilities.erase(addon::k_usage_owned_event_correction_capability_id);
    select("accepted-model-correction", native_model_growth.cache_candidate);
    const auto model_correction = page_for(request_for());
    const auto& model_corrected = find(model_correction, "kimi:child");
    check(model_corrected.total_tokens == 400 && model_corrected.model == "multiple" &&
        model_corrected.completed_date == model_child.completed_date &&
        model_corrected.capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "accepted native model growth permits duplicate correction while preserving accepted provenance");
    accepted_records = raw_records;
    write(wire_path(child), inherited + marker + usage(300, 1789200002000ull) + usage(100, 1789286400000ull));
    select("cancelled-correction", established.cache_candidate);
    Cancelled_context cancelled(request_for());
    bool saw_cancel = false;
    try { kimi::handle_usage(cancelled); }
    catch (const std::exception& failure) { saw_cancel = std::string(failure.what()) == "cancelled"; }
    check(saw_cancel && !fs::exists(addon::path_from_utf8(scan.cache_selection->candidate_root) / "files.json"),
        "cancelled correction does not publish or transition the selected raw cache");
    accepted_records.front().repository_usage = addon::Usage_repository{
        {{addon::path_to_utf8(repository), 750}, {addon::path_to_utf8(fixture / "other-repository"), 750}}, 0};
    select("ambiguous-repositories", established.cache_candidate);
    const auto ambiguous_repositories = page_for(request_for());
    const auto& unmapped_repository = find(ambiguous_repositories, "kimi:child");
    check(unmapped_repository.total_tokens == 1600 &&
        !unmapped_repository.capabilities.find(addon::k_usage_owned_event_correction_capability_id) &&
        unmapped_repository.repository_usage->allocations.size() == 2,
        "ambiguous removal preserves both actual host allocations and admits only proved own growth");
    accepted_records = raw_records;
    auto& ambiguous_time = *accepted_records.front().temporal;
    ambiguous_time.portions = {{"instant", "2026-09-10T00:00:00.000Z", {}, {}, 900}};
    ambiguous_time.unavailable_tokens = 600;
    select("ambiguous-temporal", established.cache_candidate);
    const auto ambiguous_temporal = page_for(request_for());
    const auto& unmapped_temporal = find(ambiguous_temporal, "kimi:child");
    check(unmapped_temporal.total_tokens == 1600 &&
        !unmapped_temporal.capabilities.find(addon::k_usage_owned_event_correction_capability_id) &&
        unmapped_temporal.temporal && unmapped_temporal.temporal->unavailable_tokens == 600 &&
        std::any_of(unmapped_temporal.temporal->portions.begin(), unmapped_temporal.temporal->portions.end(),
            [](const auto& portion) { return portion.instant == "2026-09-10T00:00:00.000Z" && portion.total_tokens == 900; }),
        "ambiguous removal preserves actual host temporal evidence and admits only proved own growth");
    accepted_records = raw_records;
    select("correct", established.cache_candidate);
    const auto corrected = page_for(request_for());
    const auto& child_record = find(corrected, "kimi:child");
    const auto* envelope = child_record.capabilities.find(addon::k_usage_owned_event_correction_capability_id);
    const auto authority = envelope ? addon::decode_usage_owned_event_correction(*envelope, &failure) : std::nullopt;
    check(child_record.total_tokens == 400 && authority && authority->removed && authority->removed->total_tokens == 1200 &&
        authority->removed->total_records == 2 && authority->original_record_ids == std::vector<std::string>{"kimi:parent"},
        "1500 accepted raw - 1200 copied + 100 proved growth = 400, including copied zero-event coverage");
    const auto& zero_record = find(corrected, "kimi:zero");
    check(zero_record.total_tokens == 0 && zero_record.input.coverage->total_records == 0 &&
        zero_record.completed_date == accepted_records.back().completed_date &&
        zero_record.capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "zero-own replacement retains identity and accepted date while removing copied coverage");
    check(find(corrected, "kimi:parent").capabilities.find(addon::k_usage_owned_event_correction_capability_id) &&
        !find(corrected, "kimi:parent").temporal, "retain uses the actual predecessor without replaying its old temporal parent");

    select("rejected-replay", established.cache_candidate);
    const auto replay = page_for(request_for());
    check(find(replay, "kimi:child").total_tokens == 400 &&
        find(replay, "kimi:child").capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "unselected correction candidate cannot change accepted raw mode");
    accepted_records = corrected.records;
    for (auto& record : accepted_records) record.capabilities.erase(addon::k_usage_owned_event_correction_capability_id);
    select("ordinary-after-correction", corrected.cache_candidate);
    scan.mode = "repair";
    const auto ordinary_id = host.invoke({"usage-scan", 3}, "scan", addon::encode_usage_scan_request(scan, 3), 30000);
    const auto ordinary_outcome = host.wait_outcome(ordinary_id, 31000);
    check(ordinary_outcome && ordinary_outcome->status == addon::Host_status::SUCCESS, "ordinary scan after correction");
    const auto ordinary = addon::decode_usage_scan_result(ordinary_outcome->payload, &failure, 3);
    check(ordinary && find(*ordinary, "kimi:child").total_tokens == 400 && find(*ordinary, "kimi:zero").total_tokens == 0,
        "accepted owned mode never reinflates child400 to raw1600 on ordinary repair");

    const auto unknown_directory = storage / "unknown-ownership-carrier";
    check(fs::create_directory(unknown_directory), "exclusive unknown ownership cache fixture");
    {
        std::ifstream input(addon::path_from_utf8(corrected.cache_candidate->root) / "files.json", std::ios::binary);
        std::string line, bytes;
        while (std::getline(input, line)) {
            auto row = addon::parse_json(line, {}, nullptr);
            check(row.has_value(), "unknown ownership cache line");
            if (const auto* ownership = row->find("usage-fork-ownership")) {
                auto unknown = *ownership;
                unknown.set("version", addon::Json_value(2));
                row->set("usage-fork-ownership", unknown);
            }
            bytes += addon::serialize_json(*row) + '\n';
        }
        write(unknown_directory / "files.json", bytes);
    }
    const auto unknown_cache = addon::seal_usage_cache_candidate(addon::path_to_utf8(unknown_directory), *allocated,
        scan.context.account_context, "unknown-ownership", "kimi-wire-cache@2", [] { return false; }, &failure);
    check(unknown_cache.has_value(), failure.detail);
    select("ordinary-unknown-ownership", unknown_cache);
    const auto unknown_outcome = host.wait_outcome(host.invoke({"usage-scan", 3}, "scan",
        addon::encode_usage_scan_request(scan, 3), 30000), 31000);
    check(unknown_outcome && unknown_outcome->status == addon::Host_status::SUCCESS,
        "unknown optional ownership retains ordinary accepted accounting");
    const auto unknown_page = addon::decode_usage_scan_result(unknown_outcome->payload, &failure, 3);
    check(unknown_page && find(*unknown_page, "kimi:child").total_tokens == 400 &&
        find(*unknown_page, "kimi:zero").total_tokens == 0,
        "unknown ownership mode cannot reinterpret accepted owned amounts as inflated raw usage");
    const auto unknown_descendant = profile / "sessions/workspace/unknown-descendant";
    native_session(unknown_descendant, repository);
    auto descendant_state = addon::Json_value::object();
    descendant_state.set("id", addon::Json_value("unknown-descendant"));
    descendant_state.set("createdAt", addon::Json_value(std::int64_t{1789200000000}));
    descendant_state.set("forkedFrom", addon::Json_value("parent"));
    write(unknown_descendant / "state.json", addon::serialize_json(descendant_state));
    write(wire_path(unknown_descendant), inherited + marker + usage(20, 1789200002000ull));
    select("unknown-original-proof", unknown_cache);
    check(outcome_for(request_for()).status == addon::Host_status::UNAVAILABLE &&
        !fs::exists(addon::path_from_utf8(scan.cache_selection->candidate_root) / "files.json"),
        "an unknown accepted ownership prefix cannot become original-owner proof for a new descendant");
    check(unknown_descendant.parent_path() == profile / "sessions/workspace", "unknown descendant fixture stays scoped");
    fs::remove_all(unknown_descendant);

    select("bad-digest", corrected.cache_candidate);
    auto bad = request_for();
    bad.predecessors.sha256_hex = std::string(64, '0');
    const auto rejected = outcome_for(bad);
    check(rejected.status == addon::Host_status::UNAVAILABLE &&
        !fs::exists(addon::path_from_utf8(scan.cache_selection->candidate_root) / "files.json"),
        "wrong predecessor digest cannot publish a candidate");

    const auto legacy_cache = [&](const char* name, bool child_only) {
        const auto directory = storage / name;
        check(fs::create_directory(directory), "exclusive legacy fixture cache");
        std::ifstream input(addon::path_from_utf8(established.cache_candidate->root) / "files.json", std::ios::binary);
        std::string line, bytes;
        std::size_t kept_files = 0;
        while (std::getline(input, line)) {
            auto row = addon::parse_json(line, {}, nullptr);
            check(row.has_value(), "legacy fixture cache line");
            if (row->find("fork_event")) continue;
            if (const auto* path = row->find("path")) {
                if (child_only && kimi::account_key(addon::path_from_utf8(path->as_string())) !=
                    kimi::account_key(wire_path(child))) continue;
                ++kept_files;
                row->erase("usage-fork-ownership");
            }
            bytes += addon::serialize_json(*row) + '\n';
        }
        check(!child_only || kept_files == 1, "legacy orphan fixture retains its accepted child row");
        write(directory / "files.json", bytes);
        const auto sealed = addon::seal_usage_cache_candidate(addon::path_to_utf8(directory), *allocated,
            scan.context.account_context, name, "kimi-wire-cache@2", [] { return false; }, &failure);
        check(sealed.has_value(), failure.detail);
        return *sealed;
    };
    const auto legacy = legacy_cache("legacy-raw", false);
    accepted_records = raw_records;
    select("legacy-changed-establishment", legacy);
    const auto changed = page_for(request_for());
    check(find(changed, "kimi:child").total_tokens == 1600 &&
        !find(changed, "kimi:child").capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "changed legacy fingerprint first establishes ordinary RAW1600 without old-prefix removal authority");
    accepted_records = changed.records;
    for (auto& record : accepted_records) record.capabilities.erase(addon::k_usage_owned_event_correction_capability_id);
    select("legacy-established-correction", changed.cache_candidate);
    const auto later = page_for(request_for());
    check(find(later, "kimi:child").total_tokens == 400 &&
        find(later, "kimi:child").capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "accepted raw population becomes correctable only with its separately proved ownership chain");

    const auto orphan_cache = legacy_cache("legacy-missing-parent", true);
    check(parent.parent_path() == profile / "sessions/workspace" && zero.parent_path() == parent.parent_path(),
        "native fixture deletion stays inside this test workspace");
    fs::remove_all(parent);
    fs::remove_all(zero);
    accepted_records = raw_records;
    select("unresolved-retain", orphan_cache);
    const auto retained = page_for(request_for());
    const auto& retained_child = find(retained, "kimi:child");
    const auto retained_authority = addon::decode_usage_owned_event_correction(
        *retained_child.capabilities.find(addon::k_usage_owned_event_correction_capability_id), &failure);
    check(retained_child.total_tokens == 1500 && retained_authority && retained_authority->disposition == "retain" &&
        !retained_child.temporal, "accepted unresolved legacy prefix retains the exact actual predecessor");
    write(wire_path(child), inherited + marker + usage(300, 1789200002000ull) + usage(100, 1789286400000ull) +
        usage(50, 1789372800000ull));
    select("unresolved-own-growth", retained.cache_candidate);
    const auto grown = page_for(request_for());
    check(find(grown, "kimi:child").total_tokens == 1550 &&
        !find(grown, "kimi:child").capabilities.find(addon::k_usage_owned_event_correction_capability_id),
        "unresolved accepted prefix grants only the later proved own append50, never removal authority");
    const auto orphan = profile / "sessions/workspace/fresh-orphan";
    native_session(orphan, repository);
    auto orphan_state = addon::Json_value::object();
    orphan_state.set("id", addon::Json_value("fresh-orphan"));
    orphan_state.set("createdAt", addon::Json_value(std::int64_t{1789200000000}));
    orphan_state.set("forkedFrom", addon::Json_value("missing"));
    write(orphan / "state.json", addon::serialize_json(orphan_state));
    write(wire_path(orphan), usage(30, 1789200000000ull) + marker);
    select("fresh-unresolved", grown.cache_candidate);
    const auto unavailable = outcome_for(request_for());
    check(unavailable.status == addon::Host_status::UNAVAILABLE &&
        !fs::exists(addon::path_from_utf8(scan.cache_selection->candidate_root) / "files.json"),
        "fresh unresolved billable row makes the whole operation unavailable without candidate acceptance");
    check(orphan.parent_path() == profile / "sessions/workspace", "orphan fixture stays scoped");
    fs::remove_all(orphan);
    accepted_records = grown.records;
    write_sources(profile, 1);
    select("paged-correction", grown.cache_candidate);
    auto paged_request = request_for();
    const auto first_outcome = outcome_for(paged_request);
    check(first_outcome.status == addon::Host_status::SUCCESS, "paged owned-event first outcome");
    const auto first_page = addon::decode_usage_scan_result(first_outcome.payload, &failure, 3);
    check(first_page && !first_page->complete && first_page->next_cursor, "owned-event continuation is exercised");
    auto ordinary_continuation = scan;
    ordinary_continuation.snapshot_id = first_page->snapshot_id;
    ordinary_continuation.cursor = first_page->next_cursor;
    const auto wrong_operation = host.wait_outcome(host.invoke({"usage-scan", 3}, "scan",
        addon::encode_usage_scan_request(ordinary_continuation, 3), 30000), 31000);
    check(wrong_operation && wrong_operation->status != addon::Host_status::SUCCESS,
        "ordinary usage operation cannot resume an owned-event candidate");
    paged_request.scan = ordinary_continuation;
    auto wrong_predecessor = paged_request;
    wrong_predecessor.predecessors.sha256_hex = std::string(64, '0');
    check(outcome_for(wrong_predecessor).status != addon::Host_status::SUCCESS,
        "continuation cannot change the actual predecessor stream binding");
    const auto final_outcome = outcome_for(paged_request);
    check(final_outcome.status == addon::Host_status::SUCCESS, "bound owned-event continuation succeeds");
    const auto final_page = addon::decode_usage_scan_result(final_outcome.payload, &failure, 3);
    check(final_page && final_page->complete && final_page->cache_candidate == first_page->cache_candidate,
        "owned-event pages keep one exact cache candidate through completion");
    std::cerr << "owned-event child400/zero/rejected-replay/ordinary-reparse passed\n";
}

void unsettled_observation_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "native-profile";
    const auto session = profile / "sessions/workspace/first-observation";
    native_session(session);
    write(session / "agents/main/wire.jsonl",
        "{\"type\":\"usage.record\",\"agentId\":\"main\",\"time\":1789200000000,"
        "\"usage\":{\"inputOther\":120,\"output\":30,\"inputCacheRead\":0,\"inputCacheCreation\":0}}\n");

    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "host-storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail);
    const auto generation = host.generation_nonce();
    const auto process = host.adapter_pid();
    const auto account = kimi::account_key(profile);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.kimi", &error, account);
    check(allocated.has_value(), error);

    addon::Usage_scan_request request;
    const auto native_profile = addon::path_to_utf8(profile);
    request.context.launcher = {"kimi", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"kimi"};
    request.context.profile_storage = addon::Profile_storage_context{1, *allocated};
    request.context.account_context = addon::Account_context{2, account, native_profile};
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(),
        addon::Environment_bootstrap_evidence{native_profile}};
    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = request.context.profile_storage;
    const auto acknowledge = host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledge, 6000);
    check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
        "packaged adapter acknowledges the observation fixture's storage");
    const auto select = [&](const char* name) {
        const auto candidate = addon::path_from_utf8(*allocated) / name;
        check(fs::create_directory(candidate), "allocate observation candidate");
        request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(candidate)};
    };
    const auto unpublished = [&] {
        const auto candidate = addon::path_from_utf8(request.cache_selection->candidate_root);
        check(!fs::exists(candidate / "files.json") && !fs::exists(candidate / "cache-manifest.json"),
            "an unavailable observation publishes no cache candidate");
        check(host.state() == addon::Simulator_state::READY && host.generation_nonce() == generation &&
            host.adapter_pid() == process, "the observation keeps the same adapter generation and process");
    };

    // Hold the native replacement gap open, rather than racing a timer against the reader.
    fs::rename(session / "state.json", session / "state.json.replacing");
    select("state-replacement-gap");
    const auto unsettled = test::usage_scan_outcome(host, request, 3);
    check(unsettled.status == addon::Host_status::UNAVAILABLE, "missing native state remains unavailable");
    unpublished();
    fs::rename(session / "state.json.replacing", session / "state.json");
    select("state-settled");
    const auto settled = test::usage_scan_pages(host, request, 3);
    check(settled.records.size() == 1 && test::usage_token_total(settled) == 150 && settled.cache_candidate,
        "the settled first observation succeeds with its exact token total");
    check(host.state() == addon::Simulator_state::READY && host.generation_nonce() == generation &&
        host.adapter_pid() == process, "settling the source needs no adapter restart or generation replacement");
    check(addon::validate_usage_cache_candidate(*settled.cache_candidate, *allocated,
        request.context.account_context, [] { return false; }, &failure).has_value(), failure.detail);
    std::cerr << "state replacement: " << addon::host_status_name(unsettled.status) << '/'
        << unsettled.reason_code << "; settled total=150; same generation and process\n";

    write(session / "state.json", "{}");
    select("invalid-identity");
    const auto invalid = test::usage_scan_outcome(host, request, 3);
    check(invalid.status == addon::Host_status::UNAVAILABLE && invalid.reason_code == "usage_ownership_unavailable",
        "a present invalid session identity remains an ownership refusal");
    unpublished();

    native_session(session);
    std::ifstream state_input(session / "state.json", std::ios::binary);
    const std::string state_bytes((std::istreambuf_iterator<char>(state_input)), {});
    state_input.close();
    auto state = addon::parse_json(state_bytes, {}, nullptr);
    check(state.has_value(), "native session fixture must parse");
    state->set("forkedFrom", addon::Json_value("missing-ancestor"));
    write(session / "state.json", addon::serialize_json(*state));
    select("missing-ancestor");
    const auto orphan = test::usage_scan_outcome(host, request, 3);
    check(orphan.status == addon::Host_status::UNAVAILABLE && orphan.reason_code == "usage_ownership_unavailable",
        "unresolved ancestry remains an ownership refusal");
    unpublished();
    check(unsettled.reason_code == "usage_observation_unsettled",
        "the first-observation replacement gap must be distinguishable from invalid ownership");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2 || argc == 3, "expected built package directory and optional phase");
        const std::string phase = argc == 3 ? argv[2] : "";
        check(phase.empty() || phase == "cache" || phase == "temporal" ||
            phase == "refresh" || phase == "capacity" || phase == "owned" ||
            phase == "observation", "unknown usage cache test phase");
        Workspace workspace;
        const auto run_phase = [&](const char* name, const auto& contract, const fs::path& fixture) {
            if (!phase.empty() && phase != name) {
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            std::cerr << "usage cache phase " << name << " begin" << std::endl;
            contract(addon::path_from_utf8(argv[1]), fixture);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            std::cerr << "usage cache phase " << name << " end " << elapsed.count() << "ms" << std::endl;
        };
        run_phase("cache", adapter_cache_contract, workspace.m_path);
        run_phase("temporal", temporal_cache_contract, workspace.m_path / "temporal");
        run_phase("refresh", temporal_refresh_contract, workspace.m_path / "refresh");
        run_phase("capacity", temporal_capacity_contract, workspace.m_path / "capacity");
        run_phase("owned", owned_event_contract, workspace.m_path / "owned");
        run_phase("observation", unsettled_observation_contract, workspace.m_path / "observation");
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
