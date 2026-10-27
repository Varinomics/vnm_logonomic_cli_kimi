#include "kimi_quota.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/quota.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
#endif

#include <QProcess>
#include <QProcessEnvironment>

#include <algorithm>
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

std::optional<addon::Quota_inspect_result> inspect_reply(const addon::Json_value& value, bool explain = true)
{
    Inspection context;
    addon::Probe_inspect_request request;
    request.account_context = addon::Account_context{2, "selected-kimi-account"};
    request.report_quota_failure = explain;
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

#ifdef _WIN32
void noisy_server()
{
    WSADATA data{};
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "fixture initializes sockets");
    const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "fixture creates listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        listen(listener, 2) == 0, "fixture listens on loopback");
    int size = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "fixture discovers port");
    const auto root = addon::path_from_utf8(std::getenv("KIMI_CODE_HOME"));
    fs::create_directories(root / "server/instances");
    std::ofstream(root / "server.token") << "fixture-token";
    const auto noise = [](int megabytes) {
        const std::string chunk(65536, 'x');
        for (int index = 0; index < megabytes * 16; ++index) {
            require(std::fwrite(chunk.data(), 1, chunk.size(), stderr) == chunk.size(),
                "native diagnostic output must not block or fail");
        }
        std::fflush(stderr);
    };
    noise(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::ofstream(root / "server/instances/fixture.json") <<
        "{\"pid\":" << GetCurrentProcessId() << ",\"server_id\":\"fixture\",\"host\":\"127.0.0.1\",\"port\":" <<
        ntohs(address.sin_port) << ",\"started_at\":1,\"heartbeat_at\":1}";
    for (int index = 0; index < 2; ++index) {
        const auto client = accept(listener, nullptr, nullptr);
        require(client != INVALID_SOCKET, "fixture accepts quota and shutdown requests");
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            require(count > 0, "fixture reads complete request");
            request.append(buffer, count);
        }
        noise(128);
        std::ofstream(root / (index == 0 ? "request-noise-complete" : "shutdown-noise-complete")) << "128 MiB";
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::string body = index == 0
            ? R"({"code":0,"data":{"kind":"ok","quota":{"usages":{"limit5h":{"usedRatio":0.25}}}}})"
            : "{}";
        const auto reply = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        require(send(client, reply.data(), (int)reply.size(), 0) == (int)reply.size(), "fixture sends reply");
        closesocket(client);
    }
    closesocket(listener);
    WSACleanup();
}

void noisy_output_test(const fs::path& executable, const fs::path& probe, const fs::path& root)
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("KIMI_QUOTA_TEST_NOISE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("KIMI_CODE_HOME"), QString::fromStdString(addon::path_to_utf8(root)));
    process.setProcessEnvironment(environment);
    process.start(QString::fromStdString(addon::path_to_utf8(probe)),
        {QStringLiteral("--kimi-program"), QString::fromStdString(addon::path_to_utf8(executable)),
            QStringLiteral("--kimi-code-home"), QString::fromStdString(addon::path_to_utf8(root))});
    require(process.waitForStarted(), "noisy quota helper starts");
    const auto handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)process.processId());
    require(handle != nullptr, "can measure quota helper resource use");
    SIZE_T peak = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!process.waitForFinished(10) && std::chrono::steady_clock::now() < deadline) {
        PROCESS_MEMORY_COUNTERS counters{};
        require(GetProcessMemoryInfo(handle, &counters, sizeof(counters)), "can read helper peak working set");
        peak = std::max(peak, counters.PeakWorkingSetSize);
    }
    CloseHandle(handle);
    require(process.state() == QProcess::NotRunning && process.exitCode() == 0, "noisy helper completes");
    require(fs::exists(root / "request-noise-complete") && fs::exists(root / "shutdown-noise-complete"),
        "child drains 128 MiB during both request and shutdown waits");
    const auto reply = addon::parse_json(process.readAllStandardOutput().toStdString(), {}, nullptr);
    const auto observed = reply ? inspect_reply(*reply) : std::nullopt;
    require(observed && observed->observation && observed->observation->state == addon::Quota_status::READY,
        "noisy native service still yields quota");
    std::cout << "helper peak working set: " << peak << " bytes; native stderr: 257 MiB\n";
    require(peak < 64 * 1024 * 1024, "native stderr volume must not accumulate in helper memory");
}
#endif

int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "web") {
#ifdef _WIN32
        if (std::getenv("KIMI_QUOTA_TEST_NOISE")) {
            try {
                noisy_server();
                return 0;
            }
            catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                return 1;
            }
        }
#endif
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
#ifdef _WIN32
        if (argc > 1 && std::string_view(argv[1]) == "noise") {
            noisy_output_test(executable, probe, root);
            fs::remove_all(root);
            return 0;
        }
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
        require(exited->observation && exited->observation->scope.source_account_key == "selected-kimi-account" &&
                exited->observation->scope.observed_at_ms > 0 && exited->observation->scope.fresh_for_ms == 60000,
            "classified process failure retains selected account and observation time");
        const auto neutral = inspect_reply(exited_reply, false);
        require(neutral && neutral->observation && neutral->observation->state == addon::Quota_status::FAILED &&
                neutral->observation->scope.source_account_key == "selected-kimi-account" && !neutral->failure_reason,
            "a host that did not ask for the cause receives a neutral failure under the selected account");
        fs::create_directories(root / "server");
        std::ofstream(root / "server/instances") << "not a directory";
        const auto registry = inspect_reply(inspect(false));
        require(registry && registry->diagnostic && registry->diagnostic->stage == addon::Quota_failure_stage::REGISTRY_READ &&
                registry->diagnostic->code != 0 && registry->failure_reason != exited->failure_reason,
            "registry failure preserves its filesystem category instead of readiness failure");
        require(registry->observation && registry->observation->scope.source_account_key == "selected-kimi-account",
            "classified registry failure retains selected account");
        const auto rejected = inspect_reply(kimi::quota_probe_failure("quota_request_rejected"));
        require(rejected && rejected->observation &&
                rejected->observation->scope.source_account_key == "selected-kimi-account" &&
                rejected->failure_reason == addon::quota_failure_reason(addon::Quota_failure::REQUEST_REJECTED),
            "rejected request retains selected account and category");
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
