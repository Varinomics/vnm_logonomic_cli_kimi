#include "kimi_resume.h"

#include "kimi_accounts.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;
namespace json_util = addon::json;
namespace fs = std::filesystem;

constexpr std::size_t k_workspace_bound = 4096;
constexpr std::size_t k_metadata_bound = 1024 * 1024;
constexpr std::size_t k_record_bound = 4 * 1024 * 1024;
constexpr std::size_t k_history_bound = 64 * 1024 * 1024;

bool valid_session_id(std::string_view id)
{
    const auto alphanumeric = [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9');
    };
    return !id.empty() && id.size() <= 256 && alphanumeric(id.front()) &&
        std::all_of(id.begin(), id.end(), [&](unsigned char byte) {
            return alphanumeric(byte) || byte == '_' || byte == '-';
        });
}

bool valid_home(std::string_view text)
{
    if (text.empty() || text.size() > 4096 || !addon::is_valid_utf8(text) ||
        std::any_of(text.begin(), text.end(), [](unsigned char byte) { return byte < 32 || byte == 127; }))
    {
        return false;
    }
    const auto path = addon::path_from_utf8(text);
    return path.is_absolute() && path == path.lexically_normal();
}

fs::file_status native_status(const fs::path& path)
{
    std::error_code error;
    const auto result = fs::symlink_status(addon::usage_cache_io_path(path), error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("native_session_unavailable");
    }
    return result;
}

bool native_directory(const fs::path& path)
{
    const auto entry = native_status(path);
    return fs::is_directory(entry) && !fs::is_symlink(entry);
}

std::optional<std::string> read_stable_file(const fs::path& path, std::size_t bound)
{
    if (!fs::is_regular_file(native_status(path))) {
        return std::nullopt;
    }
    const auto io_path = addon::usage_cache_io_path(path);
    const auto size = fs::file_size(io_path);
    const auto modified = fs::last_write_time(io_path);
    if (size > bound) {
        return std::nullopt;
    }
    std::ifstream input(io_path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()) || input.bad() ||
        input.peek() != std::char_traits<char>::eof() ||
        !fs::is_regular_file(native_status(path)) || fs::file_size(io_path) != size ||
        fs::last_write_time(io_path) != modified)
    {
        return std::nullopt;
    }
    return bytes;
}

bool native_metadata(const Json& value, std::string_view session)
{
    const auto* version = value.find("version");
    const auto* created = value.find("createdAt");
    const auto* updated = value.find("updatedAt");
    const auto* archived = value.find("archived");
    const auto* agents = value.find("agents");
    const auto* custom = value.find("custom");
    return version && version->is_integer() && version->as_integer() == 2 &&
        json_util::text(value, "id") == session &&
        created && created->is_number() && created->as_number() >= 0 &&
        updated && updated->is_number() && updated->as_number() >= 0 &&
        archived && archived->is_boolean() && (!agents || agents->is_object()) &&
        (!custom || custom->is_object());
}

bool root_history(const Json& metadata)
{
    if (metadata.find("forkedFrom")) {
        return false;
    }
    const auto* agents = metadata.find("agents");
    if (!agents) {
        return false;
    }
    for (const auto& agent : agents->as_object()) {
        if (agent.name != "main" || !agent.value.is_object() ||
            agent.value.find("forkedFrom") ||
            (agent.value.find("type") && json_util::text(agent.value, "type") != "main"))
        {
            return false;
        }
        const auto* parent = agent.value.find("parentAgentId");
        if (parent && !parent->is_null()) {
            return false;
        }
    }
    return true;
}

bool supported_wire_version(std::string_view text, bool minimum)
{
    if (text.empty() || text.size() > 32) {
        return false;
    }
    std::vector<unsigned> parts;
    unsigned part = 0;
    bool digit = false;
    for (const unsigned char byte : text) {
        if (byte >= '0' && byte <= '9' && part <= 100000) {
            part = part * 10 + byte - '0';
            digit = true;
        }
        else
        if (byte == '.' && digit) {
            parts.push_back(part);
            part = 0;
            digit = false;
        }
        else {
            return false;
        }
    }
    if (!digit) {
        return false;
    }
    parts.push_back(part);
    while (parts.size() > 2 && parts.back() == 0) {
        parts.pop_back();
    }
    parts.resize(std::max<std::size_t>(2, parts.size()));
    const std::vector<unsigned> supported{1, 5};
    return minimum ? parts <= supported : parts == supported;
}

bool wire_metadata(const Json& value)
{
    const auto* created = value.find("created_at");
    const auto* minimum = value.find("min_protocol_version");
    return json_util::text(value, "type") == "metadata" &&
        supported_wire_version(json_util::text(value, "protocol_version"), false) &&
        created && created->is_number() && created->as_number() >= 0 &&
        (!minimum || (minimum->is_string() && supported_wire_version(minimum->as_string(), true)));
}

std::optional<bool> text_content(const Json& parts)
{
    if (!parts.is_array()) {
        return std::nullopt;
    }
    bool nonempty = false;
    for (const auto& part : parts.as_array()) {
        const auto* text = part.find("text");
        if (json_util::text(part, "type") != "text" || !text || !text->is_string()) {
            // Blob-backed attachments and unfamiliar content need their own
            // replay contract; their presence must never establish EMPTY.
            return std::nullopt;
        }
        nonempty = nonempty || !text->as_string().empty();
    }
    return nonempty;
}

std::optional<bool> message_content(const Json& message)
{
    const auto* parts = message.find("content");
    const auto* partial = message.find("partial");
    if (!parts || (partial && (!partial->is_boolean() || partial->as_boolean()))) {
        return std::nullopt;
    }
    const auto content = text_content(*parts);
    if (!content) {
        return std::nullopt;
    }
    const auto role = json_util::text(message, "role");
    if (role != "user" && role != "assistant" && role != "tool" && role != "system") {
        return std::nullopt;
    }
    bool calls = false;
    if (const auto* tool_calls = message.find("toolCalls")) {
        if (!tool_calls->is_array()) {
            return std::nullopt;
        }
        for (const auto& call : tool_calls->as_array()) {
            if (json_util::text(call, "type") != "function" || json_util::text(call, "id").empty() ||
                json_util::text(call, "name").empty())
            {
                return std::nullopt;
            }
            calls = true;
        }
    }
    if (const auto* origin = message.find("origin")) {
        const auto kind = json_util::text(*origin, "kind");
        if (kind == "injection" && role == "user" && !calls) {
            const auto variant = json_util::text(*origin, "variant");
            if (variant == "project_context" || variant == "environment_binding" ||
                variant == "date_change" || variant == "permission_mode")
            {
                return false;
            }
            return std::nullopt;
        }
        if (kind != "user" || origin->find("attachments")) {
            return std::nullopt;
        }
    }
    return role != "system" && (*content || calls);
}

bool string_array(const Json& value)
{
    return value.is_array() && std::all_of(value.as_array().begin(), value.as_array().end(),
        [](const auto& entry) { return entry.is_string(); });
}

bool string_map(const Json& value)
{
    return value.is_object() && std::all_of(value.as_object().begin(), value.as_object().end(),
        [](const auto& entry) { return entry.value.is_string(); });
}

bool initialization_record(const Json& value)
{
    const auto* time = value.find("time");
    if (json_util::text(value, "agentId") != "main" || (time && !time->is_number())) {
        return false;
    }
    const auto type = json_util::text(value, "type");
    if (type == "environment.set_binding") {
        const auto* cwd = value.find("cwd");
        return !json_util::text(value, "environmentId").empty() && (!cwd || cwd->is_string());
    }
    if (type == "profile.bind") {
        if (!json_util::member(value, "systemPrompt").is_string() ||
            !json_util::member(value, "thinkingEffort").is_string() ||
            !string_array(json_util::member(value, "disallowedTools")))
        {
            return false;
        }
        for (const auto key : {"modelAlias", "profileName"}) {
            if (const auto* field = value.find(key); field && !field->is_string()) {
                return false;
            }
        }
        for (const auto key : {"agentsMdPaths", "activeToolNames", "subagents"}) {
            if (const auto* field = value.find(key); field && !string_array(*field)) {
                return false;
            }
        }
        const auto* disclosure = value.find("environmentDisclosure");
        const auto* generation = value.find("renderGeneration");
        return (!disclosure || disclosure->is_object()) && (!generation || generation->is_number());
    }
    if (type == "changeNotifier.snapshot") {
        for (const auto key : {"agentsMdHash", "skillsHash"}) {
            const auto* field = value.find(key);
            if (!field || (!field->is_null() && !field->is_string())) {
                return false;
            }
        }
        const auto* subagents = value.find("subagentNames");
        if (!subagents || (!subagents->is_null() && !string_array(*subagents))) {
            return false;
        }
        for (const auto key : {"agentsMdFiles", "workspacePrimaryByWorkspace"}) {
            if (const auto* field = value.find(key); field && !field->is_null() && !string_map(*field)) {
                return false;
            }
        }
        if (const auto* directories = value.find("workspaceAdditionalDirs");
            directories && !directories->is_null() && !string_array(*directories))
        {
            return false;
        }
        if (const auto* pool = value.find("modelPool"); pool && !pool->is_null()) {
            const auto* default_model = pool->find("defaultModel");
            if (!string_map(json_util::member(*pool, "models")) ||
                (default_model && !default_model->is_string()))
            {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool nonnegative_number(const Json& value)
{
    return value.is_number() && value.as_number() >= 0;
}

bool token_usage(const Json& value)
{
    for (const auto* key : {"inputOther", "output", "inputCacheCreation"}) {
        if (!nonnegative_number(json_util::member(value, key))) {
            return false;
        }
    }
    const auto* cached = value.find("inputCacheRead");
    const auto* raw = value.find("raw");
    return (!cached || nonnegative_number(*cached)) && (!raw || raw->is_object());
}

bool mirrored_message(const Json& entry)
{
    const auto& message = json_util::member(entry, "message");
    const auto role = json_util::text(message, "role");
    if ((role != "user" && role != "assistant") || !message_content(message) ||
        (role == "assistant" && !json_util::member(message, "toolCalls").is_array()))
    {
        return false;
    }
    const auto* meta = entry.find("meta");
    if (!meta) {
        return true;
    }
    if (!meta->is_object()) {
        return false;
    }
    for (const auto* key : {"source", "key", "promptId", "createdAt", "userMessageId", "messageId"}) {
        const auto* field = meta->find(key);
        if (field && !field->is_string()) {
            return false;
        }
    }
    const auto* tracked = meta->find("tracked");
    const auto* origin = meta->find("origin");
    if ((tracked && !tracked->is_boolean()) ||
        (origin && json_util::text(*origin, "kind") != "user"))
    {
        return false;
    }
    if (role == "assistant") {
        if (!token_usage(json_util::member(*meta, "usage"))) {
            return false;
        }
        if (const auto* model = meta->find("model"); model &&
            (!json_util::member(*model, "provider").is_string() ||
             !json_util::member(*model, "model").is_string()))
        {
            return false;
        }
        if (const auto* headers = meta->find("headers"); headers && !string_map(*headers)) {
            return false;
        }
        if (const auto* finish = meta->find("finish")) {
            const auto& reason = json_util::member(*finish, "finishReason");
            const auto& raw = json_util::member(*finish, "rawFinishReason");
            const auto reason_text = json_util::text(*finish, "finishReason");
            if (!finish->is_object() || !finish->find("finishReason") ||
                !finish->find("rawFinishReason") || (!raw.is_null() && !raw.is_string()) ||
                (!reason.is_null() && reason_text != "completed" && reason_text != "tool_calls" &&
                 reason_text != "truncated" && reason_text != "filtered" && reason_text != "paused" &&
                 reason_text != "other"))
            {
                return false;
            }
        }
    }
    return true;
}

bool tool_snapshot(const Json& tools, bool selected)
{
    if (!tools.is_array()) {
        return false;
    }
    for (const auto& tool : tools.as_array()) {
        if (!json_util::member(tool, "name").is_string() ||
            !json_util::member(tool, "description").is_string() ||
            !json_util::member(tool, "parameters").is_object())
        {
            return false;
        }
        if (selected) {
            const auto source = json_util::text(tool, "source");
            const auto origin = json_util::text(tool, "origin");
            const auto* disclosure = tool.find("disclosure");
            if ((source != "builtin" && source != "user" && source != "mcp") ||
                (origin != "initial" && origin != "runtime") ||
                (disclosure && json_util::text(tool, "disclosure") != "inline" &&
                 json_util::text(tool, "disclosure") != "deferred"))
            {
                return false;
            }
        }
    }
    return true;
}

bool routine_record(const Json& value)
{
    // Kimi 2.5.2's replayBuilder/fold.ts does not edit retained context for
    // these records. Validate their native schemas, but never count mirrored
    // UI messages, model requests or token counters as retained conversation.
    const auto type = json_util::text(value, "type");
    if (!nonnegative_number(json_util::member(value, "time"))) {
        return false;
    }
    if (type == "agent.message.appended" || type == "agent.turn.started" || type == "agent.turn.ended") {
        if (json_util::text(value, "kind") != "event") {
            return false;
        }
        if (type == "agent.message.appended") {
            return mirrored_message(json_util::member(value, "message"));
        }
        const auto& turn = json_util::member(value, "turnId");
        if (!turn.is_integer() || turn.as_integer() < 0) {
            return false;
        }
        if (type == "agent.turn.started") {
            const auto* item = value.find("queueItemId");
            return !item || item->is_string();
        }
        const auto outcome = json_util::text(value, "outcome");
        const auto* error = value.find("errorMessage");
        return (outcome == "done" || outcome == "failed" || outcome == "aborted") &&
            (!error || error->is_string());
    }
    if (json_util::text(value, "agentId") != "main") {
        return false;
    }
    if (type == "plugin.session_start") {
        const auto* content = value.find("content");
        return content && (content->is_null() || content->is_string());
    }
    if (type == "tool_select.initialized" || type == "llm.tools_snapshot") {
        return tool_snapshot(json_util::member(value, "tools"), type == "tool_select.initialized") &&
            (type != "llm.tools_snapshot" || json_util::member(value, "hash").is_string());
    }
    if (type == "llm.request") {
        if (json_util::text(value, "kind") != "loop" ||
            !json_util::member(value, "toolSelect").is_boolean() ||
            !nonnegative_number(json_util::member(value, "messageCount")))
        {
            return false;
        }
        for (const auto* key : {"provider", "model", "systemPromptHash", "toolsHash"}) {
            if (!json_util::member(value, key).is_string()) {
                return false;
            }
        }
        for (const auto* key : {"modelAlias", "thinkingEffort", "thinkingKeep", "systemPrompt", "turnStep", "attempt"}) {
            const auto* field = value.find(key);
            if (field && !field->is_string()) {
                return false;
            }
        }
        for (const auto* key : {"temperature", "topP", "maxTokens", "droppedCount"}) {
            const auto* field = value.find(key);
            if (field && !field->is_number()) {
                return false;
            }
        }
        const auto* beta = value.find("betaApi");
        if (beta && !beta->is_boolean()) {
            return false;
        }
        const auto projection = json_util::text(value, "projection");
        return !value.find("projection") || projection == "strict" || projection == "media-degraded" ||
            projection == "media-stripped" || projection == "strict-media-degraded" ||
            projection == "strict-media-stripped";
    }
    if (type == "turn.ended") {
        const auto reason = json_util::text(value, "reason");
        const auto* duration = value.find("durationMs");
        const auto* error = value.find("error");
        const auto* stop = value.find("stopReason");
        const auto* request = value.find("requestId");
        return nonnegative_number(json_util::member(value, "turnId")) &&
            (reason == "completed" || reason == "cancelled" || reason == "failed" || reason == "blocked") &&
            (!duration || nonnegative_number(*duration)) && (!error || error->is_object()) &&
            (!stop || stop->is_string()) && (!request || request->is_string());
    }
    if (type == "token_counting.turn_recorded") {
        return nonnegative_number(json_util::member(value, "turnId")) &&
            nonnegative_number(json_util::member(value, "length")) &&
            nonnegative_number(json_util::member(value, "tokens"));
    }
    if (type == "prompt.completed") {
        const auto reason = json_util::text(value, "reason");
        return !json_util::text(value, "promptId").empty() &&
            json_util::member(value, "finishedAt").is_string() &&
            (reason == "completed" || reason == "failed" || reason == "blocked");
    }
    return false;
}

struct History_scan
{
    bool nonempty = false;
    bool activity = false;
    std::map<std::string, bool> open_steps;
    std::set<std::string> pending_tools;

    bool loop_event(const Json& event)
    {
        activity = true;
        const auto type = json_util::text(event, "type");
        if (type == "step.begin") {
            const auto uuid = json_util::text(event, "uuid");
            return !uuid.empty() && open_steps.emplace(uuid, false).second;
        }
        if (type == "step.end") {
            const auto found = open_steps.find(json_util::text(event, "uuid"));
            if (found == open_steps.end()) {
                return false;
            }
            nonempty = nonempty || found->second;
            open_steps.erase(found);
            return true;
        }
        if (type == "tool.result") {
            const auto* result = event.find("result");
            const auto* output = result ? result->find("output") : nullptr;
            if (!output || (!output->is_string() && !text_content(*output)) ||
                pending_tools.erase(json_util::text(event, "toolCallId")) != 1)
            {
                return false;
            }
            return true;
        }
        const auto found = open_steps.find(json_util::text(event, "stepUuid"));
        if (found == open_steps.end()) {
            return false;
        }
        if (type == "content.part") {
            const auto* part = event.find("part");
            const auto* text = part ? part->find("text") : nullptr;
            if (!part || json_util::text(*part, "type") != "text" || !text || !text->is_string()) {
                return false;
            }
            found->second = found->second || !text->as_string().empty();
            return true;
        }
        if (type == "tool.call") {
            const auto id = json_util::text(event, "toolCallId");
            if (id.empty() || json_util::text(event, "name").empty() || !pending_tools.insert(id).second) {
                return false;
            }
            found->second = true;
            return true;
        }
        return false;
    }

    bool record(const Json& value)
    {
        const auto type = json_util::text(value, "type");
        if (type == "context.append_message") {
            const auto* message = value.find("message");
            const auto content = message ? message_content(*message) : std::nullopt;
            if (!content) {
                return false;
            }
            nonempty = nonempty || *content;
            return true;
        }
        if (type == "context.append_loop_event") {
            const auto* event = value.find("event");
            return event && loop_event(*event);
        }
        // These records do not edit the retained message sequence. A turn or
        // usage without understood message content remains UNKNOWN, not empty.
        if (type == "turn.prompt" || type == "turn.steer" || type == "turn.cancel" || type == "usage.record") {
            activity = true;
            return true;
        }
        if (type == "environment.set_binding" || type == "profile.bind" || type == "changeNotifier.snapshot") {
            return initialization_record(value);
        }
        if (routine_record(value)) {
            activity = true;
            return true;
        }
        return type == "config.update" || type == "permission.set_mode" ||
            type == "permission.record_approval_result" || type == "tools.set_active_tools" ||
            type == "tools.update_store" || type == "plan_mode.enter" ||
            type == "plan_mode.cancel" || type == "plan_mode.exit";
    }
};

addon::Resume_content inspect_content(const Native_session_lookup& native,
    addon::Invocation_context& context)
{
    using Content = addon::Resume_content;
    if (!native.metadata || !root_history(*native.metadata) ||
        !native_directory(native.session_directory / "agents") ||
        !native_directory(native.session_directory / "agents/main"))
    {
        return Content::UNKNOWN;
    }
    const auto wire = native.session_directory / "agents/main/wire.jsonl";
    const auto bytes = read_stable_file(wire, k_history_bound);
    if (!bytes || bytes->empty() || bytes->back() != '\n') {
        return Content::UNKNOWN;
    }
    addon::Json_parse_limits limits;
    limits.max_bytes = k_record_bound;
    limits.max_string_bytes = k_record_bound;
    History_scan history;
    std::size_t begin = 0;
    while (begin < bytes->size()) {
        if (context.cancel_requested()) {
            return Content::UNKNOWN;
        }
        const auto end = bytes->find('\n', begin);
        const auto record = addon::parse_json(std::string_view(*bytes).substr(begin, end - begin), limits, nullptr);
        if (!record || (begin == 0 ? !wire_metadata(*record) : !history.record(*record))) {
            return Content::UNKNOWN;
        }
        begin = end + 1;
    }
    // Reopen the exact path, catching replacement as well as an append/truncate
    // during parsing. Partial or moving evidence never supplies an empty proof.
    if (!history.open_steps.empty() || !history.pending_tools.empty() ||
        read_stable_file(wire, k_history_bound) != bytes)
    {
        return Content::UNKNOWN;
    }
    if (history.nonempty) {
        return Content::NONEMPTY;
    }
    if (history.activity || !json_util::text(*native.metadata, "lastPrompt").empty()) {
        return Content::UNKNOWN;
    }
    return Content::EMPTY;
}

} // namespace

Json make_resume_record(const fs::path& home, std::string_view session)
{
    const auto selected = addon::path_to_utf8(home.lexically_normal());
    if (!valid_home(selected) || !valid_session_id(session)) {
        throw std::runtime_error("invalid_resume_record");
    }
    return json_util::object({{"provider_session_id", Json(session)},
        {"native_store", json_util::object({{"version", Json(1)}, {"kimi_code_home", Json(selected)}})}});
}

std::optional<Resume_record> decode_kimi_resume_record(const Json& record)
{
    const auto id = json_util::text(record, "provider_session_id");
    if (!valid_session_id(id)) {
        return std::nullopt;
    }
    if (json_util::exact_keys(record, {"provider_session_id"})) {
        return Resume_record{id, std::nullopt};
    }
    const auto* store = record.find("native_store");
    if (!json_util::exact_keys(record, {"provider_session_id", "native_store"}) || !store ||
        !json_util::exact_keys(*store, {"version", "kimi_code_home"}))
    {
        return std::nullopt;
    }
    const auto* version = store->find("version");
    const auto home = json_util::text(*store, "kimi_code_home");
    if (!version || !version->is_integer() || version->as_integer() != 1 || !valid_home(home)) {
        return std::nullopt;
    }
    return Resume_record{id, addon::path_from_utf8(home)};
}

bool resume_scope_matches(const Resume_record& record, const addon::Session_launch_context& context)
{
    if (!record.kimi_code_home || !context.launch_environment) {
        return false;
    }
    try {
        const auto home = profile_for_environment(*context.launch_environment, context.working_directory);
        const auto key = account_key(home);
        return account_key(*record.kimi_code_home) == key &&
            (!context.account_context || (context.account_context->profile_path &&
                context.account_context->adapter_account_key == key &&
                account_key(addon::path_from_utf8(*context.account_context->profile_path)) == key));
    }
    catch (const std::exception&) {
        return false;
    }
}

Native_session_lookup lookup_native_session(const fs::path& home, std::string_view session)
{
    try {
        const auto io_home = addon::usage_cache_io_path(home);
        if (!home.is_absolute() || !valid_session_id(session) || !fs::is_directory(io_home)) {
            return {};
        }
        const auto canonical_home = fs::canonical(io_home);
        const auto sessions = home / "sessions";
        if (!native_directory(sessions)) {
            return {};
        }
        const auto io_sessions = addon::usage_cache_io_path(sessions);
        const auto initial_time = fs::last_write_time(io_sessions);
        std::vector<std::pair<fs::path, fs::file_time_type>> workspaces;
        fs::path selected;
        std::size_t entries = 0;
        // Native sessionIndexSource reads this exact path; session_index.jsonl
        // and the optional database are hints/projections. Only workspace
        // buckets are enumerated, never the other conversations' histories.
        for (const auto& entry : fs::directory_iterator(io_sessions)) {
            if (++entries > k_workspace_bound) {
                return {};
            }
            const auto name = addon::path_to_utf8(entry.path().filename());
            if (name == ".index-cache" || name == ".index-dirty") {
                continue;
            }
            // Extended Windows spelling is an I/O detail, never native scope.
            const auto workspace = sessions / entry.path().filename();
            const auto kind = native_status(workspace);
            if (fs::is_symlink(kind)) {
                return {};
            }
            if (!fs::is_directory(kind)) {
                continue;
            }
            workspaces.emplace_back(workspace, fs::last_write_time(addon::usage_cache_io_path(workspace)));
            const auto candidate = workspace / addon::path_from_utf8(session);
            const auto candidate_kind = native_status(candidate);
            if (!fs::exists(candidate_kind)) {
                continue;
            }
            if (!fs::is_directory(candidate_kind) || fs::is_symlink(candidate_kind)) {
                return {};
            }
            if (!selected.empty()) {
                Native_session_lookup ambiguous;
                ambiguous.selector_ambiguous = true;
                return ambiguous;
            }
            selected = candidate;
        }
        Native_session_lookup result;
        if (selected.empty()) {
            result.availability = addon::Resume_availability::ABSENT;
        }
        else {
            const auto bytes = read_stable_file(selected / "state.json", k_metadata_bound);
            const auto metadata = bytes ? addon::parse_json(*bytes, {}, nullptr) : std::nullopt;
            if (!metadata || !native_metadata(*metadata, session)) {
                return {};
            }
            result.availability = addon::Resume_availability::AVAILABLE;
            result.session_directory = selected;
            result.metadata = *metadata;
        }
        if (fs::canonical(io_home) != canonical_home || fs::last_write_time(io_sessions) != initial_time) {
            return {};
        }
        for (const auto& [workspace, modified] : workspaces) {
            if (!native_directory(workspace) ||
                fs::last_write_time(addon::usage_cache_io_path(workspace)) != modified)
            {
                return {};
            }
        }
        return result;
    }
    catch (const std::exception&) {
        return {};
    }
}

void handle_resume_inspection(addon::Invocation_context& context)
{
    const auto version = context.request().capability.version;
    if (context.request().operation != "inspect_resume" || (version != 3 && version != 4)) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_resume_inspect_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    addon::Resume_inspect_result result;
    result.request_digest_hex = addon::resume_inspect_request_digest_hex(*request);
    const auto record = decode_kimi_resume_record(request->resume_record);
    if (record && resume_scope_matches(*record, request->context)) {
        const auto native = lookup_native_session(*record->kimi_code_home, record->provider_session_id);
        result.availability = native.availability;
        if (version == 4 && native.availability == addon::Resume_availability::AVAILABLE) {
            try {
                result.content = inspect_content(native, context);
                const auto after = lookup_native_session(*record->kimi_code_home, record->provider_session_id);
                if (after.availability != native.availability || after.session_directory != native.session_directory ||
                    after.metadata != native.metadata)
                {
                    result.availability = addon::Resume_availability::UNKNOWN;
                    result.content = addon::Resume_content::UNKNOWN;
                }
            }
            catch (const std::exception&) {
                result.content = addon::Resume_content::UNKNOWN;
            }
        }
    }
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    context.succeed(addon::encode_resume_inspect_result(result, version));
}

} // namespace kimi
