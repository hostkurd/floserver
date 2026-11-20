#pragma once

// The window for a key kept in Windows Credential Manager (D34): the R2
// upload key (r2_key.h) and the Cloudflare purge token (purge_token.h).
// Fields, Test on a worker thread, Save, Forget the key, Cancel. The last
// field is the secret: left empty, the saved one is kept.

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace floserver::publisher {

struct KeyField {
    std::wstring label;
    bool hex = false;  // A-F typed are read as a-f
};

struct KeyWindowSpec {
    std::wstring title;  // "R2 upload key"
    std::wstring intro;  // where to make it, what it can do
    std::vector<KeyField> fields;
    std::wstring asking;   // "Asking R2…"
    std::wstring works;    // "The key works: …"
    std::wstring refused;  // "R2 refused it: "
    std::wstring forget_question;
    // What's wrong with the values typed, or empty.
    std::function<std::string(const std::vector<std::string>&)> problem;
    // On a worker thread: empty when the key works, otherwise why not.
    std::function<std::string(const std::vector<std::string>&)> test;
    std::function<std::optional<std::vector<std::string>>()> load;
    std::function<bool(const std::vector<std::string>&, std::string& error)> save;
    std::function<void()> forget;
};

// Modal over `owner`. The values saved, or nothing when it was closed or the key forgotten.
std::optional<std::vector<std::string>> run_key_window(HWND owner, const KeyWindowSpec& spec);

// A generic credential: UserName and a secret blob, on this PC only.
struct StoredCredential {
    std::string user;
    std::string secret;
};
[[nodiscard]] std::optional<StoredCredential> load_credential(const wchar_t* target);
[[nodiscard]] bool save_credential(const wchar_t* target, const std::string& user, const std::string& secret,
                                   std::string& error);
void forget_credential(const wchar_t* target);

}  // namespace floserver::publisher
