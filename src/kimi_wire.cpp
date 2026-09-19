#include "kimi_wire.h"
#include "kimi_json.h"
#include "kimi_launcher.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace kimi {
namespace addon = logonomic::cli_addon;
namespace {

std::size_t utf16_units(std::string_view value)
{
    std::size_t result = 0;
    for (const unsigned char byte : value) {
        if ((byte & 0xC0) != 0x80) result += byte >= 0xF0 ? 2 : 1;
    }
    return result;
}

bool valid_id(const std::string& value)
{
    return !value.empty() && value.size() <= 256 && value.find('\0') == std::string::npos;
}

bool boolean(const Json& value, std::string_view key, bool fallback = false)
{
    const auto& field = member(value, key);
    return field.is_boolean() ? field.as_boolean() : fallback;
}

} // namespace

Wire_decoder::Wire_decoder(std::string session_id)
: m_session_id(std::move(session_id))
{}

std::optional<std::size_t> Wire_decoder::selected(const Lineage& lineage) const
{
    if (lineage.selected && *lineage.selected < lineage.runs.size()) return lineage.selected;
    if (lineage.scan_cursor < lineage.runs.size()) return lineage.scan_cursor;
    if (lineage.runs.size() == 1) return 0;
    return std::nullopt;
}

bool Wire_decoder::remember(const std::string& agent)
{
    if (!valid_id(agent)) return false;
    if (m_known.contains(agent)) return true;
    if (m_known.size() >= 512) return false;
    m_known.insert(agent);
    return true;
}

void Wire_decoder::retire(const std::string& id)
{
    const auto found = m_calls.find(id);
    if (found == m_calls.end()) return;
    found->second.active   = false;
    found->second.terminal = true;
    m_retired.insert_or_assign(id, std::move(found->second));
    m_calls.erase(found);
}

bool Wire_decoder::stop_call(const std::string& id)
{
    const auto found = m_calls.find(id);
    if (found == m_calls.end()) return false;
    const auto agent = found->second.agent_id;
    const bool active = found->second.active;
    if (const auto lineage = m_lineages.find(agent); lineage != m_lineages.end()) {
        for (std::size_t index = 0; index < lineage->second.runs.size(); ++index) {
            auto& run = lineage->second.runs[index];
            if (run.parent != id) continue;
            run.active   = false;
            run.terminal = true;
            lineage->second.scan_cursor = std::max(lineage->second.scan_cursor, index + 1);
            if (lineage->second.selected == index) lineage->second.selected.reset();
            break;
        }
    }
    const bool stopped = active && m_running.erase(id) != 0;
    retire(id);
    return stopped;
}

addon::Lifecycle_observation Wire_decoder::observation(
    std::string_view name, const std::string& agent, bool started, bool terminal)
{
    addon::Lifecycle_observation result;
    result.provider_event_name = name;
    result.provider_session_id = m_session_id;
    result.agent_id = agent;
    result.subagent_transition = started ? addon::Subagent_transition::STARTED : addon::Subagent_transition::STOPPED;
    if (const auto label = m_labels.find(agent); label != m_labels.end()) {
        result.agent_label = label->second;
        if (terminal) m_labels.erase(label);
    }
    return result;
}

std::vector<addon::Lifecycle_observation> Wire_decoder::accept(const Json& record)
{
    if (!m_failure.empty()) return {};
    auto type = text(record, "type");
    if (type == "context.append_loop_event") {
        const auto& event = member(record, "event");
        const auto id = text(event, "toolCallId");
        if (!valid_id(id)) return {};
        const auto event_type = text(event, "type");
        if (event_type == "tool.call" && text(event, "name") == "Agent") {
            if (m_calls.contains(id) || m_retired.contains(id)) return {};
            if (m_calls.size() >= 128 || m_running.size() >= 128) {
                m_failure = "direct_agent_active_capacity_exhausted";
                return {};
            }
            if (m_calls.size() + m_retired.size() >= 4096) {
                m_failure = "direct_agent_retained_capacity_exhausted";
                return {};
            }
            Call call;
            auto name = trim(text(member(event, "display"), "agent_name"));
            if (name.empty()) name = trim(text(member(event, "args"), "subagent_type"));
            if (!name.empty() && utf16_units(name) <= 256) call.label = name;
            call.background = boolean(member(event, "args"), "run_in_background");
            call.active = true;
            if (!call.label.empty()) m_labels[id] = call.label;
            m_calls.emplace(id, std::move(call));
            m_running.insert(id);
            return {observation("Agent.tool.call", id, true, false)};
        }
        if (event_type != "tool.result") return {};
        const auto call = m_calls.find(id);
        if (call == m_calls.end()) {
            if (const auto retired = m_retired.find(id); retired != m_retired.end()) retired->second.result_observed = true;
            return {};
        }
        if (call->second.result_observed) return {};
        call->second.result_observed = true;
        if (call->second.background) {
            std::istringstream output(text(member(event, "result"), "output"));
            std::string task, status;
            for (std::string line; std::getline(output, line);) {
                line = trim(line);
                if (line.empty()) break;
                if (line.starts_with("task_id:")) task = trim(line.substr(8));
                if (line.starts_with("status:")) status = trim(line.substr(7));
            }
            if (status == "running" && valid_id(task) && call->second.active) {
                m_background[task] = id;
                return {};
            }
        }
        return stop_call(id) ? std::vector{observation("Agent.tool.result", id, false, true)} : std::vector<addon::Lifecycle_observation>{};
    }
    if (type == "context.append_message") {
        const auto& origin = member(member(record, "message"), "origin");
        const auto status = text(origin, "status");
        if (text(origin, "kind") != "background_task" ||
            (status != "completed" && status != "failed" && status != "cancelled")) return {};
        const auto task = m_background.find(text(origin, "taskId"));
        if (task == m_background.end()) return {};
        const auto id = task->second;
        m_background.erase(task);
        return stop_call(id) ? std::vector{observation("background_task." + status, id, false, true)} : std::vector<addon::Lifecycle_observation>{};
    }
    const auto agent = text(record, "subagentId");
    if (!valid_id(agent)) return {};
    if (type == "subagent.spawned") {
        const auto parent = text(record, "parentToolCallId");
        if (parent.size() > 256) return {};
        const auto call = m_calls.find(parent);
        const auto retired = m_retired.find(parent);
        Call* correlation = call != m_calls.end() ? &call->second :
            retired != m_retired.end() ? &retired->second : nullptr;
        if (correlation && !correlation->agent_id.empty() && correlation->agent_id != agent) return {};
        if (!remember(agent)) {
            if (correlation) m_failure = "direct_agent_identity_capacity_exhausted";
            return {};
        }
        auto& lineage = m_lineages[agent];
        std::optional<std::size_t> index;
        for (std::size_t candidate = 0; candidate < lineage.runs.size(); ++candidate) {
            if (lineage.runs[candidate].parent == parent) { index = candidate; break; }
        }
        if (!index && !parent.empty() && lineage.runs.size() == 1 &&
            lineage.runs.front().parent.empty() && !lineage.runs.front().terminal) {
            lineage.runs.front().parent = parent;
            index = 0;
        }
        if (!index) {
            if (std::any_of(lineage.runs.begin(), lineage.runs.end(), [](const auto& run) { return run.active; })) return {};
            if (m_run_count >= 512) {
                if (correlation) m_failure = "direct_agent_run_capacity_exhausted";
                return {};
            }
            Run run;
            run.parent = parent;
            lineage.runs.push_back(std::move(run));
            index = lineage.runs.size() - 1;
            ++m_run_count;
        }
        lineage.selected = index;
        auto& run = lineage.runs[*index];
        const auto public_id = correlation ? parent : agent;
        if (run.public_id.empty()) run.public_id = public_id;
        else
        if (run.public_id != public_id) return {};
        auto name = trim(text(record, "subagentName"));
        if (name.empty() && correlation) name = correlation->label;
        if (!name.empty() && utf16_units(name) <= 256) {
            if (run.label.empty()) run.label = name;
            if (retired == m_retired.end()) m_labels[public_id] = run.label;
        }
        if (correlation) {
            correlation->agent_id = agent;
            run.active   = correlation->active;
            run.terminal = correlation->terminal;
            correlation->background = boolean(record, "runInBackground", correlation->background);
            const auto task = text(record, "taskId");
            if (correlation->background && !correlation->terminal && valid_id(task)) m_background[task] = public_id;
        }
        return {};
    }
    if (type == "subagent.started") {
        if (!remember(agent)) return {};
        auto found = m_lineages.find(agent);
        if (found == m_lineages.end()) {
            if (m_run_count >= 512) return {};
            Lineage lineage;
            lineage.runs.emplace_back();
            found = m_lineages.emplace(agent, std::move(lineage)).first;
            ++m_run_count;
        }
        const auto index = selected(found->second);
        if (!index) return {};
        auto& run = found->second.runs[*index];
        found->second.selected = index;
        if (run.public_id.empty()) run.public_id = agent;
        if (run.terminal || run.active) return {};
        if (m_retired.contains(run.parent)) { run.terminal = true; return {}; }
        if (m_running.contains(run.public_id)) return {};
        if (m_running.size() >= 128) {
            if (m_calls.contains(run.parent)) m_failure = "direct_agent_active_capacity_exhausted";
            return {};
        }
        run.active = true;
        if (!run.label.empty()) m_labels[run.public_id] = run.label;
        if (const auto call = m_calls.find(run.parent); call != m_calls.end()) call->second.active = true;
        m_running.insert(run.public_id);
        return {observation(type, run.public_id, true, false)};
    }
    if (type != "subagent.suspended" && type != "subagent.completed" && type != "subagent.failed") return {};
    const auto found = m_lineages.find(agent);
    if (found == m_lineages.end()) return {};
    const auto index = selected(found->second);
    if (!index) return {};
    auto& run = found->second.runs[*index];
    if (run.public_id.empty()) run.public_id = agent;
    const bool terminal = type != "subagent.suspended";
    const bool active = run.active;
    if (terminal) {
        run.terminal = true;
        retire(run.parent);
        found->second.scan_cursor = std::max(found->second.scan_cursor, *index + 1);
        found->second.selected.reset();
    }
    if (!active) return {};
    run.active = false;
    if (!run.label.empty()) m_labels[run.public_id] = run.label;
    if (const auto call = m_calls.find(run.parent); call != m_calls.end()) call->second.active = false;
    m_running.erase(run.public_id);
    return {observation(type, run.public_id, false, terminal)};
}

void Wire_decoder::rewind()
{
    for (auto& [id, lineage] : m_lineages) {
        (void)id;
        lineage.scan_cursor = 0;
        lineage.selected.reset();
    }
}

std::vector<addon::Lifecycle_observation> Wire_decoder::close(std::string_view reason)
{
    std::vector<addon::Lifecycle_observation> result;
    for (const auto& id : m_running) result.push_back(observation(reason, id, false, true));
    m_running.clear();
    for (auto& [id, lineage] : m_lineages) {
        (void)id;
        for (auto& run : lineage.runs) { run.active = false; run.terminal = true; }
    }
    for (auto& [id, call] : m_calls) { (void)id; call.active = false; call.terminal = true; }
    return result;
}

} // namespace kimi
