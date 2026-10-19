#include "kimi_usage_correction.h"
#include "kimi_accounts.h"
#include "kimi_usage.h"
#include "kimi_json.h"

#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <array>
#include <map>

namespace json_util = logonomic::cli_addon::json;

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
constexpr std::uint64_t k_max_exact = 9007199254740991;

std::array<addon::Usage_amount*, 4> amounts(addon::Usage_record& record)
{
    return {&record.input, &record.output, &record.cache_read, &record.cache_creation};
}

bool increase(std::uint64_t& target, std::uint64_t value)
{
    if (value > k_max_exact - target) return false;
    target += value;
    return true;
}

bool decrease(std::uint64_t& target, std::uint64_t value)
{
    if (target < value) return false;
    target -= value;
    return true;
}

using Repository_parts = std::map<std::string, addon::Usage_repository_allocation>;

std::optional<Repository_parts> repository_parts(const addon::Usage_repository& usage)
{
    Repository_parts parts;
    if (usage.unattributed_tokens) parts.emplace(std::string{}, addon::Usage_repository_allocation{{}, usage.unattributed_tokens});
    for (const auto& allocation : usage.allocations) {
        auto path = addon::path_from_utf8(allocation.repository_path).lexically_normal();
        if (!path.has_filename() && path.has_relative_path()) path = path.parent_path();
        const auto key = account_key(path);
        if (!parts.emplace(key, allocation).second) return std::nullopt;
    }
    return parts;
}

addon::Usage_repository repository_usage(const Repository_parts& parts)
{
    addon::Usage_repository result;
    for (const auto& [key, allocation] : parts) {
        if (allocation.repository_path.empty()) result.unattributed_tokens = allocation.total_tokens;
        else if (allocation.total_tokens) result.allocations.push_back(allocation);
    }
    std::sort(result.allocations.begin(), result.allocations.end(), [](const auto& left, const auto& right) {
        return left.repository_path < right.repository_path;
    });
    return result;
}

std::string temporal_key(const addon::Usage_temporal_portion& portion)
{
    return addon::serialize_json_canonical(json_util::object({{"basis", Json(portion.basis)},
        {"instant", Json(portion.instant)}, {"date", Json(portion.date)}, {"calendar", Json(portion.calendar)}}));
}

using Temporal_parts = std::map<std::string, addon::Usage_temporal_portion>;

Temporal_parts temporal_parts(const addon::Usage_temporal& usage)
{
    Temporal_parts parts;
    for (const auto& portion : usage.portions) parts.emplace(temporal_key(portion), portion);
    return parts;
}

bool same_temporal_parts(const addon::Usage_temporal& left, const addon::Usage_temporal& right)
{
    if (left.unavailable_tokens != right.unavailable_tokens) return false;
    const auto a = temporal_parts(left), b = temporal_parts(right);
    if (a.size() != b.size()) return false;
    auto other = b.begin();
    for (const auto& [key, portion] : a) {
        if (other->first != key || other->second.total_tokens != portion.total_tokens) return false;
        ++other;
    }
    return true;
}

} // namespace

addon::Usage_record retain_usage_predecessor(const addon::Usage_owned_event_predecessor& predecessor)
{
    auto record = predecessor.record;
    record.temporal.reset();
    record.capabilities.erase(addon::k_usage_temporal_capability_id);
    addon::Usage_owned_event_correction authority;
    authority.disposition = "retain";
    authority.predecessor_sha256_hex = predecessor.predecessor_sha256_hex;
    record.capabilities.set(addon::k_usage_owned_event_correction_capability_id,
        addon::encode_usage_owned_event_correction(authority));
    return record;
}

std::optional<addon::Usage_record> adjust_owned_usage(
    const addon::Usage_owned_event_predecessor& predecessor,
    const std::optional<addon::Usage_record>& accepted_files,
    const std::optional<addon::Usage_record>& removed,
    const std::optional<addon::Usage_record>& growth,
    const std::vector<std::string>& original_record_ids)
{
    auto record = predecessor.record;
    record.aggregate_evidence = "partial";
    record.capabilities.erase(addon::k_usage_owned_event_correction_capability_id);
    addon::Usage_owned_event_correction authority;
    authority.disposition = "correct";
    authority.predecessor_sha256_hex = predecessor.predecessor_sha256_hex;
    authority.original_record_ids = original_record_ids;
    addon::Usage_owned_event_removal removal;
    auto counters = amounts(record);
    if (removed) {
        if (!accepted_files || accepted_files->total_tokens > record.total_tokens ||
            original_record_ids.empty() || !removed->input.coverage) return std::nullopt;
        removal.total_tokens = removed->total_tokens;
        removal.total_records = removed->input.coverage->total_records;
        if (!decrease(record.total_tokens, removal.total_tokens)) return std::nullopt;
        auto copy = *removed;
        const auto source = amounts(copy);
        std::array<addon::Usage_owned_event_counter_removal*, 4> removed_counters{
            &removal.input, &removal.output, &removal.cache_read, &removal.cache_creation};
        const bool records = record.coverage_unit == addon::Usage_coverage_unit::RECORDS;
        authority.coverage_policy = records ? "records" : "retained_unknown_sessions";
        for (std::size_t index = 0; index < counters.size(); ++index) {
            if (!source[index]->value || !source[index]->coverage || !counters[index]->coverage) return std::nullopt;
            *removed_counters[index] = {*source[index]->value, source[index]->coverage->covered_records};
            auto& target = *counters[index];
            if (!records) {
                if (target.value.value_or(0) || target.coverage->covered_records ||
                    target.coverage->support != addon::Usage_support::UNKNOWN || target.coverage->overflow) return std::nullopt;
                continue;
            }
            if (!target.value || !decrease(*target.value, *source[index]->value) ||
                !decrease(target.coverage->covered_records, source[index]->coverage->covered_records) ||
                !decrease(target.coverage->total_records, removal.total_records)) return std::nullopt;
            target.coverage->support = target.coverage->covered_records ?
                addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN;
        }
    }
    if (growth) {
        if (!increase(record.total_tokens, growth->total_tokens)) return std::nullopt;
        if (growth->total_tokens) record.completed_date = std::max(record.completed_date, growth->completed_date);
        auto copy = *growth;
        const auto source = amounts(copy);
        if (record.coverage_unit == addon::Usage_coverage_unit::RECORDS) {
            for (std::size_t index = 0; index < counters.size(); ++index) {
                auto& target = *counters[index];
                if (!target.value || !target.coverage || !source[index]->value || !source[index]->coverage ||
                    !increase(*target.value, *source[index]->value) ||
                    !increase(target.coverage->covered_records, source[index]->coverage->covered_records) ||
                    !increase(target.coverage->total_records, source[index]->coverage->total_records)) return std::nullopt;
                if (target.coverage->covered_records) target.coverage->support = addon::Usage_support::SUPPORTED;
            }
        }
        if (!record.model || record.model->empty()) {
            record.model = growth->model;
        }
        else
        if (growth->model && !growth->model->empty() && growth->model != record.model) {
            record.model = "multiple";
        }
    }
    if (!record.repository_usage) return std::nullopt;
    auto repositories = repository_parts(*record.repository_usage);
    if (!repositories) return std::nullopt;
    if (removed && removed->total_tokens) {
        Repository_parts removed_parts;
        if (repositories->size() == 1) {
            auto allocation = repositories->begin()->second;
            allocation.total_tokens = removed->total_tokens;
            removed_parts.emplace(repositories->begin()->first, std::move(allocation));
        }
        else
        if (accepted_files->repository_usage && removed->repository_usage) {
            const auto source_parts = repository_parts(*removed->repository_usage);
            const auto accepted_parts = repository_parts(*accepted_files->repository_usage);
            if (!source_parts || !accepted_parts) return std::nullopt;
            removed_parts = *source_parts;
            for (const auto& [key, allocation] : removed_parts) {
                const auto accepted = accepted_parts->find(key);
                const auto retained = repositories->find(key);
                if (accepted == accepted_parts->end() || retained == repositories->end() ||
                    accepted->second.total_tokens != retained->second.total_tokens)
                {
                    return std::nullopt;
                }
            }
        }
        else return std::nullopt;
        for (auto& [key, allocation] : removed_parts) {
            const auto found = repositories->find(key);
            if (found == repositories->end() || !decrease(found->second.total_tokens, allocation.total_tokens)) return std::nullopt;
            allocation.repository_path = found->second.repository_path;
        }
        removal.repository_usage = repository_usage(removed_parts);
    }
    if (growth && growth->repository_usage) {
        const auto growth_parts = repository_parts(*growth->repository_usage);
        if (!growth_parts) return std::nullopt;
        for (const auto& [key, allocation] : *growth_parts) {
            auto [found, inserted] = repositories->try_emplace(key, allocation);
            if (!inserted && !increase(found->second.total_tokens, allocation.total_tokens)) return std::nullopt;
        }
    }
    record.repository_usage = repository_usage(*repositories);
    if (record.temporal) {
        auto evidence = *record.temporal;
        auto portions = temporal_parts(evidence);
        if (removed) {
            addon::Usage_owned_event_temporal_removal temporal;
            if (portions.empty() && evidence.unavailable_tokens == predecessor.record.total_tokens) {
                temporal.unavailable_tokens = removed->total_tokens;
            }
            else if (accepted_files->temporal && removed->temporal &&
                same_temporal_parts(*accepted_files->temporal, evidence))
            {
                temporal.portions = removed->temporal->portions;
                temporal.unavailable_tokens = removed->temporal->unavailable_tokens;
            }
            else return std::nullopt;
            if (!decrease(evidence.unavailable_tokens, temporal.unavailable_tokens)) return std::nullopt;
            for (const auto& portion : temporal.portions) {
                const auto found = portions.find(temporal_key(portion));
                if (found == portions.end() || !decrease(found->second.total_tokens, portion.total_tokens)) return std::nullopt;
            }
            removal.temporal = std::move(temporal);
        }
        if (growth) {
            if (!growth->temporal) {
                if (!increase(evidence.unavailable_tokens, growth->total_tokens)) return std::nullopt;
            }
            else {
                if (!increase(evidence.unavailable_tokens, growth->temporal->unavailable_tokens)) return std::nullopt;
                for (const auto& portion : growth->temporal->portions) {
                    auto [found, inserted] = portions.try_emplace(temporal_key(portion), portion);
                    if (!inserted && !increase(found->second.total_tokens, portion.total_tokens)) return std::nullopt;
                }
            }
        }
        evidence.portions.clear();
        for (const auto& [key, portion] : portions) if (portion.total_tokens) evidence.portions.push_back(portion);
        evidence.parent.total_tokens = record.total_tokens;
        evidence.retained_from.reset();
        evidence.coverage_reason = evidence.unavailable_tokens ? "missing-evidence" : "complete";
        if (!removed && evidence.portions.size() > addon::k_max_usage_temporal_portions) {
            evidence.portions.clear();
            evidence.unavailable_tokens = record.total_tokens;
            evidence.coverage_reason    = "capacity";
        }
        if (!addon::decode_usage_temporal(addon::encode_usage_temporal(evidence), nullptr)) return std::nullopt;
        record.temporal = std::move(evidence);
    }
    else
    if (removed && !predecessor.temporal_acceptance.is_null()) {
        return std::nullopt;
    }
    if (removed) {
        authority.removed = std::move(removal);
        const auto envelope = addon::encode_usage_owned_event_correction(authority);
        if (addon::serialize_json(envelope).size() > addon::limits::k_max_exchange_payload_bytes) {
            throw Usage_ownership_unavailable();
        }
        if (!addon::decode_usage_owned_event_correction(envelope, nullptr)) return std::nullopt;
        record.capabilities.set(addon::k_usage_owned_event_correction_capability_id, envelope);
    }
    const auto encoded = addon::encode_usage_record(record, 3);
    if (!addon::parse_json(addon::serialize_json(encoded), {}, nullptr)) throw Usage_ownership_unavailable();
    if (!addon::decode_usage_record(encoded, nullptr, 3)) return std::nullopt;
    return record;
}

} // namespace kimi
