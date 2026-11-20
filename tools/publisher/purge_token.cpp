#include "purge_token.h"

#include "key_window.h"
#include "floserver/core/http.h"

namespace floserver::publisher {

namespace {

namespace mt = floserver::manifest_tool;

constexpr const wchar_t* kTarget = L"FloServer Publisher: Cloudflare purge token";

mt::PurgeToken from(const std::vector<std::string>& v) { return {v.at(0), v.at(1)}; }

}  // namespace

std::optional<mt::PurgeToken> load_purge_token() {
    const auto cred = load_credential(kTarget);
    if (!cred) {
        return std::nullopt;
    }
    // UserName is the zone ID; the blob is the token.
    mt::PurgeToken t{cred->user, cred->secret};
    if (!mt::purge_token_problem(t).empty()) {
        return std::nullopt;
    }
    return t;
}

std::optional<mt::PurgeToken> ask_purge_token(HWND owner) {
    KeyWindowSpec spec;
    spec.title = L"Cloudflare purge token";
    spec.intro =
        L"Cloudflare dashboard \x2192 My Profile \x2192 API Tokens \x2192 Create Token \x2192 Custom token. "
        L"Permission: Zone, Cache Purge, Purge. Zone Resources: Include, Specific zone, floserver.org. The token "
        L"is shown once; the Zone ID is on floserver.org's overview page, under API.\r\n\r\nKept in Windows "
        L"Credential Manager on this PC only. It can purge floserver.org's cache and nothing else. Test purges "
        L"manifest.json and manifest.json.sig, which is harmless.";
    spec.fields = {{L"Zone ID", true}, {L"API token", false}};
    spec.asking = L"Asking Cloudflare…";
    spec.works = L"The token works: Cloudflare purged manifest.json and manifest.json.sig.";
    spec.refused = L"Cloudflare refused it: ";
    spec.forget_question = L"Forget the purge token on this PC? Publish then uploads the list but doesn't purge "
                           L"it from the cache.";
    spec.problem = [](const std::vector<std::string>& v) { return mt::purge_token_problem(from(v)); };
    spec.test = [](const std::vector<std::string>& v) -> std::string {
        auto http = core::make_system_transport();
        if (!http) {
            return "no HTTPS on this computer";
        }
        return mt::purge_cache(*http, from(v), mt::list_urls(mt::kCdn));
    };
    spec.load = []() -> std::optional<std::vector<std::string>> {
        const auto t = load_purge_token();
        if (!t) {
            return std::nullopt;
        }
        return std::vector<std::string>{t->zone_id, t->token};
    };
    spec.save = [](const std::vector<std::string>& v, std::string& error) {
        const mt::PurgeToken t = from(v);
        return save_credential(kTarget, t.zone_id, t.token, error);
    };
    spec.forget = [] { forget_credential(kTarget); };
    const auto saved = run_key_window(owner, spec);
    if (!saved) {
        return std::nullopt;
    }
    return from(*saved);
}

}  // namespace floserver::publisher
