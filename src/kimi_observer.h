#pragma once

#include "kimi_wire.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <vnm_filesystem_observation.h>

#include <memory>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace kimi {

std::optional<std::filesystem::path> find_session_wire(
    const std::string& home, const std::string& session);

class Lifecycle_observer
{
public:
    Lifecycle_observer();
    Lifecycle_observer(std::shared_ptr<vnm::filesystem_observation::Service> service,
        vnm::filesystem_observation::Configuration configuration);
    ~Lifecycle_observer();
    Lifecycle_observer(const Lifecycle_observer&) = delete;
    Lifecycle_observer& operator=(const Lifecycle_observer&) = delete;
    void handle(logonomic::cli_addon::Invocation_context& context);
    logonomic::cli_addon::Classify_event_result classify(
        const logonomic::cli_addon::Classify_event_request& request);
#ifdef KIMI_OBSERVER_TEST_HOOKS
    void set_scan_gate_for_tests(std::function<void()> gate);
#endif

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace kimi
