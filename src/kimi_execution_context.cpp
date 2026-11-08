#include "kimi_execution_context.h"

#include "kimi_json.h"
#include "kimi_resume.h"

#include <exception>

namespace json_util = logonomic::cli_addon::json;

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;

constexpr auto k_session_start_source = "SessionStart:session-environment";

} // namespace

addon::Execution_context_update capture_session_start_context(
    const std::filesystem::path& home, std::string_view session,
    std::string_view transition_nonce, std::string_view native_cwd)
{
    addon::Execution_context_update update;
    update.provider_session_id = session;
    update.transition_nonce    = transition_nonce;
    update.source              = k_session_start_source;
    try {
        const auto native = lookup_native_session(home, session);
        if (!native.metadata) {
            return update;
        }
        const auto* environment = native.metadata->find("environmentId");
        if (environment && (!environment->is_string() || environment->as_string().empty())) {
            return update;
        }
        // Native SessionStart.cwd is a local carrier path for remote sessions.
        // SessionLifecycleService persists the environment before announcing
        // creation; do not publish that carrier as a remote execution default.
        if (environment && environment->as_string() != "local") {
            update.status      = addon::Execution_context_status::UNAVAILABLE;
            update.environment = "unsupported";
            return update;
        }
        const auto* effective_cwd = native.metadata->find("environmentCwd");
        if (!effective_cwd) {
            effective_cwd = native.metadata->find("cwd");
        }
        if (!effective_cwd || !effective_cwd->is_string() ||
            effective_cwd->as_string() != native_cwd || !addon::is_execution_local_path(native_cwd))
        {
            return update;
        }
        update.status      = addon::Execution_context_status::CONFIRMED;
        update.environment = "local";
        update.cwd         = native_cwd;
    }
    catch (const std::exception&) {
        // Optional context evidence cannot suppress authoritative identity.
    }
    return update;
}

void classify_session_start_context(const addon::Classify_event_request& request,
    addon::Classify_event_result& result)
{
    if (request.execution_failure) {
        result.execution_failure = request.execution_failure;
        return;
    }
    if (request.execution_context_version != 1) {
        return;
    }
    addon::decode_execution_updates(request.event.body, result.execution_updates, result.execution_failure);
    if (result.execution_failure || result.execution_updates.empty()) {
        return;
    }
    const auto& update = result.execution_updates.front();
    if (result.execution_updates.size() != 1 ||
        update.provider_session_id != json_util::text(request.event.body, "session_id") ||
        update.transition_nonce != request.transition_nonce || update.sequence != 0 ||
        update.source != k_session_start_source)
    {
        result.execution_updates.clear();
        result.execution_failure = addon::Contract_failure{
            "execution_context_source_mismatch", "SessionStart context does not match its native activation"};
    }
}

} // namespace kimi
