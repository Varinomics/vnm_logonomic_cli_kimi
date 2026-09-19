#pragma once

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/lifecycle_events.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace kimi {

class Wire_decoder
{
public:
    explicit Wire_decoder(std::string session_id);
    std::vector<logonomic::cli_addon::Lifecycle_observation> accept(
        const logonomic::cli_addon::Json_value& record);
    std::vector<logonomic::cli_addon::Lifecycle_observation> close(std::string_view reason);
    void rewind();
    const std::string& failure() const { return m_failure; }

private:
    struct Call
    {
        std::string agent_id;
        std::string label;
        bool background = false;
        bool active = false;
        bool terminal = false;
        bool result_observed = false;
    };
    struct Run
    {
        std::string parent;
        std::string public_id;
        std::string label;
        bool active = false;
        bool terminal = false;
    };
    struct Lineage
    {
        std::vector<Run> runs;
        std::optional<std::size_t> selected;
        std::size_t scan_cursor = 0;
    };
    std::optional<std::size_t> selected(const Lineage& lineage) const;
    bool remember(const std::string& agent);
    bool stop_call(const std::string& id);
    void retire(const std::string& id);
    logonomic::cli_addon::Lifecycle_observation observation(
        std::string_view name, const std::string& agent, bool started, bool terminal);

    std::string m_session_id;
    std::map<std::string, Call> m_calls;
    std::map<std::string, Call> m_retired;
    std::map<std::string, Lineage> m_lineages;
    std::map<std::string, std::string> m_background;
    std::map<std::string, std::string> m_labels;
    std::set<std::string> m_known;
    std::set<std::string> m_running;
    std::size_t m_run_count = 0;
    std::string m_failure;
};

} // namespace kimi
