#include "kimi_quota_direct.h"
#include "kimi_quota.h"

#include <logonomic_cli_addon/sha256.h>
#include <toml++/toml.hpp>

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kimi {
namespace {

namespace addon = logonomic::cli_addon;

std::string without_trailing_slashes(std::string value)
{
    while (value.ends_with('/')) {
        value.pop_back();
    }
    return value;
}

bool unsupported_network(const Environment& environment)
{
    for (const auto& [name, value] : environment) {
        const auto key = lower(name);
        if (!value.empty() &&
            (key == "http_proxy" || key == "https_proxy" || key == "all_proxy" || key == "no_proxy" ||
             key == "node_extra_ca_certs" || key == "node_tls_reject_unauthorized" || key == "node_options" ||
             key == "ssl_cert_file" || key == "ssl_cert_dir" || key == "curl_ca_bundle" ||
             key == "kimi_code_custom_headers"))
        {
            return true;
        }
    }
    return false;
}

bool admitted_provider_section(const toml::table& providers)
{
    // Kimi validates this section as a whole: an invalid sibling removes the
    // managed provider too. Admit a provably valid subset of native fields;
    // other forms (including camelCase aliases) need native normalization.
    for (const auto& [name, node] : providers) {
        (void)name;
        const auto* provider = node.as_table();
        if (!provider) {
            return false;
        }
        for (const auto& [key, value] : *provider) {
            const auto field = key.str();
            if (field == "oauth") {
                const auto* oauth = value.as_table();
                if (!oauth) {
                    return false;
                }
                const auto storage = (*oauth)["storage"].value<std::string>();
                const auto slot    = (*oauth)["key"].value<std::string>();
                if (!storage || (*storage != "file" && *storage != "keyring") || !slot || slot->empty()) {
                    return false;
                }
                for (const auto& [oauth_key, oauth_value] : *oauth) {
                    if ((oauth_key != "storage" && oauth_key != "key" && oauth_key != "oauth_host") ||
                        !oauth_value.is_string() || oauth_value.value_or(std::string{}).empty())
                    {
                        return false;
                    }
                }
            }
            else
            if (field == "type" || field == "api_key" || field == "base_url" ||
                field == "api_key_env" || field == "default_model")
            {
                if (!value.is_string()) {
                    return false;
                }
            }
            else {
                return false;
            }
        }
    }
    return true;
}

std::optional<double> ratio(const Json& value)
{
    if (value.is_number()) {
        return std::isfinite(value.as_number()) ? std::optional(value.as_number()) : std::nullopt;
    }
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto bytes = trim(value.as_string());
    if (bytes.empty()) {
        return 0.0; // The upstream parser uses JavaScript Number(string).
    }
    // Keep the decimal from_chars spelling while using QtCore's C-locale
    // conversion, available on every supported macOS deployment target.
    if (bytes.front() == '+' || bytes.find_first_not_of("0123456789.eE+-") != std::string::npos) {
        return std::nullopt;
    }
    bool parsed       = false;
    const auto result = QByteArray::fromStdString(bytes).toDouble(&parsed);
    if (!parsed || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

} // namespace

std::optional<Quota_route> direct_quota_route(std::string_view config, const Environment& environment)
{
    if (unsupported_network(environment)) {
        return std::nullopt;
    }
    try {
        const auto document = toml::parse(config);
        const auto* providers = document["providers"].as_table();
        if (!providers || !admitted_provider_section(*providers)) {
            return std::nullopt;
        }
        const auto provider = document["providers"]["managed:kimi-code"];
        const auto oauth    = provider["oauth"];
        // An explicit managed provider gives us a source-defined configuration
        // shape. Provider environment overlays and other credential forms stay native.
        if (!provider.is_table() || provider["type"].value<std::string>() != "kimi" || provider["api_key_env"] ||
            !provider["api_key"].value_or(std::string{}).empty() || !oauth.is_table())
        {
            return std::nullopt;
        }
        const auto configured_base = provider["base_url"].value<std::string>();
        const auto configured_key  = oauth["key"].value<std::string>();
        const auto storage         = oauth["storage"].value<std::string>();
        if (!configured_base || !configured_key || !storage) {
            return std::nullopt;
        }
        std::optional<std::string> env_base, env_host, old_env_host;
        for (const auto& [name, value] : environment) {
#ifdef _WIN32
            const auto key = lower(name);
#else
            const auto& key = name;
#endif
#ifdef _WIN32
            if (key == "kimi_code_base_url")   { env_base = value; }
            if (key == "kimi_code_oauth_host") { env_host = value; }
            if (key == "kimi_oauth_host")      { old_env_host = value; }
#else
            if (key == "KIMI_CODE_BASE_URL")   { env_base = value; }
            if (key == "KIMI_CODE_OAUTH_HOST") { env_host = value; }
            if (key == "KIMI_OAUTH_HOST")      { old_env_host = value; }
#endif
        }
        if (!env_host) {
            env_host = old_env_host;
        }
        const bool overridden = env_base.has_value() || env_host.has_value();
        const auto base       = without_trailing_slashes(env_base.value_or(*configured_base));
        const auto host       = without_trailing_slashes(trim(overridden
            ? env_host.value_or("https://auth.kimi.com")
            : oauth["oauth_host"].value_or(std::string("https://auth.kimi.com"))));
        if ((base != "https://api.kimi.com/coding/v1" && base != "https://api.kimi.ai/coding/v1") ||
            (host != "https://auth.kimi.com" && host != "https://auth.kimi.ai"))
        {
            return std::nullopt;
        }
        std::string name = "kimi-code";
        if (base != "https://api.kimi.com/coding/v1" || host != "https://auth.kimi.com") {
            // resolveKimiCodeOAuthKey hashes JSON in this property order.
            const auto scope = "{\"oauthHost\":\"" + host + "\",\"baseUrl\":\"" + base + "\"}";
            name += "-env-" + addon::to_hex(addon::Sha256::digest(scope)).substr(0, 16);
        }
        if (!overridden && *configured_key == "oauth/" + name && *storage != "file") {
            return std::nullopt;
        }
        // A mismatched configured key is replaced by the derived file ref by
        // Kimi's resolveKimiCodeRuntimeAuth, as is any environment override.
        return Quota_route{base == "https://api.kimi.ai/coding/v1" ? "api.kimi.ai" : "api.kimi.com", name};
    }
    catch (const toml::parse_error&) {
        return std::nullopt;
    }
}

std::optional<std::string> current_quota_access_token(const Json& record, double now, double remaining_seconds)
{
    const auto& token    = member(record, "access_token");
    const auto& expiry   = member(record, "expires_at");
    const auto& lifetime = member(record, "expires_in");
    if (!token.is_string() || token.as_string().empty() || !expiry.is_number() || !lifetime.is_number() ||
        !std::isfinite(expiry.as_number()) || !std::isfinite(lifetime.as_number()) ||
        !std::all_of(token.as_string().begin(), token.as_string().end(),
            [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; }))
    {
        return std::nullopt;
    }
    // OAuthManager.defaultRefreshThreshold: max(300s, expires_in / 2).
    // Reserve the whole observation's remaining deadline as well; the native
    // manager alone refreshes and persists credentials under its rotation lock.
    const auto required = std::max(300.0, lifetime.as_number() * 0.5) + remaining_seconds;
    if (expiry.as_number() - now < required) {
        return std::nullopt;
    }
    return token.as_string();
}

std::optional<Json> normalize_managed_quota(const Json& response)
{
    if (!response.is_object()) {
        return std::nullopt;
    }
    // The managed endpoint returns {} for accounts without quota windows;
    // parseManagedUsagePayload also maps absent/non-record usages to empty.
    auto usages = Json::object();
    for (const auto& [source, target] : {std::pair{"limit_5h", "limit5h"}, {"limit_7d", "limit7d"}}) {
        const auto* row = member(response, "usages").find(source);
        if (!row) {
            continue;
        }
        const auto used = ratio(member(*row, "used_ratio"));
        if (!used || *used < 0) {
            return std::nullopt;
        }
        auto window = object({{"usedRatio", Json(*used)}});
        const auto& reset = member(*row, "reset_time");
        if (reset.is_string() && !reset.as_string().empty()) {
            window.set("resetAt", reset);
        }
        usages.set(target, std::move(window));
    }
    try {
        return normalize_quota(object({{"code", Json(0)}, {"data", object({
            {"kind", Json("ok")}, {"quota", object({{"usages", std::move(usages)}})}})}}));
    }
    catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

} // namespace kimi
