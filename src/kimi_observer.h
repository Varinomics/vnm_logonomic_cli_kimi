#pragma once

#include "kimi_wire.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>

#include <memory>
#include <filesystem>
#include <optional>
#include <string>

namespace kimi {

std::optional<std::filesystem::path> find_session_wire(
    const std::string& home, const std::string& session);

class Lifecycle_observer
{
public:
    Lifecycle_observer();
    ~Lifecycle_observer();
    Lifecycle_observer(const Lifecycle_observer&) = delete;
    Lifecycle_observer& operator=(const Lifecycle_observer&) = delete;
    void handle(logonomic::cli_addon::Invocation_context& context);
    logonomic::cli_addon::Classify_event_result classify(
        const logonomic::cli_addon::Classify_event_request& request);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace kimi
