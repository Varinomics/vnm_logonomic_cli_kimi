#include "kimi_quota.h"
#include "kimi_quota_direct.h"
#include "kimi_quota_http.h"
#include "kimi_launcher.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using kimi::Json;
using kimi::member;
using kimi::text;
using Clock = std::chrono::steady_clock;

struct Scrub_string
{
    std::string& bytes;
    ~Scrub_string() { std::fill(bytes.begin(), bytes.end(), '\0'); }
};

std::string read_file(const fs::path& path, std::size_t maximum)
{
    std::error_code error;
    if (fs::is_symlink(path, error) || error || !fs::is_regular_file(path, error) || error ||
        fs::file_size(path, error) > maximum || error) throw std::runtime_error("invalid_quota_file");
    std::ifstream input(path, std::ios::binary);
    std::string result(maximum + 1, '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<std::size_t>(input.gcount()));
    if (result.empty() || result.size() > maximum || input.bad()) throw std::runtime_error("invalid_quota_file");
    return result;
}

#ifdef _WIN32
std::wstring wide(std::string_view text)
{
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length == 0 && !text.empty()) throw std::runtime_error("invalid_utf8_path");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::wstring quote(std::wstring_view input)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : input) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        result += character;
        slashes = 0;
    }
    result.append(2 * slashes, L'\\');
    return result + L'"';
}
#endif

class Owned_server
{
public:
    explicit Owned_server(const std::string& program)
    {
#ifdef _WIN32
        auto command = quote(wide(program)) + L" web --no-open --host 127.0.0.1 --port 0";
        auto* block = GetEnvironmentStringsW();
        if (!block) throw std::runtime_error("quota_environment_failed");
        std::wstring environment;
        for (const auto* item = block; *item; item += wcslen(item) + 1) {
            const std::wstring value(item);
            if (_wcsnicmp(item, L"LOGONOMIC_", 10) == 0 || _wcsnicmp(item, L"VNM_CONTROL_", 12) == 0) continue;
            environment += value;
            environment += L'\0';
        }
        FreeEnvironmentStringsW(block);
        environment += L'\0';
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        auto null_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (null_handle == INVALID_HANDLE_VALUE) throw std::runtime_error("quota_start_failed");
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> storage(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        const bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &size) != FALSE;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = null_handle;
        startup.StartupInfo.hStdOutput = null_handle;
        startup.StartupInfo.hStdError = null_handle;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        const bool started = initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &null_handle, sizeof(null_handle), nullptr, nullptr) &&
            CreateProcessW(wide(program).c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                environment.data(), nullptr, &startup.StartupInfo, &process);
        if (initialized) DeleteProcThreadAttributeList(attributes);
        CloseHandle(null_handle);
        if (!started) throw std::runtime_error("quota_start_failed");
        m_process = process.hProcess;
        m_pid = process.dwProcessId;
        CloseHandle(process.hThread);
#else
        std::vector<std::string> variables;
        for (char** item = environ; *item; ++item) {
            const std::string value(*item);
            if (value.starts_with("LOGONOMIC_") || value.starts_with("VNM_CONTROL_")) continue;
            variables.push_back(value);
        }
        std::vector<char*> environment;
        for (auto& variable : variables) environment.push_back(variable.data());
        environment.push_back(nullptr);
        std::vector<std::string> values{program, "web", "--no-open", "--host", "127.0.0.1", "--port", "0"};
        std::vector<char*> arguments;
        for (auto& value : values) arguments.push_back(value.data());
        arguments.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        const auto error = posix_spawn(&m_pid, program.c_str(), &actions, nullptr, arguments.data(), environment.data());
        posix_spawn_file_actions_destroy(&actions);
        if (error != 0) { m_pid = 0; throw std::runtime_error("quota_start_failed"); }
#endif
    }

    ~Owned_server()
    {
#ifdef _WIN32
        if (m_process) {
            if (WaitForSingleObject(m_process, 0) != WAIT_OBJECT_0) TerminateProcess(m_process, 1);
            WaitForSingleObject(m_process, INFINITE);
            CloseHandle(m_process);
        }
#else
        if (m_pid != 0 && !m_reaped) {
            kill(m_pid, SIGTERM);
            const auto end = Clock::now() + std::chrono::seconds(1);
            while (alive() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (!m_reaped) {
                kill(m_pid, SIGKILL);
                int status = 0;
                while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {}
            }
        }
#endif
    }

    Owned_server(const Owned_server&) = delete;
    Owned_server& operator=(const Owned_server&) = delete;
    std::uint64_t pid() const { return static_cast<std::uint64_t>(m_pid); }
    bool alive()
    {
#ifdef _WIN32
        return WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
#else
        if (m_reaped) return false;
        int status = 0;
        const auto result = waitpid(m_pid, &status, WNOHANG);
        if (result == m_pid || (result < 0 && errno == ECHILD)) m_reaped = true;
        return !m_reaped;
#endif
    }
    void wait_for_shutdown()
    {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (alive() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

private:
#ifdef _WIN32
    HANDLE m_process = nullptr;
    DWORD m_pid = 0;
#else
    pid_t m_pid = 0;
    bool m_reaped = false;
#endif
};

std::set<std::string> registry_names(const fs::path& directory)
{
    std::set<std::string> result;
    std::error_code error;
    for (fs::directory_iterator item(directory, error), end; !error && item != end; item.increment(error)) {
        if (item->path().extension() == ".json") result.insert(kimi::path_string(item->path().filename()));
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
        try { document = kimi::parse(read_file(directory / addon::path_from_utf8(name), 16384)); }
        catch (const std::exception&) { continue; }
        if (!document || !valid_integer(member(*document, "pid"), 1, 9007199254740991ull) ||
            static_cast<std::uint64_t>(member(*document, "pid").as_integer()) != pid) continue;
        const auto id = text(*document, "server_id");
        if (id.empty() || id.size() > 128 || name != id + ".json" || text(*document, "host") != "127.0.0.1" ||
            !valid_integer(member(*document, "port"), 0, 65535) ||
            !valid_integer(member(*document, "started_at"), 1, 9007199254740991ull) ||
            !valid_integer(member(*document, "heartbeat_at"), 1, 9007199254740991ull)) throw std::runtime_error("invalid_owned_server");
        const auto port = member(*document, "port").as_integer();
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
        const auto record = kimi::parse(bytes);
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
        const auto response = kimi::parse(*raw);
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
    if (const auto direct = direct_quota(directory, operation_deadline)) {
        return *direct;
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
    if (!port || !child.alive()) throw std::runtime_error("quota_server_not_ready");
    auto token = kimi::trim(read_file(directory / "server.token", 16384));
    Scrub_string scrub_token{token};
    if (token.empty() || token.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("invalid_quota_token");
    try {
        const auto raw = http(*port, token, false, operation_deadline);
        const auto response = kimi::parse(raw);
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
    catch (const std::exception& error) {
        result = kimi::quota_probe_failure(error.what());
    }
    const auto encoded = addon::serialize_json(result) + '\n';
    std::fwrite(encoded.data(), 1, encoded.size(), stdout);
    return 0;
}
