#include "kimi_acp.h"
#include "kimi_accounts.h"
#include "kimi_config.h"
#include "kimi_json.h"
#include "kimi_launcher.h"
#include "kimi_observer.h"
#include "kimi_preparation.h"
#include "kimi_quota.h"
#include "kimi_usage.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/legacy_state_import.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <exception>
#include <optional>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
using kimi::Json;

void transform_configuration(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_managed_configuration_transform_operation) {
        context.reject_unknown_operation(); return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_managed_configuration_transform_request(context.request().payload, &failure);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    auto transformed = kimi::updated_config(request->content);
    if (transformed.size() > addon::k_max_managed_configuration_bytes) {
        context.fail(addon::Result_status::UNAVAILABLE, "configuration_size_limit"); return;
    }
    context.succeed(addon::encode_managed_configuration_transform_result({request->grant, std::move(transformed)}));
}

void launcher(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == "discover") {
        const auto request = addon::decode_discover_request(context.request().payload, &failure);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
        const auto found = kimi::discover_launcher(*request);
        context.succeed(addon::encode_candidates(found ? std::vector<addon::Launcher_candidate>{*found} : std::vector<addon::Launcher_candidate>{}));
    }
    else
    if (context.request().operation == "recognize") {
        const auto request = addon::decode_recognize_request(context.request().payload, &failure);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
        addon::Launcher_recognition result;
        result.recognized = kimi::recognize_launcher(request->launcher.script_body);
        if (result.recognized) result.display_name = "Kimi";
        context.succeed(addon::encode_recognition(result));
    }
    else context.reject_unknown_operation();
}

void invoke(addon::Invocation_context& context, kimi::Lifecycle_observer& observer)
{
    try {
        if (context.cancel_requested()) { context.fail(addon::Result_status::CANCELLED, "cancelled"); return; }
        const auto& request = context.request();
        const auto& capability = request.capability.capability_id;
        if (capability == "launcher-discovery") launcher(context);
        else
        if (capability == "legacy-state-import") kimi::handle_legacy_data(context);
        else
        if (capability == "accounts") kimi::handle_accounts(context);
        else
        if (capability == "usage-scan") kimi::handle_usage(context);
        else
        if (capability == "managed-configuration") transform_configuration(context);
        else
        if (capability == "profile-storage" && request.operation == "acknowledge") {
            std::string error;
            const auto* value = request.payload.find("profile_storage");
            const auto storage = value ? addon::decode_profile_storage_context(*value, &error) : std::nullopt;
            if (!kimi::exact_keys(request.payload, {"profile_storage"}) || !storage) {
                context.fail(addon::Result_status::FAILED, "profile_storage_mismatch");
                return;
            }
            context.succeed(kimi::object({{"profile_storage", addon::profile_storage_member(*storage)}}));
        }
        else
        if (capability == "lifecycle-events" || capability == "helper-ingress" ||
            capability == "lifecycle-correlation-lifetime") observer.handle(context);
        else
        if (capability == "quota") kimi::handle_quota(context);
        else
        if (capability == "structured-session" && request.operation == "inspect") {
            addon::Contract_failure failure;
            const auto inspection = addon::decode_structured_inspect_request(request.payload, &failure, 2);
            if (!inspection) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
            if (inspection->purpose != addon::Structured_purpose::IMPLEMENTATION) {
                context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
                return;
            }
            auto payload = addon::encode_structured_inspect_result(kimi::inspect_acp(inspection->inspection), 2);
            if (!addon::validate_session_exchange_payload(payload, &failure)) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            context.succeed(std::move(payload));
        }
        else kimi::handle_preparation(context);
    }
    catch (const std::exception&) {
        // Configuration, prompt, tokens and private filesystem bytes never
        // enter host diagnostics.
        context.fail(addon::Result_status::FAILED, "kimi_operation_failed", "Kimi could not complete the requested operation.");
    }
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name = "kimi-addon";
    options.adapter_version = "1.0.0";
    options.capabilities = {
        {"presentation", 1}, {"launcher-discovery", 1}, {"profile-storage", 1},
        {"lifecycle-correlation-lifetime", 1},
        {"launch-preparation", 3}, {"structured-session", 2}, {"delegated-session", 2},
        {"interactive-resume", 2}, {"quota", 2}, {"lifecycle-events", 2},
        {"helper-ingress", 2}, {"coordination-delivery", 1}, {"legacy-state-import", 1},
        {"accounts", 2}, {"usage-scan", 2}, {"managed-configuration", 1},
    };
    kimi::Lifecycle_observer observer;
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler([&](auto& context) { invoke(context, observer); });
    return runtime.run();
}
