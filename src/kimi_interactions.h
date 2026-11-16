#pragma once

#include "kimi_json.h"

#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/model_state.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kimi {

struct Interaction_wire
{
    std::string agent_id;
    std::filesystem::path path;
    bool present = false;
};

struct Interaction_inventory
{
    std::optional<std::filesystem::path> root;
    std::vector<Interaction_wire> wires;
};

Interaction_inventory discover_interaction_wires(const std::string& home, const std::string& session);

// Called by the native SessionStart helper before delivery can release launch.
// The authenticated body carries offsets and hashes, never paths or wire text.
Json capture_interaction_frontier(const std::string& home, const std::string& session);

class Interaction_reader
{
public:
    Interaction_reader(std::string session, const Json& frontier);
    ~Interaction_reader();
    Interaction_reader(const Interaction_reader&) = delete;
    Interaction_reader& operator=(const Interaction_reader&) = delete;

    bool acquire(const Interaction_inventory& inventory,
        std::vector<logonomic::cli_addon::Lifecycle_observation>& observations,
        const std::function<bool()>& cancelled, bool observe_models = false);
    std::optional<logonomic::cli_addon::Model_update> model_observation(std::string_view source_nonce) const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace kimi
