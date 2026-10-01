#include "kimi_quota_http.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace kimi {
namespace {

// Same raw response bound as the native loopback usage response.
constexpr std::size_t k_max_response_bytes = 256 * 1024;

long remaining_ms(std::chrono::steady_clock::time_point deadline)
{
    return static_cast<long>(std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count()));
}

#ifdef _WIN32

class Internet_handle
{
public:
    explicit Internet_handle(HINTERNET value)
    :
        m_value(value)
    {}
    ~Internet_handle() { if (m_value) { WinHttpCloseHandle(m_value); } }
    Internet_handle(const Internet_handle&) = delete;
    Internet_handle& operator=(const Internet_handle&) = delete;
    HINTERNET get() const { return m_value; }

private:
    HINTERNET m_value;
};

#else

std::size_t append_body(char* bytes, std::size_t size, std::size_t count, void* context)
{
    auto& body = *static_cast<std::string*>(context);
    if (size && count > k_max_response_bytes / size) {
        return 0;
    }
    const auto length = size * count;
    if (length > k_max_response_bytes - body.size()) {
        return 0;
    }
    body.append(bytes, length);
    return length;
}

#endif

} // namespace

std::optional<std::string> get_managed_quota(
    std::string_view host,
    std::string_view bearer,
    std::chrono::steady_clock::time_point deadline)
{
    if (remaining_ms(deadline) == 0) {
        return std::nullopt;
    }
#ifdef _WIN32
    Internet_handle session(WinHttpOpen(L"Logonomic/KimiQuota", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    const auto timeout = remaining_ms(deadline);
    if (!session.get() || timeout == 0 || !WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout)) {
        return std::nullopt;
    }
    const std::wstring hostname(host.begin(), host.end());
    Internet_handle connection(WinHttpConnect(session.get(), hostname.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.get()) {
        return std::nullopt;
    }
    Internet_handle request(WinHttpOpenRequest(connection.get(), L"GET", L"/coding/v1/usages", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!request.get() || !WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
        &redirect_policy, sizeof(redirect_policy)))
    {
        return std::nullopt;
    }
    std::wstring headers = L"Authorization: Bearer " + std::wstring(bearer.begin(), bearer.end()) +
        L"\r\nAccept: application/json\r\n";
    const auto sent = WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!sent || remaining_ms(deadline) == 0 || !WinHttpReceiveResponse(request.get(), nullptr)) {
        return std::nullopt;
    }
    DWORD status      = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200)
    {
        return std::nullopt;
    }
    std::string body;
    for (;;) {
        const auto remaining = remaining_ms(deadline);
        if (remaining == 0 || !WinHttpSetTimeouts(request.get(), remaining, remaining, remaining, remaining)) {
            return std::nullopt;
        }
        char buffer[8192];
        DWORD count = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &count)) {
            return std::nullopt;
        }
        if (count == 0) {
            return body;
        }
        if (count > k_max_response_bytes - body.size()) {
            return std::nullopt;
        }
        body.append(buffer, count);
    }
#else
    static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready) {
        return std::nullopt;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        return std::nullopt;
    }
    std::string authorization = "Authorization: Bearer " + std::string(bearer);
    curl_slist* headers       = curl_slist_append(nullptr, authorization.c_str());
    std::fill(authorization.begin(), authorization.end(), '\0');
    if (!headers) {
        curl_easy_cleanup(curl);
        return std::nullopt;
    }
    auto* complete_headers = curl_slist_append(headers, "Accept: application/json");
    if (!complete_headers) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return std::nullopt;
    }
    headers = complete_headers;
    const auto url = "https://" + std::string(host) + "/coding/v1/usages";
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, std::max(1L, remaining_ms(deadline)));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const auto code = curl_easy_perform(curl);
    long status = 0;
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status != 200) {
        return std::nullopt;
    }
    return body;
#endif
}

} // namespace kimi
