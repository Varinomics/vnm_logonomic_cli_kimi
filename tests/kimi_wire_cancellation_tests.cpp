#include "kimi_wire.h"
#include "kimi_json.h"

#include <logonomic_cli_addon/lifecycle_events.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

namespace {

namespace addon = logonomic::cli_addon;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

kimi::Json json(std::string_view text)
{
    const auto result = json_util::parse(text);
    require(result.has_value(), "native wire fixture must be valid JSON");
    return *result;
}

void stopped(const std::vector<addon::Lifecycle_observation>& events, const std::string& id)
{
    require(events.size() == 1 && events.front().agent_id == id &&
        events.front().subagent_transition == addon::Subagent_transition::STOPPED,
        "terminal event must stop exactly its correlated public agent");
}

void launch(kimi::Wire_decoder& decoder, const std::string& call, const std::string& task)
{
    const auto events = decoder.accept(json(
        "{\"type\":\"context.append_loop_event\",\"event\":{\"type\":\"tool.call\","
        "\"name\":\"Agent\",\"toolCallId\":\"" + call + "\","
        "\"args\":{\"run_in_background\":true,\"subagent_type\":\"reviewer\"}}}"));
    require(events.size() == 1 && events.front().agent_id == call &&
        events.front().subagent_transition == addon::Subagent_transition::STARTED,
        "background Agent call must start one public agent");
    require(decoder.accept(json(
        "{\"type\":\"subagent.spawned\",\"subagentId\":\"worker\","
        "\"parentToolCallId\":\"" + call + "\",\"runInBackground\":true,"
        "\"taskId\":\"" + task + "\"}")).empty(), "spawn must retain the tool-call identity");
    require(decoder.accept(json(R"({"type":"subagent.started","subagentId":"worker"})")).empty(),
        "correlated native start must not start a second public agent");
}

kimi::Json background_reply(const std::string& call, const std::string& task)
{
    return json(
        "{\"type\":\"context.append_loop_event\",\"event\":{\"type\":\"tool.result\","
        "\"toolCallId\":\"" + call + "\",\"result\":{\"output\":\"task_id: " + task +
        "\\nstatus: running\\n\\nAgent continues in the background\"}}}");
}

kimi::Json notification(const std::string& kind, const std::string& task, const std::string& status)
{
    return json(
        "{\"type\":\"context.append_message\",\"message\":{\"origin\":{\"kind\":\"" + kind +
        "\",\"taskId\":\"" + task + "\",\"status\":\"" + status + "\"}}}");
}

void cancellation_and_resume()
{
    // Native mirrorAgentRun emits durable subagent.cancelled for an aborted
    // background run; a later Agent(resume=...) creates another tool call.
    kimi::Wire_decoder decoder("session-cancellation");
    launch(decoder, "first-call", "first-task");
    const auto reply = background_reply("first-call", "first-task");
    require(decoder.accept(reply).empty(), "running reply must leave the background agent active");
    const auto cancelled = json(R"({"type":"subagent.cancelled","subagentId":"worker"})");
    const auto events = decoder.accept(cancelled);
    stopped(events, "first-call");
    require(events.front().agent_label == "reviewer", "cancellation must retain the public agent label");
    require(decoder.accept(cancelled).empty(), "duplicate cancellation must not stop twice");

    decoder.rewind();
    require(decoder.accept(json(
        R"({"type":"context.append_loop_event","event":{"type":"tool.call","name":"Agent","toolCallId":"first-call"}})")).empty(),
        "wire replay must not revive a cancelled tool call");
    require(decoder.accept(json(
        R"({"type":"subagent.spawned","subagentId":"worker","parentToolCallId":"first-call"})")).empty(),
        "wire replay must retain the cancelled run correlation");
    require(decoder.accept(json(R"({"type":"subagent.started","subagentId":"worker"})")).empty(),
        "late start must not revive a cancelled run");
    require(decoder.accept(cancelled).empty(), "replayed cancellation must be idempotent");

    launch(decoder, "resumed-call", "resumed-task");
    require(decoder.accept(background_reply("resumed-call", "resumed-task")).empty(),
        "resumed background reply must leave its new run active");
    require(decoder.accept(notification("task", "first-task", "killed")).empty(),
        "delayed notification from the cancelled task must not stop its resumed run");
    stopped(decoder.accept(cancelled), "resumed-call");
    require(decoder.close("SessionEnd").empty(), "both cancelled runs must be retired");
}

void cancellation_before_reply()
{
    kimi::Wire_decoder decoder("session-early-cancellation");
    launch(decoder, "early-call", "early-task");
    stopped(decoder.accept(json(R"({"type":"subagent.cancelled","subagentId":"worker"})")), "early-call");
    require(decoder.accept(background_reply("early-call", "early-task")).empty(),
        "a running reply received after cancellation must not revive the call");
    require(decoder.close("SessionEnd").empty(), "early cancellation must retire the public agent");
}

void terminal_notifications()
{
    // TaskService uses origin.kind=task and its terminal task statuses. Retained
    // wire logs also contain the background_task notification contract.
    for (const auto* kind : {"task", "background_task"}) {
        for (const auto* status : {"completed", "failed", "cancelled", "killed", "timed_out", "lost"}) {
            kimi::Wire_decoder decoder("session-task-notification");
            launch(decoder, "background-call", "background-task");
            require(decoder.accept(background_reply("background-call", "background-task")).empty(),
                "background reply must leave the task active");
            require(decoder.accept(notification(kind, "unrelated-task", status)).empty(),
                "another task's notification must not stop this agent");
            require(decoder.accept(notification(kind, "background-task", "running")).empty(),
                "running task notification must leave the agent active");
            const auto terminal = notification(kind, "background-task", status);
            stopped(decoder.accept(terminal), "background-call");
            require(decoder.accept(terminal).empty(), "duplicate terminal notification must be idempotent");
            require(decoder.accept(json(R"({"type":"subagent.cancelled","subagentId":"worker"})")).empty(),
                "native cancellation after the task notification must not stop twice");
            require(decoder.close("SessionEnd").empty(), "terminal notification must retire the agent");
        }
    }
}

void independent_cancellation()
{
    kimi::Wire_decoder decoder("session-native-agent");
    const auto start = json(R"({"type":"subagent.started","subagentId":"native-worker"})");
    const auto cancelled = json(R"({"type":"subagent.cancelled","subagentId":"native-worker"})");
    require(decoder.accept(start).size() == 1, "uncorrelated native run must start");
    stopped(decoder.accept(cancelled), "native-worker");
    require(decoder.accept(cancelled).empty(), "uncorrelated cancellation must not stop twice");
    require(decoder.accept(start).empty(), "late start must not revive the cancelled native run");
    require(decoder.close("SessionEnd").empty(), "uncorrelated cancellation must retire the native agent");
}

void capacity_preserves_admitted_finishes()
{
    kimi::Wire_decoder decoder("session-capacity");
    const auto call = [](const std::string& id) {
        return json("{\"type\":\"context.append_loop_event\",\"event\":{\"type\":\"tool.call\","
            "\"name\":\"Agent\",\"toolCallId\":\"" + id + "\"}}");
    };
    const auto result = [](const std::string& id) {
        return json("{\"type\":\"context.append_loop_event\",\"event\":{\"type\":\"tool.result\","
            "\"toolCallId\":\"" + id + "\"}}");
    };
    for (int index = 0; index < 128; ++index) {
        require(decoder.accept(call("admitted-" + std::to_string(index))).size() == 1,
            "native Agent calls within the host limit remain admitted");
    }
    require(decoder.accept(call("excess")).empty(), "the 129th active child is locally refused");
    stopped(decoder.accept(result("admitted-0")), "admitted-0");
    require(decoder.accept(result("excess")).empty(), "an unadmitted call cannot produce a STOP");
    require(decoder.accept(call("replacement")).size() == 1, "a released active slot remains reusable");
    stopped(decoder.accept(result("replacement")), "replacement");
    require(decoder.close("SessionEnd").size() == 127, "root closure retains every other admitted identity");
}

void native_correlation_capacity_is_reserved_before_start()
{
    for (const bool distinct_agents : {false, true}) {
        kimi::Wire_decoder decoder("session-native-capacity");
        const auto call = [](const std::string& id) {
            return json("{\"type\":\"context.append_loop_event\",\"event\":{\"type\":\"tool.call\","
                "\"name\":\"Agent\",\"toolCallId\":\"" + id + "\"}}");
        };
        const auto spawned = [](const std::string& id, const std::string& agent) {
            return json("{\"type\":\"subagent.spawned\",\"subagentId\":\"" + agent +
                "\",\"parentToolCallId\":\"" + id + "\"}");
        };
        const auto cancelled = [](const std::string& agent) {
            return json("{\"type\":\"subagent.cancelled\",\"subagentId\":\"" + agent + "\"}");
        };
        for (int index = 0; index < 511; ++index) {
            const auto id = "previous-" + std::to_string(index);
            const auto agent = distinct_agents ? "native-" + std::to_string(index) : "resumed-native";
            require(decoder.accept(call(id)).size() == 1, "native history below its retained bound is admitted");
            require(decoder.accept(spawned(id, agent)).empty(), "spawn binds the admitted native call");
            stopped(decoder.accept(cancelled(agent)), id);
        }
        require(decoder.accept(call("held-call")).size() == 1, "the final native correlation slot can be reserved");
        require(decoder.accept(json(R"({"type":"subagent.started","subagentId":"unreserved-native"})")).empty(),
            "an uncorrelated native run cannot consume an admitted call's reserved lineage capacity");
        require(decoder.accept(spawned("held-call", "held-native")).empty(), "the admitted call retains its native identity join");
        stopped(decoder.accept(cancelled("held-native")), "held-call");
        require(decoder.accept(call("excess-call")).empty(),
            "native identity/run exhaustion must refuse START before promising an untrackable child");
        require(decoder.close("SessionEnd").empty(), "native cancellation closed every published call before root END");
    }
}

} // namespace

int main()
{
    int failures = 0;
    const struct {
        const char* name;
        void (*run)();
    } cases[] = {
        {"cancellation-and-resume", cancellation_and_resume},
        {"cancellation-before-reply", cancellation_before_reply},
        {"terminal-notifications", terminal_notifications},
        {"independent-cancellation", independent_cancellation},
        {"capacity-preserves-admitted-finishes", capacity_preserves_admitted_finishes},
        {"native-correlation-capacity", native_correlation_capacity_is_reserved_before_start},
    };
    for (const auto& test : cases) {
        try {
            test.run();
            std::printf("PASS: %s\n", test.name);
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s: %s\n", test.name, error.what());
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
