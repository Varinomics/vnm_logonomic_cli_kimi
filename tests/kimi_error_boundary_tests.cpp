#define main adapter_entry
#include "../src/adapter.cpp"
#undef main

#include "kimi_interactions.h"

#include <logonomic_cli_addon_test/invocation_context.h>

#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {

class Classification_context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override { m_status = addon::Result_status::SUCCESS; }
    void fail(addon::Result_status status, std::string reason, std::string message) override
    {
        m_status  = status;
        m_reason  = std::move(reason);
        m_message = std::move(message);
    }
    bool finished() const override { return m_status.has_value(); }

    addon::Invoke_request m_request;
    std::optional<addon::Result_status> m_status;
    std::string m_reason;
    std::string m_message;
};

bool failure_diagnostic(
    kimi::Lifecycle_observer& observer,
    addon::Classify_event_request request,
    const std::string& field,
    addon::Provider_event_problem problem)
{
    Classification_context context;
    context.m_request.capability = {addon::k_attention_kind_capability_id, addon::k_attention_kind_version};
    context.m_request.operation = addon::k_classify_event_operation;
    context.m_request.payload = addon::encode_classify_event_request(request);
    invoke(context, observer);
    const auto diagnostic = addon::decode_provider_event_diagnostic(context.m_message);
    const bool valid = context.m_status == addon::Result_status::FAILED &&
        context.m_reason == "invalid_provider_event" && diagnostic &&
        diagnostic->event_name == request.event.name && diagnostic->field == field && diagnostic->problem == problem &&
        context.m_message.find("private-event-value") == std::string::npos;
    if (!valid) {
        std::cerr << "classification did not preserve a value-free field diagnostic for " << field << '\n';
    }
    return valid;
}

} // namespace

int main()
{
    logonomic::cli_addon::test::Sentinel_exception_context context;
    auto service = std::make_shared<vnm::filesystem_observation::Service>();
    kimi::Lifecycle_observer observer(service, {});
    invoke(context, observer);
    if (context.m_status != addon::Result_status::FAILED ||
        context.m_reason != "kimi_operation_failed" ||
        context.m_message.find("private-exception-sentinel") != std::string::npos)
    {
        std::cerr << "unexpected exception crossed the provider privacy boundary\n";
        return 1;
    }
    addon::Classify_event_request request;
    request.event.name = "SessionEnd";
    request.transition_nonce = std::string(32, 'd');
    if (!failure_diagnostic(observer, request, "session_id", addon::Provider_event_problem::MISSING)) {
        return 1;
    }
    request.event.body.set("session_id", addon::Json_value());
    if (!failure_diagnostic(observer, request, "session_id", addon::Provider_event_problem::WRONG_TYPE)) {
        return 1;
    }
    request.event.body.set("session_id", addon::Json_value(""));
    if (!failure_diagnostic(observer, request, "session_id", addon::Provider_event_problem::EMPTY)) {
        return 1;
    }
    request.event.body.set("session_id", addon::Json_value(std::string(257, 'x')));
    if (!failure_diagnostic(observer, request, "session_id", addon::Provider_event_problem::TOO_LARGE)) {
        return 1;
    }
    request.event.body.set("session_id", addon::Json_value("private-event-value/invalid"));
    if (!failure_diagnostic(observer, request, "session_id", addon::Provider_event_problem::INVALID_VALUE)) {
        return 1;
    }
    request.event.body.set("session_id", addon::Json_value("session-primary"));
    request.event.name = "PermissionRequest";
    request.event.body.set("id", addon::Json_value("request-primary"));
    request.event.body.set("agent_id", addon::Json_value(false));
    if (!failure_diagnostic(observer, request, "agent_id", addon::Provider_event_problem::WRONG_TYPE)) {
        return 1;
    }
    request.event.name = "SessionStart";
    request.event.body.set("kimi_code_home", addon::Json_value("private-event-value"));
    if (!failure_diagnostic(observer, request, "kimi_code_home", addon::Provider_event_problem::INVALID_VALUE)) {
        return 1;
    }
    auto frontier = addon::Json_value::object();
    frontier.set("version", addon::Json_value(1));
    frontier.set("root", addon::Json_value(""));
    auto agents = addon::Json_value::array();
    auto entry = addon::Json_value::object();
    entry.set("agent_id", addon::Json_value("child-primary"));
    entry.set("offset", addon::Json_value("private-event-value"));
    entry.set("anchor", addon::Json_value(std::string(64, 'a')));
    entry.set("partial", addon::Json_value(false));
    agents.push_back(std::move(entry));
    frontier.set("agents", std::move(agents));
    try {
        kimi::Interaction_reader reader("session-primary", frontier);
        std::cerr << "a malformed frontier offset was accepted\n";
        return 1;
    }
    catch (const addon::Provider_event_error& error) {
        const auto& diagnostic = error.diagnostic();
        if (diagnostic.event_name != "SessionStart" || diagnostic.field != "interaction_frontier.agents[0].offset" ||
            diagnostic.problem != addon::Provider_event_problem::WRONG_TYPE ||
            addon::encode_provider_event_diagnostic(diagnostic).find("private-event-value") != std::string::npos)
        {
            std::cerr << "a malformed frontier lost its exact nested field diagnostic\n";
            return 1;
        }
    }
    return 0;
}
