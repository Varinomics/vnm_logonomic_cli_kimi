#include "kimi_interactions.h"
#include "kimi_wire_record.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace kimi {
namespace addon = logonomic::cli_addon;
namespace json_util = addon::json;
namespace fs = std::filesystem;
namespace {

constexpr std::size_t k_agent_bound = 256;
constexpr std::size_t k_directory_bound = 4096;
constexpr std::size_t k_frontier_bound = 32 * 1024;
constexpr std::size_t k_request_bound = 4096;
constexpr std::size_t k_record_bound = 4 * 1024 * 1024;
constexpr std::size_t k_anchor_bound = 4096;

bool valid_component(const std::string& value)
{
    return !value.empty() && value.size() <= 256 && value != "." && value != ".." &&
        value.find_first_of("/\\") == std::string::npos && value.find('\0') == std::string::npos;
}

fs::file_status entry_status(const fs::path& path)
{
    std::error_code error;
    const auto result = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) {
        throw std::runtime_error("interaction_wire_unavailable");
    }
    return result;
}

std::string root_digest(const Interaction_inventory& inventory)
{
    return inventory.root ? addon::to_hex(addon::Sha256::digest(addon::path_to_utf8(*inventory.root))) : std::string{};
}

std::string read_anchor(std::ifstream& input, std::uint64_t offset)
{
    const auto size = std::min<std::uint64_t>(k_anchor_bound, offset);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset - size));
    input.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error("interaction_wire_unavailable");
    }
    return bytes;
}

std::uint64_t wire_size(std::ifstream& input)
{
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) {
        throw std::runtime_error("interaction_wire_unavailable");
    }
    return static_cast<std::uint64_t>(length);
}

std::string read_consumed_record(
    std::ifstream& input,
    std::uint64_t begin,
    std::uint64_t size,
    std::uint64_t resume,
    const addon::Sha256_digest& observed)
{
    // A partial type-last record must not reserve another agent's raw-byte
    // budget. Materialize only a completed consumed record, and bind the
    // reread to the exact bytes already inspected before parsing or accepting.
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.clear();
    input.seekg(static_cast<std::streamoff>(begin));
    input.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error("interaction_wire_unavailable");
    }
    if (addon::Sha256::digest(bytes) != observed) {
        throw std::runtime_error("interaction_wire_changed");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(resume));
    if (!input) {
        throw std::runtime_error("interaction_wire_unavailable");
    }
    return bytes;
}

struct Stream
{
    std::uint64_t offset = 0;
    std::string anchor = addon::to_hex(addon::Sha256::digest({}));
    Wire_record_envelope envelope;
    std::uint64_t record_bytes = 0;
    addon::Sha256 record_digest;
    bool discarding = false;
};

} // namespace

Interaction_inventory discover_interaction_wires(const std::string& home, const std::string& session)
{
    if (!valid_component(session) || !addon::path_from_utf8(home).is_absolute()) {
        throw std::runtime_error("invalid_kimi_session");
    }
    Interaction_inventory result;
    const auto sessions = addon::path_from_utf8(home) / "sessions";
    const auto sessions_status = entry_status(sessions);
    if (!fs::exists(sessions_status)) {
        return result;
    }
    if (!fs::is_directory(sessions_status) || fs::is_symlink(sessions_status)) {
        throw std::runtime_error("interaction_wire_unavailable");
    }
    std::error_code error;
    std::size_t visited = 0;
    for (fs::directory_iterator workspace(sessions, error), end;
         !error && workspace != end; workspace.increment(error))
    {
        if (++visited > k_directory_bound) {
            throw std::runtime_error("interaction_inventory_capacity_exhausted");
        }
        const auto workspace_status = entry_status(workspace->path());
        if (!fs::is_directory(workspace_status) || fs::is_symlink(workspace_status)) {
            continue;
        }
        const auto directory = workspace->path() / session;
        const auto session_status = entry_status(directory);
        if (!fs::is_directory(session_status) || fs::is_symlink(session_status)) {
            continue;
        }
        if (result.root) {
            throw std::runtime_error("ambiguous_interaction_session");
        }
        result.root = fs::canonical(directory) / "agents";
    }
    if (error) {
        throw std::runtime_error("interaction_wire_unavailable");
    }
    if (!result.root) {
        return result;
    }
    const auto agents_status = entry_status(*result.root);
    if (!fs::exists(agents_status)) {
        return result;
    }
    if (!fs::is_directory(agents_status) || fs::is_symlink(agents_status)) {
        throw std::runtime_error("interaction_wire_unavailable");
    }
    visited = 0;
    for (fs::directory_iterator agent(*result.root, error), end; !error && agent != end; agent.increment(error)) {
        if (++visited > k_directory_bound) {
            throw std::runtime_error("interaction_inventory_capacity_exhausted");
        }
        const auto agent_status = entry_status(agent->path());
        if (fs::is_symlink(agent_status)) {
            throw std::runtime_error("interaction_wire_unavailable");
        }
        if (!fs::is_directory(agent_status)) {
            continue;
        }
        const auto id = addon::path_to_utf8(agent->path().filename());
        if (!valid_component(id) || result.wires.size() >= k_agent_bound) {
            throw std::runtime_error("interaction_inventory_capacity_exhausted");
        }
        const auto wire = agent->path() / "wire.jsonl";
        const auto wire_status = entry_status(wire);
        if (fs::is_symlink(wire_status) || (fs::exists(wire_status) && !fs::is_regular_file(wire_status))) {
            throw std::runtime_error("interaction_wire_unavailable");
        }
        result.wires.push_back({id, wire, fs::is_regular_file(wire_status)});
    }
    if (error) {
        throw std::runtime_error("interaction_wire_unavailable");
    }
    return result;
}

Json capture_interaction_frontier(const std::string& home, const std::string& session)
{
    const auto inventory = discover_interaction_wires(home, session);
    auto agents = Json::array();
    for (const auto& wire : inventory.wires) {
        if (!wire.present) {
            continue;
        }
        std::ifstream input(wire.path, std::ios::binary);
        const auto offset = wire_size(input);
        const auto anchor = read_anchor(input, offset);
        agents.as_array().push_back(json_util::object({
            {"agent_id", Json(wire.agent_id)},
            {"offset",   Json(static_cast<std::int64_t>(offset))},
            {"anchor",   Json(addon::to_hex(addon::Sha256::digest(anchor)))},
            {"partial",  Json(!anchor.empty() && anchor.back() != '\n')},
        }));
    }
    auto result = json_util::object({{"version", Json(1)}, {"root", Json(root_digest(inventory))},
        {"agents", std::move(agents)}});
    if (addon::serialize_json(result).size() > k_frontier_bound) {
        throw std::runtime_error("interaction_frontier_capacity_exhausted");
    }
    return result;
}

struct Interaction_reader::State
{
    struct Request
    {
        std::string agent;
        bool resolved = false;
    };

    std::string session;
    std::string root;
    std::map<std::string, Stream> streams;
    std::map<std::string, Request> requests;

    void accept(const Json& record, const std::string& agent,
        std::vector<addon::Lifecycle_observation>& observations)
    {
        const auto type = json_util::text(record, "type");
        if (type != "interaction.request" && type != "interaction.resolved") {
            return;
        }
        const auto id = json_util::text(record, "id");
        if (!json_util::bounded_string(json_util::member(record, "id"), 256) ||
            json_util::text(record, "agentId") != agent)
        {
            return;
        }
        const bool requested = type == "interaction.request";
        auto found = requests.find(id);
        if (requested) {
            const auto kind = json_util::text(record, "kind");
            if ((kind != "approval" && kind != "question") || found != requests.end() ||
                !json_util::bounded_string(json_util::member(record, "toolCallId"), 256) ||
                !json_util::member(record, "request").is_object())
            {
                return;
            }
            if (requests.size() >= k_request_bound) {
                throw std::runtime_error("interaction_request_capacity_exhausted");
            }
            requests.emplace(id, Request{agent, false});
        }
        else {
            if (found == requests.end() || found->second.agent != agent || found->second.resolved ||
                !record.find("response"))
            {
                return;
            }
            found->second.resolved = true;
        }
        addon::Lifecycle_observation observation;
        observation.provider_event_name  = type;
        observation.provider_session_id  = session;
        observation.agent_id             = agent;
        observation.attention_request_id = id;
        observation.attention_transition = requested ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED;
        observation.attention_evidence   = addon::Evidence::CONFIRMED;
        observations.push_back(std::move(observation));
    }
};

Interaction_reader::Interaction_reader(std::string session, const Json& frontier)
: m_state(std::make_unique<State>())
{
    const auto& version = json_util::member(frontier, "version");
    const auto& root = json_util::member(frontier, "root");
    const auto& agents = json_util::member(frontier, "agents");
    if (!frontier.is_object() || frontier.as_object().size() != 3 ||
        !version.is_integer() || version.as_integer() != 1 || !root.is_string() ||
        (!root.as_string().empty() && !addon::sha256_from_hex(root.as_string())) ||
        !agents.is_array() || agents.as_array().size() > k_agent_bound ||
        addon::serialize_json(frontier).size() > k_frontier_bound)
    {
        throw std::runtime_error("invalid_interaction_frontier");
    }
    m_state->session = std::move(session);
    m_state->root = root.as_string();
    for (const auto& entry : agents.as_array()) {
        const auto agent = json_util::text(entry, "agent_id");
        const auto& offset = json_util::member(entry, "offset");
        const auto& anchor = json_util::member(entry, "anchor");
        const auto& partial = json_util::member(entry, "partial");
        if (!entry.is_object() || entry.as_object().size() != 4 || !valid_component(agent) ||
            !offset.is_integer() || offset.as_integer() < 0 || !anchor.is_string() ||
            !addon::sha256_from_hex(anchor.as_string()) || !partial.is_boolean() || m_state->streams.contains(agent))
        {
            throw std::runtime_error("invalid_interaction_frontier");
        }
        Stream stream;
        stream.offset = static_cast<std::uint64_t>(offset.as_integer());
        stream.anchor = anchor.as_string();
        stream.discarding = partial.as_boolean();
        m_state->streams.emplace(agent, std::move(stream));
    }
}

Interaction_reader::~Interaction_reader() = default;

bool Interaction_reader::acquire(const Interaction_inventory& inventory,
    std::vector<addon::Lifecycle_observation>& observations, const std::function<bool()>& cancelled)
{
    const auto root = root_digest(inventory);
    if (!root.empty()) {
        if (!m_state->root.empty() && root != m_state->root) {
            throw std::runtime_error("interaction_scope_changed");
        }
        m_state->root = root;
    }
    bool complete = true;
    for (const auto& wire : inventory.wires) {
        if (!wire.present) {
            continue;
        }
        if (cancelled()) {
            return false;
        }
        if (!m_state->streams.contains(wire.agent_id) && m_state->streams.size() >= k_agent_bound) {
            throw std::runtime_error("interaction_inventory_capacity_exhausted");
        }
        auto& stream = m_state->streams[wire.agent_id];
        std::ifstream input(wire.path, std::ios::binary);
        if (!input) {
            // Inventory and opening can race. Keep other wires progressing
            // while the observer requests a rescan for this incomplete acquisition.
            complete = false;
            continue;
        }
        const auto end = wire_size(input);
        if (end < stream.offset || addon::to_hex(addon::Sha256::digest(read_anchor(input, stream.offset))) != stream.anchor) {
            // Rewritten history cannot prove which prompts belong to this process.
            // Retain already accepted attention and report observation loss.
            throw std::runtime_error("interaction_wire_changed");
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(stream.offset));
        const auto append_record = [&](std::string_view part) {
            if (stream.discarding || part.empty()) {
                return;
            }
            stream.record_bytes += part.size();
            stream.envelope.append(part);
            if (!stream.envelope.type_known() || stream.envelope.kind() == Wire_record_kind::INTERACTION) {
                stream.record_digest.update(part);
            }
        };
        const auto finish_record = [&](std::uint64_t newline_offset) {
            if (!stream.discarding && stream.record_bytes) {
                if (!stream.envelope.finish()) {
                    throw std::runtime_error("invalid_interaction_wire_record");
                }
                if (stream.envelope.kind() == Wire_record_kind::INTERACTION) {
                    if (stream.record_bytes > k_record_bound) {
                        throw std::runtime_error("interaction_record_capacity_exhausted");
                    }
                    const auto bytes = read_consumed_record(input,
                        newline_offset - stream.record_bytes, stream.record_bytes,
                        stream.offset, stream.record_digest.finish());
                    addon::Json_parse_limits limits;
                    limits.max_bytes = k_record_bound;
                    limits.max_string_bytes = k_record_bound;
                    const auto record = addon::parse_json(bytes, limits, nullptr);
                    if (!record || !record->is_object()) {
                        throw std::runtime_error("invalid_interaction_wire_record");
                    }
                    m_state->accept(*record, wire.agent_id, observations);
                }
            }
            stream.envelope.reset();
            stream.record_bytes = 0;
            stream.record_digest = addon::Sha256{};
            stream.discarding = false;
        };
        std::array<char, 65536> bytes{};
        while (stream.offset < end && !cancelled()) {
            const auto remaining = std::min<std::uint64_t>(bytes.size(), end - stream.offset);
            input.read(bytes.data(), static_cast<std::streamsize>(remaining));
            const auto count = static_cast<std::size_t>(input.gcount());
            if (count == 0) {
                throw std::runtime_error("interaction_wire_unavailable");
            }
            stream.offset += count;
            auto chunk = std::string_view(bytes.data(), count);
            while (!chunk.empty()) {
                const auto newline = chunk.find('\n');
                append_record(chunk.substr(0, newline));
                if (newline == std::string_view::npos) {
                    break;
                }
                finish_record(stream.offset - chunk.size() + newline);
                chunk.remove_prefix(newline + 1);
            }
        }
        stream.anchor = addon::to_hex(addon::Sha256::digest(read_anchor(input, stream.offset)));
        complete = complete && stream.offset == end;
    }
    return complete;
}

} // namespace kimi
