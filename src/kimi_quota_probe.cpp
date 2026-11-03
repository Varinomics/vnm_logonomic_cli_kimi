#include "kimi_quota.h"
#include "kimi_quota_direct.h"
#include "kimi_quota_http.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/quota.h>

#include <QProcess>
#include <QProcessEnvironment>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using kimi::Json;
using Clock = std::chrono::steady_clock;

struct Scrub_string
{
    std::string& bytes;
    ~Scrub_string() { std::fill(bytes.begin(), bytes.end(), '\0'); }
};

std::string read_file(const fs::path& path, std::size_t maximum)
{
    std::error_code error;
    if (fs::is_symlink(path, error) || error || !fs::is_regular_file(path, error) || error) {
        throw std::runtime_error("invalid_quota_file");
    }
    const auto size = fs::file_size(path, error);
    if (error || size > maximum) {
        throw std::runtime_error("invalid_quota_file");
    }
    std::ifstream input(path, std::ios::binary);
    std::string result(static_cast<std::size_t>(size) + 1, '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<std::size_t>(input.gcount()));
    // The size observation is a hint: accept subsequent growth up to the same limit.
    while (input && result.size() <= maximum) {
        const auto offset = result.size();
        const auto size = std::min(maximum + 1, std::max<std::size_t>(4096, offset * 2));
        // Reserve from empty so string growth does not double an existing capacity past the limit.
        std::string grown;
        grown.reserve(size);
        grown.append(result);
        result = std::move(grown);
        result.resize(size);
        input.read(result.data() + offset, static_cast<std::streamsize>(result.size() - offset));
        result.resize(offset + static_cast<std::size_t>(input.gcount()));
    }
    if (result.empty() || result.size() > maximum || input.bad()) throw std::runtime_error("invalid_quota_file");
    return result;
}

class Probe_failure : public std::runtime_error
{
public:
    Probe_failure(const char* reason, addon::Quota_failure_stage stage, std::int64_t code)
    :
        std::runtime_error(reason),
        diagnostic{stage, code}
    {}

    addon::Quota_diagnostic diagnostic;
};

class Owned_server
{
public:
    explicit Owned_server(const std::string& program)
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto& name : environment.keys()) {
#ifdef _WIN32
            constexpr auto sensitivity = Qt::CaseInsensitive;
#else
            constexpr auto sensitivity = Qt::CaseSensitive;
#endif
            if (name.startsWith(QStringLiteral("LOGONOMIC_"), sensitivity) ||
                name.startsWith(QStringLiteral("VNM_CONTROL_"), sensitivity))
            {
                environment.remove(name);
            }
        }
        m_process.setProcessEnvironment(environment);
        // The native server may log without limit while the helper waits on it;
        // no output of it explains a failure, so none is kept.
        m_process.setStandardInputFile(QProcess::nullDevice());
        m_process.setStandardOutputFile(QProcess::nullDevice());
        m_process.setStandardErrorFile(QProcess::nullDevice());
        m_process.start(QString::fromUtf8(program),
            {QStringLiteral("web"), QStringLiteral("--no-open"), QStringLiteral("--host"),
                QStringLiteral("127.0.0.1"), QStringLiteral("--port"), QStringLiteral("0")});
        if (!m_process.waitForStarted()) {
            throw Probe_failure("quota_start_failed", addon::Quota_failure_stage::PROCESS_START,
                static_cast<int>(m_process.error()));
        }
        m_pid = static_cast<std::uint64_t>(m_process.processId());
    }

    ~Owned_server()
    {
        if (alive()) {
            m_process.terminate();
            if (!m_process.waitForFinished(1000)) {
                m_process.kill();
                m_process.waitForFinished(-1);
            }
        }
    }

    Owned_server(const Owned_server&) = delete;
    Owned_server& operator=(const Owned_server&) = delete;
    std::uint64_t pid() const { return m_pid; }
    bool alive()
    {
        m_process.waitForFinished(0);
        return m_process.state() != QProcess::NotRunning;
    }
    [[noreturn]] void failed()
    {
        // Native text can contain settings and credentials. Exit status proves
        // process failure; no substring of private output proves a safe cause.
        throw Probe_failure("quota_server_exited", addon::Quota_failure_stage::PROCESS_EXIT,
            m_process.exitCode());
    }
    void wait_for_shutdown()
    {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (alive() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

private:
    QProcess m_process;
    std::uint64_t m_pid = 0;
};

std::set<std::string> registry_names(const fs::path& directory)
{
    std::set<std::string> result;
    std::error_code error;
    for (fs::directory_iterator item(directory, error), end; !error && item != end; item.increment(error)) {
        if (item->path().extension() == ".json") result.insert(kimi::path_string(item->path().filename()));
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        throw Probe_failure("quota_registry_unavailable", addon::Quota_failure_stage::REGISTRY_READ, error.value());
    }
    return result;
}

bool valid_integer(const Json& value, std::uint64_t minimum, std::uint64_t maximum)
{
    return value.is_integer() && value.as_integer() >= 0 &&
        static_cast<std::uint64_t>(value.as_integer()) >= minimum && static_cast<std::uint64_t>(value.as_integer()) <= maximum;
}

std::optional<unsigned short> owned_port(const fs::path& directory, std::uint64_t pid,
    const std::set<std::string>& preexisting)
{
    std::optional<unsigned short> found;
    for (const auto& name : registry_names(directory)) {
        if (preexisting.contains(name)) continue;
        std::optional<Json> document;
        try { document = json_util::parse(read_file(directory / addon::path_from_utf8(name), 16384)); }
        catch (const std::exception&) { continue; }
        if (!document || !valid_integer(json_util::member(*document, "pid"), 1, 9007199254740991ull) ||
            static_cast<std::uint64_t>(json_util::member(*document, "pid").as_integer()) != pid) continue;
        const auto id = json_util::text(*document, "server_id");
        if (id.empty() || id.size() > 128 || name != id + ".json" || json_util::text(*document, "host") != "127.0.0.1" ||
            !valid_integer(json_util::member(*document, "port"), 0, 65535) ||
            !valid_integer(json_util::member(*document, "started_at"), 1, 9007199254740991ull) ||
            !valid_integer(json_util::member(*document, "heartbeat_at"), 1, 9007199254740991ull)) throw std::runtime_error("invalid_owned_server");
        const auto port = json_util::member(*document, "port").as_integer();
        if (port == 0) continue;
        if (found) throw std::runtime_error("ambiguous_owned_server");
        found = static_cast<unsigned short>(port);
    }
    return found;
}

class Socket
{
public:
#ifdef _WIN32
    Socket()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("socket_start_failed");
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == INVALID_SOCKET) { WSACleanup(); throw std::runtime_error("socket_start_failed"); }
    }
    ~Socket() { closesocket(m_socket); WSACleanup(); }
    SOCKET value() const { return m_socket; }
#else
#ifdef __APPLE__
    Socket() : m_socket(socket(AF_INET, SOCK_STREAM, 0))
#else
    Socket() : m_socket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
#endif
    {
        if (m_socket < 0) throw std::runtime_error("socket_start_failed");
#ifdef __APPLE__
        const auto flags = fcntl(m_socket, F_GETFD, 0);
        const int no_sigpipe = 1;
        if (flags < 0 || fcntl(m_socket, F_SETFD, flags | FD_CLOEXEC) != 0 ||
            setsockopt(m_socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
            close(m_socket);
            m_socket = -1;
            throw std::runtime_error("socket_start_failed");
        }
#endif
    }
    ~Socket() { close(m_socket); }
    int value() const { return m_socket; }
#endif
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

private:
#ifdef _WIN32
    SOCKET m_socket = INVALID_SOCKET;
#else
    int m_socket = -1;
#endif
};

void wait_socket(const Socket& socket, bool writing, Clock::time_point deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) throw std::runtime_error("quota_http_timeout");
    fd_set selected;
    FD_ZERO(&selected);
    FD_SET(socket.value(), &selected);
    timeval timeout{};
    timeout.tv_sec  = static_cast<decltype(timeout.tv_sec)>(remaining / 1000000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(remaining % 1000000);
    const auto ready = select(static_cast<int>(socket.value() + 1), writing ? nullptr : &selected,
        writing ? &selected : nullptr, nullptr, &timeout);
    if (ready <= 0) throw std::runtime_error("quota_http_timeout");
}

std::string http(unsigned short port, const std::string& token, bool shutdown, Clock::time_point operation_deadline)
{
    Socket socket;
    const auto deadline = std::min(operation_deadline,
        Clock::now() + std::chrono::milliseconds(shutdown ? 3000 : 10000));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(socket.value(), FIONBIO, &nonblocking) != 0) throw std::runtime_error("quota_socket_failed");
#else
    const auto flags = fcntl(socket.value(), F_GETFL, 0);
    if (flags < 0 || fcntl(socket.value(), F_SETFL, flags | O_NONBLOCK) != 0) throw std::runtime_error("quota_socket_failed");
#endif
    if (connect(socket.value(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
#ifdef _WIN32
        if (WSAGetLastError() != WSAEWOULDBLOCK) throw std::runtime_error("quota_connect_failed");
#else
        if (errno != EINPROGRESS) throw std::runtime_error("quota_connect_failed");
#endif
        wait_socket(socket, true, deadline);
        int error = 0;
#ifdef _WIN32
        int size = sizeof(error);
        if (getsockopt(socket.value(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0 || error != 0)
#else
        socklen_t size = sizeof(error);
        if (getsockopt(socket.value(), SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0)
#endif
            throw std::runtime_error("quota_connect_failed");
    }
    std::string request = shutdown ? "POST /api/v1/shutdown" : "GET /api/v1/oauth/usage?provider=managed%3Akimi-code";
    request += " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nAuthorization: Bearer " + token +
        "\r\nAccept: application/json\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    Scrub_string scrub_request{request};
    std::size_t offset = 0;
    while (offset < request.size()) {
        wait_socket(socket, true, deadline);
        const auto count = send(socket.value(), request.data() + offset, static_cast<int>(request.size() - offset),
#if defined(_WIN32) || defined(__APPLE__)
            0);
#else
            MSG_NOSIGNAL);
#endif
        if (count <= 0) throw std::runtime_error("quota_send_failed");
        offset += static_cast<std::size_t>(count);
    }
    std::fill(request.begin(), request.end(), '\0');
    std::string response;
    std::array<char, 8192> buffer{};
    for (;;) {
        wait_socket(socket, false, deadline);
        const auto count = recv(socket.value(), buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count == 0) break;
        if (count < 0) throw std::runtime_error("quota_receive_failed");
        if (response.size() + static_cast<std::size_t>(count) > 280 * 1024) throw std::runtime_error("quota_response_limit");
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    const auto separator = response.find("\r\n\r\n");
    if (separator == std::string::npos || separator > 16384 ||
        (!response.starts_with("HTTP/1.1 200 ") && !response.starts_with("HTTP/1.0 200 "))) throw std::runtime_error("quota_http_failed");
    std::optional<std::uint64_t> content_length;
    bool chunked = false;
    auto header_cursor = response.find("\r\n") + 2;
    while (header_cursor < separator) {
        const auto end = response.find("\r\n", header_cursor);
        const auto colon = response.find(':', header_cursor);
        if (end == std::string::npos || colon >= end) throw std::runtime_error("invalid_http_header");
        const auto name = kimi::lower(response.substr(header_cursor, colon - header_cursor));
        const auto value = kimi::trim(response.substr(colon + 1, end - colon - 1));
        if (name == "content-length") {
            std::uint64_t length = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
            if (content_length || value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                length > 256 * 1024) throw std::runtime_error("invalid_http_length");
            content_length = length;
        }
        else
        if (name == "transfer-encoding") {
            if (chunked || kimi::lower(value) != "chunked") throw std::runtime_error("invalid_http_encoding");
            chunked = true;
        }
        header_cursor = end + 2;
    }
    if (chunked && content_length) throw std::runtime_error("ambiguous_http_length");
    auto body = response.substr(separator + 4);
    if (content_length && *content_length != body.size()) throw std::runtime_error("invalid_http_length");
    if (chunked) {
        std::string decoded;
        std::size_t cursor = 0;
        for (;;) {
            const auto end = body.find("\r\n", cursor);
            if (end == std::string::npos) throw std::runtime_error("invalid_http_chunk");
            const auto extension = body.find(';', cursor);
            const auto size_end = extension < end ? extension : end;
            std::uint64_t size = 0;
            const auto parsed = std::from_chars(body.data() + cursor, body.data() + size_end, size, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != body.data() + size_end || size > 256 * 1024) throw std::runtime_error("invalid_http_chunk");
            cursor = end + 2;
            if (size == 0) {
                if (body.substr(cursor) != "\r\n" && body.find("\r\n\r\n", cursor) != body.size() - 4) {
                    throw std::runtime_error("invalid_http_chunk");
                }
                break;
            }
            if (size > body.size() - cursor || cursor + size + 2 > body.size() || body.substr(cursor + size, 2) != "\r\n" ||
                decoded.size() + size > 256 * 1024) throw std::runtime_error("invalid_http_chunk");
            decoded.append(body, cursor, static_cast<std::size_t>(size));
            cursor += static_cast<std::size_t>(size) + 2;
        }
        body = std::move(decoded);
    }
    if (body.size() > 256 * 1024) throw std::runtime_error("quota_response_limit");
    return body;
}

std::optional<Json> direct_quota(const fs::path& directory, Clock::time_point deadline)
{
    try {
        const auto route = kimi::direct_quota_route(
            read_file(directory / "config.toml", 4 * 1024 * 1024), addon::current_process_environment());
        if (!route) {
            return std::nullopt;
        }
        auto bytes = read_file(directory / "credentials" / (route->credential_name + ".json"), 4 * 1024 * 1024);
        Scrub_string scrub_bytes{bytes};
        const auto record = json_util::parse(bytes);
        if (!record) {
            return std::nullopt;
        }
        const auto now       = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto remaining = std::chrono::duration<double>(deadline - Clock::now()).count();
        auto token = kimi::current_quota_access_token(*record, now, remaining);
        if (!token) {
            return std::nullopt;
        }
        Scrub_string scrub_token{*token};
        const auto raw = kimi::get_managed_quota(route->host, *token, deadline);
        if (!raw) {
            return std::nullopt;
        }
        const auto response = json_util::parse(*raw);
        return response ? kimi::normalize_managed_quota(*response) : std::nullopt;
    }
    catch (const std::runtime_error&) {
        // An unreadable/rotating credential or unsupported reply is left to
        // the native manager; no exception text crosses the helper boundary.
        return std::nullopt;
    }
}

Json run(const std::string& program, const std::string& home)
{
    // Matches the host's quota operation budget. Its process job/cgroup remains
    // authoritative for cancellation and generation retirement, including DNS
    // or native startup that cannot finish inside the remaining time.
    const auto operation_deadline = Clock::now() + std::chrono::seconds(30);
    const auto executable = kimi::regular_file(addon::path_from_utf8(program));
    const auto directory = addon::path_from_utf8(home);
    if (!executable || !kimi::native_image(*executable) || !directory.is_absolute()) throw std::runtime_error("invalid_quota_request");
    if (auto direct = direct_quota(directory, operation_deadline)) {
        return std::move(*direct);
    }
    if (Clock::now() >= operation_deadline) {
        throw std::runtime_error("quota_http_timeout");
    }
    const auto registry = directory / "server" / "instances";
    const auto preexisting = registry_names(registry);
    Owned_server child(kimi::path_string(*executable));
    const auto deadline = std::min(operation_deadline, Clock::now() + std::chrono::seconds(10));
    std::optional<unsigned short> port;
    while (child.alive() && Clock::now() < deadline) {
        port = owned_port(registry, child.pid(), preexisting);
        if (port) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!child.alive()) child.failed();
    if (!port) throw std::runtime_error("quota_server_not_ready");
    auto token = kimi::trim(read_file(directory / "server.token", 16384));
    Scrub_string scrub_token{token};
    if (token.empty() || token.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("invalid_quota_token");
    try {
        const auto raw = http(*port, token, false, operation_deadline);
        const auto response = json_util::parse(raw);
        if (!response) throw std::runtime_error("invalid_quota_response");
        auto result = kimi::normalize_quota(*response);
        try { http(*port, token, true, operation_deadline); } catch (const std::exception&) {}
        std::fill(token.begin(), token.end(), '\0');
        child.wait_for_shutdown();
        return result;
    }
    catch (...) {
        try { http(*port, token, true, operation_deadline); } catch (const std::exception&) {}
        std::fill(token.begin(), token.end(), '\0');
        child.wait_for_shutdown();
        throw;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    Json result;
    try {
        std::vector<std::string> arguments;
        for (int index = 0; index < argc; ++index) {
#ifdef _WIN32
            arguments.push_back(kimi::path_string(fs::path(argv[index])));
#else
            arguments.emplace_back(argv[index]);
#endif
        }
        if (arguments.size() != 5 || arguments[1] != "--kimi-program" || arguments[3] != "--kimi-code-home") {
            throw std::runtime_error("invalid_quota_request");
        }
        result = run(arguments[2], arguments[4]);
    }
    catch (const Probe_failure& error) {
        const auto category = error.diagnostic.stage == addon::Quota_failure_stage::REGISTRY_READ
            ? addon::Quota_failure::REGISTRY_FAILED : addon::Quota_failure::PROCESS_FAILED;
        result = json_util::object({{"status", Json("error")},
            {"reason_code", Json(addon::quota_failure_reason(category))},
            {"diagnostic", addon::encode_quota_diagnostic(error.diagnostic)}});
    }
    catch (const std::exception& error) {
        result = kimi::quota_probe_failure(error.what());
    }
    const auto encoded = addon::serialize_json(result) + '\n';
    std::fwrite(encoded.data(), 1, encoded.size(), stdout);
    return 0;
}
