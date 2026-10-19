#include "kimi_quota_direct.h"

#include <QLocale>

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace json_util = logonomic::cli_addon::json;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

kimi::Json json(std::string_view text)
{
    const auto result = json_util::parse(text);
    require(result.has_value(), "fixture JSON parses");
    return *result;
}

std::string config(std::string_view base, std::string_view key, std::string_view oauth = {})
{
    return "[providers.'managed:kimi-code']\ntype='kimi'\napi_key=''\nbase_url='" + std::string(base) +
        "'\n[providers.'managed:kimi-code'.oauth]\nstorage='file'\nkey='" + std::string(key) + "'\n" +
        std::string(oauth);
}

void routes()
{
    // Oracle: upstream managed-kimi-code.ts resolveKimiCodeRuntimeAuth and
    // resolveKimiCodeOAuthKey, including JSON property order in the scope hash.
    const auto mainland = config("https://api.kimi.com/coding/v1", "oauth/kimi-code");
    const auto selected = kimi::direct_quota_route(mainland, {});
    require(selected && selected->host == "api.kimi.com" && selected->credential_name == "kimi-code",
        "mainland default uses its native file slot");
    const auto global = kimi::direct_quota_route(config("https://api.kimi.ai/coding/v1",
        "oauth/kimi-code-env-0e4f99c69cc27850", "oauth_host='https://auth.kimi.ai'\n"), {});
    require(global && global->host == "api.kimi.ai" && global->credential_name == "kimi-code-env-0e4f99c69cc27850",
        "global configuration uses the scope belonging to both endpoints");
    const auto overridden = kimi::direct_quota_route(mainland, {{"KIMI_CODE_BASE_URL", "https://api.kimi.ai/coding/v1/"}});
    require(overridden && overridden->credential_name == "kimi-code-env-d44abaad1d85681f",
        "an API-only environment override keeps the native default OAuth host");
    const auto mismatched = kimi::direct_quota_route(config("https://api.kimi.ai/coding/v1", "oauth/kimi-code"), {});
    require(mismatched && mismatched->credential_name == "kimi-code-env-d44abaad1d85681f",
        "a stale configured key is replaced by the native derived file scope");
    for (const char* endpoint : {"http://api.kimi.com/coding/v1", "https://other.example/coding/v1",
        "https://api.kimi.com/coding/v1?redirect=other", "https://api.kimi.com@other.example/coding/v1"})
    {
        require(!kimi::direct_quota_route(config(endpoint, "oauth/kimi-code"), {}),
            "unsupported destination falls back before reading a credential");
    }
    for (const char* name : {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "ALL_PROXY", "NODE_OPTIONS",
        "NODE_EXTRA_CA_CERTS", "SSL_CERT_FILE", "KIMI_CODE_CUSTOM_HEADERS"})
    {
        require(!kimi::direct_quota_route(mainland, {{name, "configured"}}),
            "native networking overrides retain the native route");
    }
    auto keyring = mainland;
    keyring.replace(keyring.find("storage='file'"), 14, "storage='keyring'");
    require(!kimi::direct_quota_route(keyring, {}), "keyring credentials remain native-owned");
    require(!kimi::direct_quota_route("invalid=[", {}), "invalid configuration remains native-owned");
    require(!kimi::direct_quota_route("", {}), "unproven configuration remains native-owned");
    // Kimi validates the entire providers section before selecting a provider.
    // A bad sibling discards that section, changing which file slot is selected.
    require(!kimi::direct_quota_route(mainland + "\n[providers.other]\napi_key=1\n", {}),
        "an invalid sibling cannot bypass native provider-section validation");
    const auto aliases = config("https://api.kimi.ai/coding/v1", "oauth/kimi-code") +
        "oauthHost='https://auth.kimi.com'\n";
    require(!kimi::direct_quota_route(aliases, {}),
        "native TOML key aliases stay native when their normalization is not reproduced");
}

void credentials()
{
    // Oracle: OAuthManager.defaultRefreshThreshold is max(300, expires_in/2)
    // seconds. Admission must also cover the whole remaining observation.
    auto token = json(R"({"access_token":"test-access-token","expires_at":2000,"expires_in":900})");
    require(kimi::current_quota_access_token(token, 1400, 30).has_value(), "a native-current token is eligible");
    require(!kimi::current_quota_access_token(token, 1540, 30), "renewal threshold plus deadline stays native");
    require(!kimi::current_quota_access_token(token, 2001, 30), "expired credentials stay native");
    token.set("access_token", kimi::Json(""));
    require(!kimi::current_quota_access_token(token, 1400, 30), "revoked tombstone stays native");
    token.set("access_token", kimi::Json("token\r\nInjected: value"));
    require(!kimi::current_quota_access_token(token, 1400, 30), "credential cannot create another HTTP header");
    token.set("access_token", kimi::Json("test-access-token"));
    token.set("expires_in", kimi::Json(3600));
    require(!kimi::current_quota_access_token(token, 1400, 30), "renewal is lifetime-derived, not a 900s assumption");
}

void responses()
{
    // Oracle: managed-usage.ts exposes used_ratio/reset_time under optional
    // usages.limit_5h/limit_7d; quota@2 keeps over-limit values and absence.
    const auto both = kimi::normalize_managed_quota(json(R"({"usages":{
        "limit_5h":{"used_ratio":"1.25","reset_time":"2026-10-02T02:00:00Z"},
        "limit_7d":{"used_ratio":0.2}}})"));
    require(both && json_util::member(json_util::member(*both, "primary"), "used_percent").as_number() == 125 &&
        json_util::member(json_util::member(*both, "secondary"), "used_percent").as_number() == 20,
        "direct usage preserves native normalized percentages");
    const auto weekly = kimi::normalize_managed_quota(json(R"({"usages":{"limit_7d":{"used_ratio":0}}})"));
    require(weekly && !weekly->find("primary") && weekly->find("secondary"), "missing window stays absent");
    const auto empty = kimi::normalize_managed_quota(json(R"({"usages":{}})"));
    require(empty && json_util::text(*empty, "status") == "unavailable", "no windows does not mean zero usage");
    const auto missing = kimi::normalize_managed_quota(json("{}"));
    require(missing && json_util::text(*missing, "status") == "unavailable",
        "the managed endpoint's empty account response stays unavailable without a second request");
    require(!kimi::normalize_managed_quota(json(R"({"usages":{"limit_5h":{"used_ratio":"unknown"}}})")),
        "unsupported response preserves the native compatibility route");
}

void numeric_strings()
{
    // Oracle: the existing ratio contract accepts complete finite decimal
    // strings, independently of the user's locale; blank strings mean zero.
    const auto previous_locale = QLocale();
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    const auto parse_ratio = [](const std::string& text) {
        auto response = json(R"({"usages":{"limit_5h":{}}})");
        response.find("usages")->find("limit_5h")->set("used_ratio", kimi::Json(text));
        return kimi::normalize_managed_quota(response);
    };
    for (const auto& [text, expected] : {std::pair{" 1.25\t", 125.0}, {".5", 50.0}, {"5.", 500.0},
        {"1.25e-1", 12.5}, {"1e+1", 1000.0}, {"", 0.0}, {" \t", 0.0}, {"-0", 0.0}})
    {
        const auto result = parse_ratio(text);
        require(result && json_util::member(json_util::member(*result, "primary"), "used_percent").as_number() == expected,
            "complete decimal quota strings remain locale-independent");
    }
    for (const auto& text : {"+1.25", "1,25", "1.25suffix", "1.25 0", "0x1", "nan", "inf", "-inf",
        "1e9999", "1e-9999"})
    {
        require(!parse_ratio(text), "invalid, partial and non-finite quota strings stay native-owned");
    }
    require(!parse_ratio(std::string("1\0.25", 5)), "an embedded null cannot terminate a numeric quota string");
    QLocale::setDefault(previous_locale);
}

} // namespace

int main()
{
    try {
        routes();
        credentials();
        responses();
        numeric_strings();
        std::puts("Kimi direct quota contracts passed");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
