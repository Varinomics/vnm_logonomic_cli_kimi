#include "kimi_quota.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/quota.h>

#include <QProcess>
#include <QProcessEnvironment>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

class Inspection : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return invocation; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value value) override { result = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string) override { throw std::runtime_error(reason); }
    bool finished() const override { return result.has_value(); }
    addon::Invoke_request invocation;
    std::optional<addon::Json_value> result;
};

std::optional<addon::Quota_inspect_result> inspect_reply(const addon::Json_value& value)
{
    Inspection context;
    addon::Probe_inspect_request request;
    request.inspection.final = true;
    request.inspection.exit_code = 0;
    request.inspection.stdout_bytes = addon::serialize_json(value);
    context.invocation.capability = {"quota", 2};
    context.invocation.operation = "inspect";
    context.invocation.payload = addon::encode_probe_inspect_request(request, 2);
    kimi::handle_quota(context);
    addon::Contract_failure failure;
    return context.result ? addon::decode_quota_inspect_result(*context.result, true, &failure) : std::nullopt;
}

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "web") {
        std::fputs("private-startup-sentinel\n", stderr);
        if (std::getenv("KIMI_QUOTA_TEST_WAIT")) std::this_thread::sleep_for(std::chrono::seconds(20));
        return 17;
    }
    const auto root = fs::temp_directory_path() / ("kimi-startup-" + addon::random_nonce_hex());
    fs::create_directories(root);
    int result = 1;
    try {
        const auto executable = fs::absolute(addon::path_from_utf8(argv[0]));
        auto probe = executable.parent_path() / "kimi_quota_probe";
#ifdef _WIN32
        probe += ".exe";
#endif
        const auto inspect = [&](bool wait) {
            QProcess process;
            auto environment = QProcessEnvironment::systemEnvironment();
            if (wait) environment.insert(QStringLiteral("KIMI_QUOTA_TEST_WAIT"), QStringLiteral("1"));
            process.setProcessEnvironment(environment);
            process.start(QString::fromStdString(addon::path_to_utf8(probe)),
                {QStringLiteral("--kimi-program"), QString::fromStdString(addon::path_to_utf8(executable)),
                    QStringLiteral("--kimi-code-home"), QString::fromStdString(addon::path_to_utf8(root))});
            require(process.waitForStarted() && process.waitForFinished(15000), "quota helper completes");
            const auto output = process.readAllStandardOutput();
            const auto diagnostic = process.readAllStandardError();
            require(!output.contains("private-startup-sentinel") && !diagnostic.contains("private-startup-sentinel"),
                "private native output never escapes the quota helper");
            const auto value = addon::parse_json(output.toStdString(), {}, nullptr);
            require(value.has_value(), "quota helper returns structured evidence");
            return *value;
        };
        auto exited_reply = inspect(false);
        const auto exited = inspect_reply(exited_reply);
        require(exited && exited->diagnostic && exited->diagnostic->stage == addon::Quota_failure_stage::PROCESS_EXIT &&
                exited->diagnostic->code == 17,
            "native early exit preserves the process stage and exit code");
        fs::create_directories(root / "server");
        std::ofstream(root / "server/instances") << "not a directory";
        const auto registry = inspect_reply(inspect(false));
        require(registry && registry->diagnostic && registry->diagnostic->stage == addon::Quota_failure_stage::REGISTRY_READ &&
                registry->diagnostic->code != 0 && registry->failure_reason != exited->failure_reason,
            "registry failure preserves its filesystem category instead of readiness failure");
        exited_reply.find("diagnostic")->set("stage", addon::Json_value("private-stage-sentinel"));
        bool refused = false;
        try { (void)inspect_reply(exited_reply); }
        catch (const std::exception& error) {
            refused = std::string_view(error.what()) == "invalid_quota_response";
        }
        require(refused, "unreviewed diagnostic stages cannot cross the helper boundary");
        fs::remove(root / "server/instances");
        const auto waiting = inspect(true);
        require(waiting.find("reason_code") && waiting.find("reason_code")->as_string() == "quota_server_not_ready",
            "a running child with an absent registry remains a readiness timeout");
        result = 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    fs::remove_all(root);
    return result;
}
