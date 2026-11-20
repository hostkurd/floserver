#include "r2_key.h"

#include "key_window.h"
#include "floserver/core/http.h"

namespace floserver::publisher {

namespace {

namespace mt = floserver::manifest_tool;

constexpr const wchar_t* kTarget = L"FloServer Publisher: R2 upload key";

mt::R2Key from(const std::vector<std::string>& v) { return {v.at(0), v.at(1), v.at(2)}; }

}  // namespace

std::optional<mt::R2Key> load_r2_key() {
    const auto cred = load_credential(kTarget);
    if (!cred) {
        return std::nullopt;
    }
    mt::R2Key key;
    // UserName is "<account id>:<access key id>"; the blob is the secret.
    if (const auto colon = cred->user.find(':'); colon != std::string::npos) {
        key.account_id = cred->user.substr(0, colon);
        key.access_key_id = cred->user.substr(colon + 1);
    }
    key.secret = cred->secret;
    if (!mt::r2_key_problem(key).empty()) {
        return std::nullopt;
    }
    return key;
}

bool save_r2_key(const mt::R2Key& key, std::string& error) {
    return save_credential(kTarget, key.account_id + ":" + key.access_key_id, key.secret, error);
}

void forget_r2_key() { forget_credential(kTarget); }

std::optional<mt::R2Key> ask_r2_key(HWND owner) {
    KeyWindowSpec spec;
    spec.title = L"R2 upload key";
    spec.intro =
        L"Cloudflare dashboard \x2192 R2 \x2192 Manage API tokens \x2192 Create API token. Permission: Object "
        L"Read & Write. Apply to specific buckets only: floserver-cdn. The Access Key ID and Secret Access Key "
        L"are shown once; the account ID is on R2's overview page.\r\n\r\nKept in Windows Credential Manager on "
        L"this PC only. It can write to floserver-cdn and nothing else; lists are still signed with your "
        L"offline key.";
    spec.fields = {{L"Account ID", true}, {L"Access Key ID", true}, {L"Secret Access Key", true}};
    spec.asking = L"Asking R2…";
    spec.works = L"The key works: R2 answered for floserver-cdn.";
    spec.refused = L"R2 refused it: ";
    spec.forget_question = L"Forget the R2 key on this PC? Get selected and Publish then can't upload until a key "
                           L"is entered again; Download only still works.";
    spec.problem = [](const std::vector<std::string>& v) { return mt::r2_key_problem(from(v)); };
    spec.test = [](const std::vector<std::string>& v) -> std::string {
        auto http = core::make_system_transport();
        if (!http) {
            return "no HTTPS on this computer";
        }
        mt::R2Bucket bucket(*http, from(v));
        return bucket.check();
    };
    spec.load = []() -> std::optional<std::vector<std::string>> {
        const auto k = load_r2_key();
        if (!k) {
            return std::nullopt;
        }
        return std::vector<std::string>{k->account_id, k->access_key_id, k->secret};
    };
    spec.save = [](const std::vector<std::string>& v, std::string& error) { return save_r2_key(from(v), error); };
    spec.forget = [] { forget_r2_key(); };
    const auto saved = run_key_window(owner, spec);
    if (!saved) {
        return std::nullopt;
    }
    return from(*saved);
}

}  // namespace floserver::publisher
