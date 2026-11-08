#include "kimi_accounts.h"
#include "kimi_execution_context.h"
#include "kimi_guidance.h"
#include "kimi_interactions.h"
#include "kimi_json.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace json_util = logonomic::cli_addon::json;

namespace {

namespace addon = logonomic::cli_addon;
using kimi::Json;

bool asks_changed_files(const Json& body)
{
    std::string prompt;
    for (const auto key : {"prompt", "user_prompt", "message"}) {
        if (json_util::member(body, key).is_string()) { prompt = kimi::lower(json_util::text(body, key)); break; }
    }
    bool presentation = false, changes = false;
    for (const auto verb : {"show", "open", "view", "display", "bring up", "pull up"}) {
        presentation = presentation || prompt.find(verb) != std::string::npos;
    }
    for (const auto phrase : {"changed file", "modified file", "edited file", "my changes", "your changes",
            "changes you made", "files you changed", "files you modified", "files you edited"}) {
        changes = changes || prompt.find(phrase) != std::string::npos;
    }
    return presentation && changes;
}

} // namespace

int main()
{
    addon::Contract_failure failure;
    const auto environment = addon::read_helper_environment(&failure);
    // Installed hook blocks also run when Kimi is launched outside Logonomic.
    if (!environment || environment->package_id != "logonomic.cli.kimi") return 0;
    std::string input;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = std::fread(buffer.data(), 1, buffer.size(), stdin);
        if (input.size() + count > 4 * 1024 * 1024) return 1;
        input.append(buffer.data(), count);
        if (count < buffer.size()) {
            if (std::ferror(stdin)) return 1;
            break;
        }
    }
    addon::Json_parse_limits input_limits;
    input_limits.max_string_bytes = 4 * 1024 * 1024;
    const auto payload = addon::parse_json(input, input_limits, nullptr);
    if (!payload || !payload->is_object()) return 0;
    auto name = json_util::text(*payload, "hook_event_name");
    if (name.empty()) name = json_util::text(*payload, "event");
    const bool permission = name == "PermissionRequest" || name == "PermissionResult";
    if (name != "SessionStart" && name != "SessionEnd" && name != "UserPromptSubmit" && !permission) {
        return 0;
    }
    const int budget_ms = name == "SessionStart" ? 16000 : name == "UserPromptSubmit" ? 21000 : 11000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    addon::Helper_client client(*environment, [&] { return std::chrono::steady_clock::now() >= deadline; });
    const auto id = addon::random_nonce_hex();
    const auto nonce = addon::random_nonce_hex();
    auto body = json_util::object({{"session_id", Json(json_util::text(*payload, "session_id"))},
        {"source", Json(json_util::text(*payload, "source"))}});
    if (name == "SessionEnd") {
        body.set("reason", Json(json_util::text(*payload, "reason")));
    }
    if (permission) {
        // Approval IDs pair native hooks without relaying commands or approval prose.
        for (const auto key : {"id", "agent_id", "tool_call_id"}) {
            const auto& identity = json_util::member(*payload, key);
            if (!json_util::bounded_string(identity, 256)) {
                return 1;
            }
            body.set(key, identity);
        }
    }
    std::filesystem::path guidance;
    try {
        addon::Launch_environment runtime;
        runtime.platform_id = addon::host_platform_id();
        for (const auto key : {"KIMI_CODE_HOME", "HOME", "USERPROFILE"}) {
            if (const auto* value = std::getenv(key)) runtime.effective_environment.set(key, Json(value));
        }
        const auto home = kimi::path_string(kimi::profile_for_environment(
            runtime, kimi::path_string(std::filesystem::current_path())));
        body.set("kimi_code_home", Json(home));
        if (name == "SessionStart") {
            addon::encode_execution_updates(body, {kimi::capture_session_start_context(
                addon::path_from_utf8(home), json_util::text(*payload, "session_id"), nonce,
                json_util::text(*payload, "cwd"))});
            try {
                body.set("interaction_frontier", kimi::capture_interaction_frontier(
                    home, json_util::text(*payload, "session_id")));
            }
            catch (const std::exception&) {
                // Optional wire access must not suppress authoritative session admission.
                // Null disables interactions for this admission without replaying history.
                body.set("interaction_frontier", Json());
                std::fputs("Logonomic native interaction observation is unavailable for this session.\n", stderr);
            }
        }
        guidance = kimi::session_guidance_path(addon::path_from_utf8(home), environment->generation_nonce,
            json_util::text(*payload, "session_id"));
    }
    catch (const std::exception&) { return 1; }
    auto result = client.deliver_provider_event(name, body, id, nonce);
    if (!result.committed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        result = client.deliver_provider_event(name, body, id, nonce);
    }
    if (result.committed) client.acknowledge_provider_event(id);
    if (name == "SessionStart" || name == "UserPromptSubmit") {
        const auto changed_files = name == "UserPromptSubmit" && asks_changed_files(*payload);
        auto title = client.request_title_instruction("kimi", name, changed_files);
        if (name == "UserPromptSubmit" && !title.completed && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            title = client.request_title_instruction("kimi", name, changed_files);
        }
        if (title.completed && title.accepted && !title.instruction.empty()) {
            try {
                const auto reminder = kimi::publish_session_guidance(guidance, title.instruction);
                std::fwrite(reminder.data(), 1, reminder.size(), stdout);
                std::fputc('\n', stdout);
            }
            catch (const std::exception&) {
                std::fputs("Logonomic session guidance could not be saved.\n", stderr);
                return 1;
            }
        }
    }
    if (name == "SessionEnd") {
        try { kimi::remove_session_guidance(guidance); }
        catch (const std::exception&) {
            std::fputs("Logonomic session guidance could not be removed.\n", stderr);
            return 1;
        }
    }
    return 0;
}
