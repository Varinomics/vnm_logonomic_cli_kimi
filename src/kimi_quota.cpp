#include "kimi_quota.h"
#include "kimi_accounts.h"
#include "kimi_config.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/quota.h>
#include <logonomic_cli_addon/package_source.h>

#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;

std::int64_t integer(const Json& value, std::int64_t minimum = 0)
{
    if (!value.is_number() || value.as_number() < static_cast<double>(minimum) ||
        value.as_number() > 9007199254740991.0 || std::floor(value.as_number()) != value.as_number()) {
        throw std::runtime_error("invalid_quota_integer");
    }
    return static_cast<std::int64_t>(value.as_number());
}

Json normalized_window(double used_percent, const char* label, std::int64_t minutes, const Json& reset)
{
    if (!std::isfinite(used_percent)) throw std::runtime_error("invalid_quota_usage");
    auto result = object({{"used_percent", Json(used_percent)},
        {"window_minutes", Json(minutes)}, {"window_label", Json(label)}});
    if (!reset.is_null()) {
        if (!bounded_string(reset, 64)) throw std::runtime_error("invalid_quota_reset");
        result.set("reset_at", reset);
    }
    return result;
}

Json window(const Json& row, const char* label, std::int64_t expected_minutes)
{
    const auto& used = member(row, "used");
    const auto& limit = member(row, "limit");
    if (!used.is_number() || !limit.is_number() || used.as_number() < 0 || limit.as_number() <= 0) {
        throw std::runtime_error("invalid_quota_usage");
    }
    const auto& period = member(row, "window");
    const auto duration = integer(member(period, "duration"), 1);
    const auto unit = text(period, "unit");
    const std::int64_t multiplier = unit == "minute" ? 1 : unit == "hour" ? 60 : unit == "day" ? 1440 : unit == "week" ? 10080 : 0;
    if (multiplier == 0 || duration > 9007199254740991ll / multiplier || duration * multiplier != expected_minutes) {
        throw std::runtime_error("invalid_quota_window");
    }
    return normalized_window(
        100.0 * used.as_number() / limit.as_number(), label, expected_minutes, member(row, "reset_at"));
}

// Kimi Code 2.0.2 reports a window as the used fraction of its allowance.
Json ratio_window(const Json& usage, const char* label, std::int64_t minutes)
{
    const auto& ratio = member(usage, "usedRatio");
    if (!ratio.is_number() || ratio.as_number() < 0) throw std::runtime_error("invalid_quota_usage");
    return normalized_window(100.0 * ratio.as_number(), label, minutes, member(usage, "resetAt"));
}

std::optional<std::uint64_t> reset_time(const Json& value)
{
    if (value.is_null()) return std::nullopt;
    if (!value.is_string()) throw std::runtime_error("invalid_quota_reset");
    const auto& time = value.as_string();
    // Kimi emits ISO 8601 UTC or explicitly offset timestamps. Convert with
    // civil arithmetic; local timezone/DST must not affect quota identity.
    if (time.size() < 20 || time[4] != '-' || time[7] != '-' || time[10] != 'T' || time[13] != ':' || time[16] != ':') {
        throw std::runtime_error("invalid_quota_reset");
    }
    const auto digits = [&](std::size_t offset, std::size_t length) {
        int result = 0;
        for (std::size_t index = offset; index < offset + length; ++index) {
            if (index >= time.size() || time[index] < '0' || time[index] > '9') throw std::runtime_error("invalid_quota_reset");
            result = result * 10 + time[index] - '0';
        }
        return result;
    };
    const int year = digits(0, 4), month = digits(5, 2), day = digits(8, 2);
    const int hour = digits(11, 2), minute = digits(14, 2), second = digits(17, 2);
    const std::chrono::year_month_day civil{std::chrono::year(year), std::chrono::month(static_cast<unsigned>(month)),
        std::chrono::day(static_cast<unsigned>(day))};
    if (!civil.ok() || hour > 23 || minute > 59 || second > 59) throw std::runtime_error("invalid_quota_reset");
    std::size_t cursor = 19;
    std::int64_t fraction = 0;
    if (cursor < time.size() && time[cursor] == '.') {
        ++cursor;
        const auto begin = cursor;
        while (cursor < time.size() && time[cursor] >= '0' && time[cursor] <= '9') ++cursor;
        if (cursor == begin) throw std::runtime_error("invalid_quota_reset");
        for (std::size_t index = 0; index < 3; ++index) fraction = fraction * 10 + (begin + index < cursor ? time[begin + index] - '0' : 0);
    }
    std::int64_t offset = 0;
    if (cursor + 1 == time.size() && time[cursor] == 'Z') {}
    else
    if (cursor + 6 == time.size() && (time[cursor] == '+' || time[cursor] == '-') && time[cursor + 3] == ':') {
        const int hours = digits(cursor + 1, 2), minutes = digits(cursor + 4, 2);
        if (hours > 23 || minutes > 59) throw std::runtime_error("invalid_quota_reset");
        offset = (hours * 60 + minutes) * 60000ll * (time[cursor] == '+' ? 1 : -1);
    }
    else throw std::runtime_error("invalid_quota_reset");
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::sys_days(civil).time_since_epoch()).count() +
        (hour * 3600 + minute * 60 + second) * 1000ll + fraction - offset;
    if (milliseconds < 0) throw std::runtime_error("invalid_quota_reset");
    return static_cast<std::uint64_t>(milliseconds);
}

addon::Quota_window observation_window(const Json& value)
{
    const auto& percent = member(value, "used_percent");
    if (!percent.is_number() || percent.as_number() < 0 || !bounded_string(member(value, "window_label"), 128)) {
        throw std::runtime_error("invalid_quota_window");
    }
    addon::Quota_window result;
    result.used_percent = percent.as_number();
    result.window_minutes = static_cast<std::uint64_t>(integer(member(value, "window_minutes"), 1));
    result.label = text(value, "window_label");
    result.resets_at_ms = reset_time(member(value, "reset_at"));
    return result;
}

} // namespace

Json normalize_quota(const Json& response)
{
    if (integer(member(response, "code")) != 0 || !member(response, "data").is_object()) throw std::runtime_error("quota_request_rejected");
    const auto& data = member(response, "data");
    if (text(data, "kind") == "error") {
        if (text(data, "message") == "No token for \"kimi-code\". Run /login to authenticate.") {
            return object({{"status", Json("not_logged_in")}});
        }
        throw std::runtime_error("quota_unavailable");
    }
    if (text(data, "kind") != "ok") throw std::runtime_error("invalid_quota_response");
    auto result = object({{"status", Json("ok")}});
    // Kimi Code 2.0.2 reports quota.usages instead of summary and limits;
    // earlier releases, which the package still supports, send the latter.
    // A reply carrying quota is read as that shape alone, and it names both
    // windows: a missing one is a truncated reply, not an absent window.
    if (data.find("quota")) {
        const auto& usages = member(member(data, "quota"), "usages");
        if (!usages.is_object()) throw std::runtime_error("invalid_quota_usages");
        result.set("primary",   ratio_window(member(usages, "limit5h"), "5-hour", 300));
        result.set("secondary", ratio_window(member(usages, "limit7d"), "Weekly", 10080));
        return result;
    }
    if (!member(data, "summary").is_null()) result.set("secondary", window(member(data, "summary"), "Weekly", 10080));
    const auto& limits = member(data, "limits");
    if (!limits.is_null()) {
        if (!limits.is_array()) throw std::runtime_error("invalid_quota_limits");
        for (const auto& row : limits.as_array()) {
            if (!row.is_object()) throw std::runtime_error("invalid_quota_limit");
            const auto& period = member(row, "window");
            if (period.is_null()) continue;
            const auto duration = integer(member(period, "duration"), 1);
            if (!bounded_string(member(period, "unit"), 128)) throw std::runtime_error("invalid_quota_window");
            if (duration != 5 || text(period, "unit") != "hour") continue;
            if (result.find("primary")) throw std::runtime_error("duplicate_quota_window");
            result.set("primary", window(row, "5-hour", 300));
        }
    }
    if (!result.find("primary") && !result.find("secondary")) throw std::runtime_error("quota_window_unavailable");
    return result;
}

void handle_quota(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    if (context.request().operation == "prepare") {
        const auto request = addon::decode_probe_prepare_request(context.request().payload, &error, 2);
        if (!request) { context.fail(addon::Result_status::FAILED, error.reason_code, error.detail); return; }
        const auto args = literal_argv(request->context.launcher.script_body);
        if (args.empty() || !is_kimi_program(args.front())) {
            context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable");
            return;
        }
        auto environment = environment_pairs(request->context.launch_environment->effective_environment);
        const auto program = resolve_program(args.front(), environment, request->context.working_directory);
        if (!program || !native_image(*program)) {
            context.fail(addon::Result_status::UNAVAILABLE, "native_quota_program_required");
            return;
        }
        const auto home = path_string(profile_for_context(request->context));
        set_environment(environment, "KIMI_CODE_HOME", home);
        addon::Structured_prepare_result result;
#ifdef _WIN32
        result.process.executable = "helpers/kimi_quota_probe.exe";
#else
        result.process.executable = "helpers/kimi_quota_probe";
#endif
        result.process.argv = {"--kimi-program", path_string(*program), "--kimi-code-home", home};
        result.process.environment = std::move(environment);
        result.profile_storage = request->context.profile_storage;
        result.account_context = request->context.account_context;
        result.inspection.protocol_state = "await_exit";
        context.succeed(addon::encode_structured_prepare_result(result, 2));
        return;
    }
    if (context.request().operation != "inspect") { context.reject_unknown_operation(); return; }
    const auto request = addon::decode_probe_inspect_request(context.request().payload, &error, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, error.reason_code, error.detail); return; }
    if (!request->inspection.final) {
        addon::Quota_inspect_result result;
        result.protocol_state = "await_exit";
        context.succeed(addon::encode_quota_inspect_result(result, 2));
        return;
    }
    if (request->inspection.timed_out || request->inspection.exit_code != 0 ||
        request->inspection.stdout_bytes.empty() || request->inspection.stdout_bytes.size() > 65536) {
        context.fail(addon::Result_status::FAILED, request->inspection.timed_out ? "quota_timeout" : "quota_probe_failed");
        return;
    }
    const auto payload = parse(request->inspection.stdout_bytes);
    if (!payload || !payload->is_object()) { context.fail(addon::Result_status::FAILED, "invalid_quota_response"); return; }
    const auto status = text(*payload, "status");
    if (status != "ok" && status != "not_logged_in") { context.fail(addon::Result_status::FAILED, "quota_probe_failed"); return; }
    addon::Quota_observation observation;
    if (request->account_context) observation.scope.source_account_key = request->account_context->adapter_account_key;
    observation.scope.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    observation.scope.fresh_for_ms = 60000;
    observation.state = status == "ok" ? addon::Quota_status::READY : addon::Quota_status::NOT_LOGGED_IN;
    if (status == "not_logged_in" && (payload->find("primary") || payload->find("secondary"))) {
        throw std::runtime_error("invalid_quota_response");
    }
    if (payload->find("primary")) observation.primary = observation_window(member(*payload, "primary"));
    if (payload->find("secondary")) observation.secondary = observation_window(member(*payload, "secondary"));
    if (status == "ok" && !observation.primary && !observation.secondary) throw std::runtime_error("quota_window_unavailable");
    addon::Quota_inspect_result result;
    result.complete = true;
    result.observation = std::move(observation);
    context.succeed(addon::encode_quota_inspect_result(result, 2));
}

} // namespace kimi
