#include "floserver/core/mailpit.h"

#include "floserver/core/crypto.h"
#include "floserver/core/encoding.h"
#include "floserver/core/child_process.h"
#include "floserver/core/integration.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/paths.h"
#include "floserver/core/sha256.h"
#include "floserver/core/version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::string_view kCryptAlphabet = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr unsigned kMailpitRounds = 1000;  // the least SHA-crypt allows; see mailpit_auth_line()

void add_repeated(Sha256& hash, const Sha256Digest& digest, std::size_t length) {
    for (; length >= digest.size(); length -= digest.size()) {
        hash.update(std::span<const std::uint8_t>(digest.data(), digest.size()));
    }
    hash.update(std::span<const std::uint8_t>(digest.data(), length));
}

std::span<const std::uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::optional<Port> port_value(const json& j) {
    if (j.is_number_integer() && j.get<std::int64_t>() > 0 && j.get<std::int64_t>() < 65536) {
        return static_cast<Port>(j.get<std::int64_t>());
    }
    return std::nullopt;
}

}  // namespace

bool runtime_version_allowed(std::string_view name, std::string_view version) {
    if (name != kMailpitRuntime) {
        return true;
    }
    const auto v = Version::parse(version);
    const auto floor = Version::parse(kMinMailpitVersion);
    return v && floor && !(*v < *floor);
}

MailSettings read_mail_settings(const AppConfig& app, std::vector<std::string>* problems) {
    MailSettings s;
    if (!app.other.is_object() || !app.other.contains("mail")) {
        return s;
    }
    const json& m = app.other.at("mail");
    const auto problem = [problems](std::string text) {
        if (problems != nullptr) {
            problems->push_back(std::move(text));
        }
    };
    if (!m.is_object()) {
        problem("\"mail\" isn't an object");
        return s;
    }
    for (const auto& [key, flag] : {std::pair<const char*, bool*>{"php", &s.php},
                                    {"notices", &s.notices},
                                    {"keep_running", &s.keep_running}}) {
        if (!m.contains(key)) {
            continue;
        }
        if (m.at(key).is_boolean()) {
            *flag = m.at(key).get<bool>();
        } else {
            problem(std::string(R"("mail": ")") + key + R"(" is true or false)");
        }
    }
    if (m.contains("max")) {
        const json& max = m.at("max");
        if (max.is_number_integer() && max.get<std::int64_t>() >= 0 && max.get<std::int64_t>() <= kMaxMailMax) {
            s.max = static_cast<int>(max.get<std::int64_t>());
        } else {
            problem(R"("mail": "max" is a number from 0 to )" + std::to_string(kMaxMailMax));
        }
    }
    if (m.contains("port")) {
        s.port = port_value(m.at("port"));
        if (!s.port) {
            problem(R"("mail": "port" is a port number)");
        }
    }
    return s;
}

void write_mail_settings(AppConfig& app, const MailSettings& settings) {
    if (!app.other.is_object()) {
        app.other = json::object();
    }
    json& m = app.other["mail"];
    if (!m.is_object()) {
        m = json::object();
    }
    m["php"] = settings.php;
    m["notices"] = settings.notices;
    m["keep_running"] = settings.keep_running;
    m["max"] = settings.max;
    if (settings.port) {
        m["port"] = *settings.port;
    } else {
        m.erase("port");
    }
}

MailpitPaths mailpit_paths(const fs::path& data) {
    MailpitPaths p;
    p.dir = data / "mailpit";
    p.database = p.dir / "mailpit.db";
    p.ui_auth = p.dir / "ui-auth";
    p.nginx_auth = p.dir / "nginx-auth.conf";
    p.status = p.dir / "status.json";
    p.log = data / "web" / "logs" / "mailpit.log";
    p.smtp_socket = p.dir / "smtp.sock";
    p.http_socket = p.dir / "http.sock";
    return p;
}

std::optional<fs::path> mailpit_program(const fs::path& install_dir) {
#ifdef _WIN32
    const fs::path name = "mailpit.exe";
#else
    const fs::path name = "mailpit";
#endif
    std::error_code ec;
    if (fs::is_regular_file(install_dir / name, ec)) {
        return install_dir / name;
    }
    // A release archive with everything in one folder.
    std::optional<fs::path> only;
    int folders = 0;
    for (fs::directory_iterator it(install_dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code inner;
        if (it->is_directory(inner) && !it->path().filename().native().starts_with('.')) {
            ++folders;
            only = it->path();
        }
    }
    if (folders == 1 && only && fs::is_regular_file(*only / name, ec)) {
        return *only / name;
    }
    return std::nullopt;
}

const InstalledRuntime* newest_mailpit(const std::vector<InstalledRuntime>& installed) {
    const InstalledRuntime* best = nullptr;
    for (const InstalledRuntime& r : installed) {
        if (r.name != kMailpitRuntime || !runtime_version_allowed(r.name, r.version)) {
            continue;
        }
        if (best == nullptr || Version::parse(r.version) > Version::parse(best->version)) {
            best = &r;
        }
    }
    return best;
}

const InstalledRuntime* newest_mailpit_any(const std::vector<InstalledRuntime>& installed) {
    const InstalledRuntime* best = nullptr;
    for (const InstalledRuntime& r : installed) {
        if (r.name == kMailpitRuntime &&
            (best == nullptr || Version::parse(r.version) > Version::parse(best->version))) {
            best = &r;
        }
    }
    return best;
}

std::string mailpit_socket_text(const fs::path& socket) {
    const std::u8string u = socket.generic_u8string();
    return {u.begin(), u.end()};
}

std::optional<std::string> mailpit_socket_problem(const MailpitPaths& paths) {
    for (const fs::path& socket : {paths.smtp_socket, paths.http_socket}) {
        const std::string text = mailpit_socket_text(socket);
        if (std::any_of(text.begin(), text.end(), [](char c) {
                const auto u = static_cast<unsigned char>(c);
                return u < 0x20 || u == 0x7F;
            })) {
            return "FloServer's data folder (" + clean_error_text(path_to_utf8(paths.dir)) +
                   ") has a control character in its path, so Mailpit can't make its sockets there";
        }
        if (text.size() > max_local_socket_path()) {
            return "FloServer's data folder path is too long for Mailpit's sockets (" + std::to_string(text.size()) +
                   " characters; at most " + std::to_string(max_local_socket_path()) +
                   "): set a shorter FLOSERVER_DATA_DIR";
        }
    }
    return std::nullopt;
}

std::vector<std::string> mailpit_args(const MailpitPaths& paths, int max) {
    return {"--smtp",
            "unix:" + mailpit_socket_text(paths.smtp_socket) + ":0600",
            "--listen",
            "unix:" + mailpit_socket_text(paths.http_socket) + ":0600",
            "--database",
            path_to_utf8(paths.database),
            "--max",
            std::to_string(std::clamp(max, 0, kMaxMailMax)),
            "--smtp-auth-accept-any",
            "--smtp-auth-allow-insecure",
            "--disable-version-check",
            "--ui-auth-file",
            path_to_utf8(paths.ui_auth)};
}

const std::vector<std::string>& mailpit_env_names() {
    // Every MP_* name Mailpit 1.33 reads (cmd/root.go, config/).
    static const std::vector<std::string> names = {
        "MP_ALLOWED_HOSTS", "MP_ALLOW_INTERNAL_HTTP_REQUESTS", "MP_ALLOW_UNTRUSTED_TLS", "MP_API_CORS",
        "MP_BLOCK_REMOTE_CSS_AND_FONTS", "MP_CHAOS_TRIGGERS", "MP_COMPRESSION", "MP_DATABASE", "MP_DATA_FILE",
        "MP_DEMO_MODE", "MP_DISABLE_AUTO_VACUUM", "MP_DISABLE_HTML_CHECK", "MP_DISABLE_HTTP_COMPRESSION",
        "MP_DISABLE_LINK_CHECK_RATE_LIMIT", "MP_DISABLE_VERSION_CHECK", "MP_DISABLE_WAL", "MP_ENABLE_CHAOS",
        "MP_ENABLE_PROMETHEUS", "MP_ENABLE_RSPAMD", "MP_ENABLE_SPAMASSASSIN", "MP_HIDE_DELETE_ALL_BUTTON",
        "MP_IGNORE_DUPLICATE_IDS", "MP_LABEL", "MP_LOG_FILE", "MP_MAX_AGE", "MP_MAX_MESSAGES", "MP_MAX_MESSAGE_SIZE",
        "MP_POP3_AUTH", "MP_POP3_AUTH_FILE", "MP_POP3_BIND_ADDR", "MP_POP3_TLS_CERT", "MP_POP3_TLS_KEY", "MP_QUIET",
        "MP_SEND_API_AUTH", "MP_SEND_API_AUTH_ACCEPT_ANY", "MP_SEND_API_AUTH_FILE", "MP_SMTP_ALLOWED_RECIPIENTS",
        "MP_SMTP_AUTH", "MP_SMTP_AUTH_ACCEPT_ANY", "MP_SMTP_AUTH_ALLOW_INSECURE", "MP_SMTP_AUTH_FILE",
        "MP_SMTP_BIND_ADDR", "MP_SMTP_DISABLE_RDNS", "MP_SMTP_FORWARD_ALLOW_INSECURE", "MP_SMTP_FORWARD_AUTH",
        "MP_SMTP_FORWARD_CONFIG", "MP_SMTP_FORWARD_FWD_SMTP_ERRORS", "MP_SMTP_FORWARD_HOST",
        "MP_SMTP_FORWARD_OVERRIDE_FROM", "MP_SMTP_FORWARD_PASSWORD", "MP_SMTP_FORWARD_PORT",
        "MP_SMTP_FORWARD_RETURN_PATH", "MP_SMTP_FORWARD_SECRET", "MP_SMTP_FORWARD_STARTTLS",
        "MP_SMTP_FORWARD_SUBJECT_PREFIX", "MP_SMTP_FORWARD_TLS", "MP_SMTP_FORWARD_TO", "MP_SMTP_FORWARD_USERNAME",
        "MP_SMTP_IGNORE_REJECTED_RECIPIENTS", "MP_SMTP_MAX_RECIPIENTS", "MP_SMTP_RELAY_ALL",
        "MP_SMTP_RELAY_ALLOWED_RECIPIENTS", "MP_SMTP_RELAY_ALLOW_INSECURE", "MP_SMTP_RELAY_AUTH",
        "MP_SMTP_RELAY_BLOCKED_RECIPIENTS", "MP_SMTP_RELAY_CONFIG", "MP_SMTP_RELAY_FWD_SMTP_ERRORS",
        "MP_SMTP_RELAY_HOST", "MP_SMTP_RELAY_MATCHING", "MP_SMTP_RELAY_OVERRIDE_FROM", "MP_SMTP_RELAY_PASSWORD",
        "MP_SMTP_RELAY_PORT", "MP_SMTP_RELAY_PRESERVE_MESSAGE_IDS", "MP_SMTP_RELAY_RETURN_PATH",
        "MP_SMTP_RELAY_SECRET", "MP_SMTP_RELAY_STARTTLS", "MP_SMTP_RELAY_SUBJECT_PREFIX", "MP_SMTP_RELAY_TLS",
        "MP_SMTP_RELAY_USERNAME", "MP_SMTP_REQUIRE_STARTTLS", "MP_SMTP_REQUIRE_TLS", "MP_SMTP_SMTP_KEY",
        "MP_SMTP_SSL_CERT", "MP_SMTP_SSL_KEY", "MP_SMTP_STRICT_RFC_HEADERS", "MP_SMTP_TLS_CERT", "MP_SMTP_TLS_KEY",
        "MP_SMTP_TLS_REQUIRED", "MP_STRICT_RFC_HEADERS", "MP_TAG", "MP_TAGS_CONFIG", "MP_TAGS_DISABLE",
        "MP_TAGS_TITLE_CASE", "MP_TAGS_USERNAME", "MP_TENANT_ID", "MP_UI_AUTH", "MP_UI_AUTH_FILE", "MP_UI_BIND_ADDR",
        "MP_UI_SSL_CERT", "MP_UI_SSL_KEY", "MP_UI_TLS_CERT", "MP_UI_TLS_KEY", "MP_USE_MESSAGE_DATES", "MP_VERBOSE",
        "MP_WEBHOOK_DELAY", "MP_WEBHOOK_LIMIT", "MP_WEBHOOK_URL", "MP_WEBROOT"};
    return names;
}

std::string sha256_crypt(std::string_view password, std::string_view salt_in, unsigned rounds) {
    const std::string_view salt = salt_in.substr(0, 16);
    const unsigned r = std::clamp(rounds, 1000U, 999999999U);
    const std::size_t pl = password.size();
    const std::size_t sl = salt.size();

    // B = H(P S P)
    Sha256 b_ctx;
    b_ctx.update(password);
    b_ctx.update(salt);
    b_ctx.update(password);
    const Sha256Digest b = b_ctx.finish();
    // A = H(P S B..., then B or P for each bit of len(P))
    Sha256 a_ctx;
    a_ctx.update(password);
    a_ctx.update(salt);
    add_repeated(a_ctx, b, pl);
    for (std::size_t n = pl; n > 0; n >>= 1U) {
        if ((n & 1U) != 0) {
            a_ctx.update(std::span<const std::uint8_t>(b.data(), b.size()));
        } else {
            a_ctx.update(password);
        }
    }
    Sha256Digest c = a_ctx.finish();
    // P' and S'
    Sha256 dp_ctx;
    for (std::size_t i = 0; i < pl; ++i) {
        dp_ctx.update(password);
    }
    const Sha256Digest dp = dp_ctx.finish();
    std::string p_bytes;
    p_bytes.reserve(pl);
    for (std::size_t i = 0; i < pl; ++i) {
        p_bytes += static_cast<char>(dp[i % dp.size()]);
    }
    Sha256 ds_ctx;
    for (std::size_t i = 0; i < 16U + c[0]; ++i) {
        ds_ctx.update(salt);
    }
    const Sha256Digest ds = ds_ctx.finish();
    std::string s_bytes;
    for (std::size_t i = 0; i < sl; ++i) {
        s_bytes += static_cast<char>(ds[i % ds.size()]);
    }
    const auto c_span = [&c] { return std::span<const std::uint8_t>(c.data(), c.size()); };
    for (unsigned i = 0; i < r; ++i) {
        Sha256 ctx;
        if ((i & 1U) != 0) {
            ctx.update(p_bytes);
        } else {
            ctx.update(c_span());
        }
        if (i % 3 != 0) {
            ctx.update(s_bytes);
        }
        if (i % 7 != 0) {
            ctx.update(p_bytes);
        }
        if ((i & 1U) != 0) {
            ctx.update(c_span());
        } else {
            ctx.update(p_bytes);
        }
        c = ctx.finish();
    }
    std::string out = "$5$";
    if (rounds != 5000) {
        out += "rounds=" + std::to_string(r) + "$";
    }
    out += salt;
    out += '$';
    const auto b64 = [&out](unsigned b2, unsigned b1, unsigned b0, int n) {
        unsigned w = (b2 << 16U) | (b1 << 8U) | b0;
        for (int i = 0; i < n; ++i) {
            out += kCryptAlphabet[w & 0x3FU];
            w >>= 6U;
        }
    };
    static constexpr std::array<std::array<int, 3>, 10> kOrder = {{{0, 10, 20}, {21, 1, 11}, {12, 22, 2}, {3, 13, 23},
                                                                   {24, 4, 14}, {15, 25, 5}, {6, 16, 26}, {27, 7, 17},
                                                                   {18, 28, 8}, {9, 19, 29}}};
    for (const auto& [x, y, z] : kOrder) {
        b64(c[static_cast<std::size_t>(x)], c[static_cast<std::size_t>(y)], c[static_cast<std::size_t>(z)], 4);
    }
    b64(0, c[31], c[30], 3);
    return out;
}

std::optional<std::string> mailpit_auth_line(std::string_view password, std::string& error) {
    // SHA-256-crypt, which Mailpit's htpasswd reader (tg123/go-htpasswd)
    // takes, made here from FloServer's own SHA-256: bcrypt would need a
    // Blowfish of our own. The password is 32 random bytes, so no guessing
    // gets near it however fast the hash; the fewest rounds keep each of
    // the inbox's requests cheap.
    std::array<std::uint8_t, 16> random{};
    if (!random_bytes(random, error)) {
        return std::nullopt;
    }
    std::string salt;
    for (const std::uint8_t byte : random) {
        salt += kCryptAlphabet[byte % kCryptAlphabet.size()];
    }
    return std::string(kMailUiUser) + ":" + sha256_crypt(password, salt, kMailpitRounds) + "\n";
}

std::string mailpit_basic_auth(std::string_view password) {
    const std::string pair = std::string(kMailUiUser) + ":" + std::string(password);
    return "Basic " + to_base64(bytes_of(pair));
}

std::string mailpit_nginx_auth(std::string_view password) {
    return "# Written by FloServer: the inbox's password, added to every request for mail.localhost.\n"
           "# This computer's own; don't share this file.\n"
           "proxy_set_header Authorization \"" +
           mailpit_basic_auth(password) + "\";\n";
}

std::optional<std::string> mail_sendmail_problem(const fs::path& mailpit) {
#ifdef _WIN32
    (void)mailpit;
    return std::nullopt;
#else
    const std::string program = path_to_utf8(mailpit);
    const bool bad = program.empty() || std::any_of(program.begin(), program.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F || c == '\'' || c == '$' || c == '"' || c == '`' || c == '\\';
    });
    if (!bad) {
        return std::nullopt;
    }
    return "PHP's mail() can't use Mailpit: its path (" + clean_error_text(program) +
           ") has a ' $ \" ` \\ or control character, which PHP's sendmail_path can't hold safely";
#endif
}

std::vector<std::pair<std::string, std::string>> mail_php_ini_lines(Port port, const fs::path& mailpit) {
#ifdef _WIN32
    (void)mailpit;
    return {{"SMTP", "127.0.0.1"}, {"smtp_port", std::to_string(port)}, {"sendmail_from", std::string(kMailFrom)}};
#else
    if (mail_sendmail_problem(mailpit)) {
        return {};
    }
    // PHP runs sendmail_path with /bin/sh -c: the program in '...' (a path
    // with a space, as every macOS data folder has), the whole value in
    // php.ini's "..." (which expands ${...}, so no $ gets this far).
    return {{"sendmail_path",
             "\"'" + path_to_utf8(mailpit) + "' sendmail -S 127.0.0.1:" + std::to_string(port) + "\""}};
#endif
}

std::optional<std::string> mail_php_problem(const AppConfig& app, const std::vector<InstalledRuntime>& installed) {
    const MailSettings settings = read_mail_settings(app);
    const InstalledRuntime* mailpit = newest_mailpit(installed);
    if (!settings.php || mailpit == nullptr) {
        return std::nullopt;
    }
    const auto program = mailpit_program(mailpit->dir);
    return program ? mail_sendmail_problem(*program) : std::nullopt;
}

std::vector<std::pair<std::string, std::string>> mail_php_ini(const AppConfig& app,
                                                              const std::vector<InstalledRuntime>& installed) {
    const MailSettings settings = read_mail_settings(app);
    const InstalledRuntime* mailpit = newest_mailpit(installed);
    if (!settings.php || !settings.port || mailpit == nullptr) {
        return {};
    }
    const auto program = mailpit_program(mailpit->dir);
#ifdef _WIN32
    return mail_php_ini_lines(*settings.port, program.value_or(fs::path()));
#else
    return program ? mail_php_ini_lines(*settings.port, *program) : std::vector<std::pair<std::string, std::string>>{};
#endif
}

namespace {

// One code point from `text` at `at` (advanced); U+FFFD for a bad sequence.
char32_t next_code_point(std::string_view text, std::size_t& at) {
    const auto byte = [&text](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char first = byte(at);
    std::size_t length = 1;
    char32_t cp = first;
    if (first >= 0xC2 && first <= 0xDF) {
        length = 2;
        cp = first & 0x1FU;
    } else if (first >= 0xE0 && first <= 0xEF) {
        length = 3;
        cp = first & 0x0FU;
    } else if (first >= 0xF0 && first <= 0xF4) {
        length = 4;
        cp = first & 0x07U;
    } else if (first >= 0x80) {
        ++at;
        return 0xFFFD;
    }
    if (at + length > text.size()) {
        ++at;
        return 0xFFFD;
    }
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(at + i) & 0xC0U) != 0x80U) {
            ++at;
            return 0xFFFD;
        }
        cp = (cp << 6U) | (byte(at + i) & 0x3FU);
    }
    // Overlong, surrogate or out of range: not UTF-8.
    if ((length == 3 && cp < 0x800) || (length == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++at;
        return 0xFFFD;
    }
    at += length;
    return cp;
}

void append_utf8(std::string& out, char32_t cp) {
    const auto put = [&out](unsigned value) { out += static_cast<char>(static_cast<unsigned char>(value)); };
    if (cp < 0x80) {
        put(cp);
    } else if (cp < 0x800) {
        put(0xC0U | (cp >> 6U));
        put(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000) {
        put(0xE0U | (cp >> 12U));
        put(0x80U | ((cp >> 6U) & 0x3FU));
        put(0x80U | (cp & 0x3FU));
    } else {
        put(0xF0U | (cp >> 18U));
        put(0x80U | ((cp >> 12U) & 0x3FU));
        put(0x80U | ((cp >> 6U) & 0x3FU));
        put(0x80U | (cp & 0x3FU));
    }
}

// Bidi controls, zero-width and other invisible characters (soft hyphen,
// Mongolian vowel separator, invisible operators, tags, variation
// selectors), and line/paragraph separators.
bool invisible(char32_t cp) {
    return cp == 0x00AD || cp == 0x061C || cp == 0x180E || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x2069) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || (cp >= 0xE0000 && cp <= 0xE007F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

bool control(char32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F); }

std::string clean_text(std::string_view text, std::size_t max_chars) {
    std::string out;
    std::size_t chars = 0;
    for (std::size_t at = 0; at < text.size();) {
        char32_t cp = next_code_point(text, at);
        if (invisible(cp)) {
            continue;
        }
        if (control(cp)) {
            cp = U' ';
        }
        if (chars == max_chars) {
            out += "\u2026";  // …
            break;
        }
        append_utf8(out, cp);
        ++chars;
    }
    return out;
}

}  // namespace

std::string clean_mail_text(std::string_view text, std::size_t max_chars) { return clean_text(text, max_chars); }

std::string clean_error_text(std::string_view text, std::size_t max_chars) { return clean_text(text, max_chars); }

MailEnvKind mail_env_kind(const fs::path& project_dir, bool python) {
    if (env_style(project_dir) == EnvStyle::Laravel) {
        return MailEnvKind::Laravel;
    }
    if (is_flocms_project(project_dir)) {
        return MailEnvKind::None;
    }
    return python ? MailEnvKind::Python : MailEnvKind::Other;
}

std::vector<std::pair<std::string, std::string>> mail_env_values(MailEnvKind kind, Port port) {
    const std::string p = std::to_string(port);
    const std::string url = "smtp://127.0.0.1:" + p;
    switch (kind) {
        case MailEnvKind::None:
            return {};
        case MailEnvKind::Laravel:
            return {{"MAIL_MAILER", "smtp"}, {"MAIL_HOST", "127.0.0.1"}, {"MAIL_PORT", p}};
        case MailEnvKind::Python:
            return {{"EMAIL_HOST", "127.0.0.1"}, {"EMAIL_PORT", p}, {"SMTP_URL", url}};
        case MailEnvKind::Other:
            break;
    }
    return {{"MAIL_HOST", "127.0.0.1"}, {"MAIL_PORT", p}, {"SMTP_URL", url}};
}

}  // namespace floserver::core
