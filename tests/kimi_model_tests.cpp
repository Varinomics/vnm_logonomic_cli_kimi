#include "kimi_interactions.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/package_source.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool value, const char* message)
{
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct Workspace
{
    fs::path root = fs::temp_directory_path() / ("kimi-model-" + addon::random_nonce_hex());
    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

void append(const fs::path& file, std::string_view record, bool complete = true)
{
    fs::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary | std::ios::app);
    output << record;
    if (complete) output << '\n';
    require(static_cast<bool>(output), "native wire fixture must append completely");
}

} // namespace

int main()
{
    // Oracle: native llm.request separates loop, compaction and prediction,
    // and names the serving model independently of modelAlias and billing.
    Workspace workspace;
    const auto home = addon::path_to_utf8(workspace.root);
    const auto wire = workspace.root / "sessions" / "workspace" / "session" / "agents" / "main" / "wire.jsonl";
    const std::string nonce(32, 'e');
    append(wire, R"({"type":"llm.request","agentId":"main","kind":"loop","provider":"native","model":"history-model"})");
    kimi::Interaction_reader reader("session", kimi::capture_interaction_frontier(home, "session"));
    std::vector<addon::Lifecycle_observation> observations;
    const auto acquire = [&] {
        require(reader.acquire(kimi::discover_interaction_wires(home, "session"), observations,
            [] { return false; }, nonce), "native source acquisition must finish");
    };
    acquire();
    require(!reader.model_observation(), "a new activation must not replay historical models");
    append(wire, R"({"agentId":"main","kind":"loop","provider":"openai","model":"own-model","modelAlias":"selected-alias","type":"llm.request"})");
    acquire();
    const auto first = reader.model_observation();
    require(first && first->provider_session_id == "session" && first->agent_id.empty() &&
        first->provider.empty() && first->model == "own-model" && first->transition_nonce == nonce,
        "the root reports its literal model without treating an API protocol as its backend");
    append(wire, R"({"type":"llm.request","agentId":"main","kind":"compaction","provider":"native","model":"summary-model"})");
    append(wire, R"({"type":"llm.request","agentId":"main","kind":"prediction","provider":"native","model":"prediction-model"})");
    append(wire, R"({"type":"usage.record","agentId":"main","model":"billed-model"})");
    append(wire, R"({"type":"llm.request","agentId":"main","kind":"loop","model":{"invalid":true}})");
    const auto child_wire = wire.parent_path().parent_path() / "child" / "wire.jsonl";
    append(child_wire, R"({"type":"llm.request","agentId":"child","kind":"loop","provider":"native","model":"child-model"})");
    acquire();
    require(reader.model_observation() == first && observations.empty(),
        "auxiliary, malformed, billed and unadmitted child models cannot replace root evidence or invent lifecycle");
    append(wire, R"({"type":"llm.request","agentId":"main","kind":"loop","provider":"native","model":"changed-model"})");
    acquire();
    const auto changed = reader.model_observation();
    require(changed && changed->model == "changed-model" && changed->sequence > first->sequence,
        "native byte position preserves source order across repeated scans");
    append(wire, R"({"type":"llm.request","agentId":"main",)", false);
    acquire();
    require(reader.model_observation() == changed, "a partial record leaves the last complete sample intact");
    append(wire, R"("kind":"loop","provider":"native","model":"completed-model"})");
    acquire();
    require(reader.model_observation()->model == "completed-model", "a split record becomes evidence at completion");
    kimi::Interaction_reader next("session", kimi::capture_interaction_frontier(home, "session"));
    require(next.acquire(kimi::discover_interaction_wires(home, "session"), observations, [] { return false; },
        std::string(32, 'f')) && !next.model_observation(),
        "a repeated process execution begins without inherited model evidence");
    std::cout << "Kimi source-frontier model observation contracts passed\n";
}
