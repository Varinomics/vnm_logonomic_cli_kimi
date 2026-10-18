#include "kimi_usage_correction.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/usage_temporal.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

addon::Usage_record usage(std::uint64_t tokens, std::uint64_t events)
{
    addon::Usage_record record;
    record.record_id                = "kimi:child";
    record.session_key              = record.record_id;
    record.completed_date           = "2026-09-12";
    record.total_tokens             = tokens;
    record.model                    = "model-a";
    record.counter_field            = "usage";
    record.source_counter_semantics = "per_event_sum";
    record.aggregate_evidence       = "partial";
    record.input = {true, tokens, addon::Usage_field_coverage{events, events, false, addon::Usage_support::SUPPORTED}};
    for (auto* amount : {&record.output, &record.cache_read, &record.cache_creation}) {
        *amount = {true, 0, addon::Usage_field_coverage{events, events, false, addon::Usage_support::SUPPORTED}};
    }
    const auto path = addon::path_to_utf8(std::filesystem::temp_directory_path() / "kimi-repository");
    record.repository_usage = addon::Usage_repository{{{path, tokens}}, 0};
    addon::Usage_temporal temporal;
    temporal.parent             = {record.record_id, tokens, "accepted-snapshot", std::nullopt};
    temporal.unavailable_tokens = tokens;
    temporal.coverage_reason    = "missing-evidence";
    record.temporal = std::move(temporal);
    return record;
}

addon::Usage_owned_event_predecessor predecessor(const addon::Usage_record& record)
{
    return {record, std::string(64, 'a'), addon::Json_value{}};
}

} // namespace

int main()
{
    unsigned failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    try {
        const auto accepted = usage(1500, 3);
        const auto removed  = usage(1200, 2);
        auto host = predecessor(usage(1600, 4));
        const auto other = addon::path_to_utf8(std::filesystem::temp_directory_path() / "other-repository");
        host.record.repository_usage->allocations.front().total_tokens = 1500;
        host.record.repository_usage->allocations.push_back({other, 100});
        auto cached = host.record;
        cached.repository_usage->allocations.pop_back();
        cached.repository_usage->unattributed_tokens = 100;
        const auto component = kimi::adjust_owned_usage(host, cached, removed, std::nullopt, {"kimi:parent"});
        check(component && component->total_tokens == 400 &&
            component->repository_usage->allocations.size() == 2 &&
            component->repository_usage->allocations.back().repository_path == other &&
            component->repository_usage->allocations.back().total_tokens == 100,
            "known repository component removes copied1200 and preserves unrelated host100");
        cached.repository_usage->allocations.front().total_tokens = 1400;
        cached.repository_usage->unattributed_tokens = 200;
        check(!kimi::adjust_owned_usage(host, cached, removed, std::nullopt, {"kimi:parent"}),
            "a mismatching removed repository component grants no subtraction authority");

#ifdef _WIN32
        auto normalized_host = predecessor(accepted);
        auto& host_path = normalized_host.record.repository_usage->allocations.front().repository_path;
        std::replace(host_path.begin(), host_path.end(), '\\', '/');
        if (host_path.front() >= 'A' && host_path.front() <= 'Z') host_path.front() += 'a' - 'A';
        const auto host_repository_path = host_path;
        const auto native_growth = kimi::adjust_owned_usage(normalized_host, accepted, std::nullopt, usage(100, 1), {});
        std::cerr << "host repository=" << host_repository_path << ", native repository="
            << accepted.repository_usage->allocations.front().repository_path << ", growth allocations="
            << (native_growth ? native_growth->repository_usage->allocations.size() : 0) << '\n';
        check(native_growth && native_growth->total_tokens == 1600 &&
            native_growth->repository_usage->allocations.size() == 1 &&
            native_growth->repository_usage->allocations.front().repository_path == host_repository_path &&
            native_growth->repository_usage->allocations.front().total_tokens == 1600,
            "native growth merges into the same host-normalized repository and preserves its path");
        normalized_host = predecessor(usage(1600, 4));
        normalized_host.record.repository_usage->allocations.front() = {host_repository_path, 1500};
        normalized_host.record.repository_usage->allocations.push_back({other, 100});
        cached = usage(1600, 4);
        cached.repository_usage->allocations.front().total_tokens = 1500;
        cached.repository_usage->unattributed_tokens = 100;
        const auto normalized_component = kimi::adjust_owned_usage(
            normalized_host, cached, removed, std::nullopt, {"kimi:parent"});
        std::cerr << "normalized component correction="
            << (normalized_component ? std::to_string(normalized_component->total_tokens) : "unavailable") << '\n';
        check(normalized_component && normalized_component->total_tokens == 400 &&
            normalized_component->repository_usage->allocations.size() == 2 &&
            std::any_of(normalized_component->repository_usage->allocations.begin(),
                normalized_component->repository_usage->allocations.end(), [&](const auto& allocation) {
                    return allocation.repository_path == host_repository_path && allocation.total_tokens == 300;
                }) &&
            std::any_of(normalized_component->repository_usage->allocations.begin(),
                normalized_component->repository_usage->allocations.end(), [&](const auto& allocation) {
                    return allocation.repository_path == other && allocation.total_tokens == 100;
                }),
            "native copied usage matches the host-normalized component while preserving unrelated host usage");
        cached.repository_usage->allocations.front().total_tokens = 1400;
        cached.repository_usage->unattributed_tokens = 200;
        check(!kimi::adjust_owned_usage(normalized_host, cached, removed, std::nullopt, {"kimi:parent"}),
            "equivalent repository spelling does not authorize a mismatching component total");
#endif

        const auto trailing_repository = [](addon::Usage_record record) {
            auto& path = record.repository_usage->allocations.front().repository_path;
            path = addon::path_to_utf8(addon::path_from_utf8(path) / "");
            return record;
        };
        const auto trailing_growth = kimi::adjust_owned_usage(
            predecessor(accepted), trailing_repository(accepted), std::nullopt, trailing_repository(usage(100, 1)), {});
        std::cerr << "trailing-separator growth allocations="
            << (trailing_growth ? trailing_growth->repository_usage->allocations.size() : 0) << '\n';
        check(trailing_growth && trailing_growth->total_tokens == 1600 &&
            trailing_growth->repository_usage->allocations.size() == 1 &&
            trailing_growth->repository_usage->allocations.front().repository_path ==
                accepted.repository_usage->allocations.front().repository_path,
            "a native terminal separator merges growth into the accepted repository");
        cached = trailing_repository(host.record);
        cached.repository_usage->allocations.pop_back();
        cached.repository_usage->unattributed_tokens = 100;
        const auto trailing_component = kimi::adjust_owned_usage(
            host, cached, trailing_repository(removed), std::nullopt, {"kimi:parent"});
        std::cerr << "trailing-separator component correction="
            << (trailing_component ? std::to_string(trailing_component->total_tokens) : "unavailable") << '\n';
        check(trailing_component && trailing_component->total_tokens == 400 &&
            trailing_component->repository_usage->allocations.size() == 2 &&
            trailing_component->repository_usage->allocations.front().total_tokens == 300 &&
            trailing_component->repository_usage->allocations.back().total_tokens == 100,
            "a native terminal separator maps copied usage without changing the unrelated component");
        for (const auto& path : {std::filesystem::temp_directory_path().root_path(),
                addon::path_from_utf8("//server/share/")})
        {
            auto root_accepted = accepted;
            root_accepted.repository_usage->allocations.front().repository_path = addon::path_to_utf8(path);
            auto root_growth = usage(100, 1);
            root_growth.repository_usage->allocations.front().repository_path =
                addon::path_to_utf8(path.has_relative_path() ? path.parent_path() : path);
            const auto adjusted = kimi::adjust_owned_usage(predecessor(root_accepted), root_accepted, std::nullopt, root_growth, {});
            check(adjusted && adjusted->total_tokens == 1600 &&
                adjusted->repository_usage->allocations.size() == 1 &&
                adjusted->repository_usage->allocations.front().repository_path ==
                    root_accepted.repository_usage->allocations.front().repository_path,
                "repository identity preserves filesystem and share roots in accepted output");
            root_growth.repository_usage->allocations.front().repository_path =
                addon::path_to_utf8(path.has_relative_path() ? addon::path_from_utf8("//server/other-share/") :
                    std::filesystem::current_path());
            const auto distinct = kimi::adjust_owned_usage(predecessor(root_accepted), root_accepted, std::nullopt, root_growth, {});
            check(distinct && distinct->repository_usage->allocations.size() == 2,
                "filesystem roots remain distinct from the working directory and other share roots");
        }

        auto opaque = predecessor(accepted);
        opaque.record.temporal.reset();
        opaque.temporal_acceptance = addon::Json_value::object();
        opaque.temporal_acceptance.set("negotiated_version", addon::Json_value(99));
        check(!kimi::adjust_owned_usage(opaque, accepted, removed, std::nullopt, {"kimi:parent"}),
            "opaque temporal acceptance grants no removal authority");
        const auto opaque_growth = kimi::adjust_owned_usage(opaque, accepted, std::nullopt, usage(100, 1), {});
        check(opaque_growth && opaque_growth->total_tokens == 1600 &&
            !opaque_growth->capabilities.find(addon::k_usage_owned_event_correction_capability_id),
            "opaque temporal acceptance still permits ordinary independently proved growth");

        auto no_model = predecessor(accepted);
        no_model.record.model.reset();
        const auto first_model = kimi::adjust_owned_usage(no_model, accepted, std::nullopt, usage(100, 1), {});
        check(first_model && first_model->model == "model-a",
            "the first known growth model fills absent predecessor metadata");
        no_model.record.model = "";
        const auto empty_model = kimi::adjust_owned_usage(no_model, accepted, std::nullopt, usage(100, 1), {});
        check(empty_model && empty_model->model == "model-a",
            "the first known growth model fills empty predecessor metadata");

        auto full = usage(addon::k_max_usage_temporal_portions, addon::k_max_usage_temporal_portions);
        full.temporal->unavailable_tokens = 0;
        full.temporal->coverage_reason    = "complete";
        for (std::size_t index = 0; index < addon::k_max_usage_temporal_portions; ++index) {
            char instant[25]{};
            std::snprintf(instant, sizeof(instant), "2026-09-12T%02u:%02u:%02u.000Z",
                (unsigned)(index / 3600), (unsigned)(index / 60 % 60), (unsigned)(index % 60));
            full.temporal->portions.push_back({"instant", instant, {}, {}, 1});
        }
        check(addon::decode_usage_record(addon::encode_usage_record(full, 3), nullptr, 3).has_value(),
            "the capacity predecessor is valid accepted temporal evidence");
        auto growth = usage(1, 1);
        growth.completed_date = "2026-09-13";
        growth.temporal->unavailable_tokens = 0;
        growth.temporal->coverage_reason    = "complete";
        growth.temporal->portions = {{"instant", "2026-09-13T00:00:00.000Z", {}, {}, 1}};
        const auto capacity = kimi::adjust_owned_usage(predecessor(full), full, std::nullopt, growth, {});
        check(capacity && capacity->total_tokens == 4097 && capacity->temporal &&
            capacity->temporal->portions.empty() && capacity->temporal->unavailable_tokens == 4097 &&
            capacity->temporal->coverage_reason == "capacity" &&
            !capacity->capabilities.find(addon::k_usage_owned_event_correction_capability_id),
            "4096 accepted instants plus own growth use ordinary capacity evidence without stalling accounting");
        auto copied = usage(1, 1);
        copied.temporal->unavailable_tokens = 0;
        copied.temporal->coverage_reason    = "complete";
        copied.temporal->portions = {full.temporal->portions.front()};
        growth = usage(2, 2);
        growth.temporal->unavailable_tokens = 0;
        growth.temporal->coverage_reason    = "complete";
        growth.temporal->portions = {{"instant", "2026-09-13T00:00:00.000Z", {}, {}, 1},
            {"instant", "2026-09-13T00:00:01.000Z", {}, {}, 1}};
        check(!kimi::adjust_owned_usage(predecessor(full), full, copied, growth, {"kimi:parent"}),
            "a removal requiring over-capacity temporal evidence remains unavailable for correction");
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cerr << "owned adjustment failures=" << failures << '\n';
    return failures ? 1 : 0;
}
