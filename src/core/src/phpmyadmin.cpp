#include "floserver/core/phpmyadmin.h"

#include "floserver/core/crypto.h"
#include "floserver/core/database_service.h"
#include "floserver/core/mariadb_settings.h"
#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// A PHP single-quoted string, or nothing for text with a control character.
std::optional<std::string> php_quoted(std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            return std::nullopt;
        }
        if (c == '\\' || c == '\'') {
            out += '\\';
        }
        out += c;
    }
    out += '\'';
    return out;
}

// A path as PHP reads it on every platform: forward slashes.
std::optional<std::string> php_path(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    std::string text(u.begin(), u.end());
    std::replace(text.begin(), text.end(), '\\', '/');
    if (text.empty()) {
        return std::nullopt;
    }
    return php_quoted(text);
}

bool is_series(std::string_view s) {
    return !s.empty() && s.size() <= 16 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || c == '.';
    }) && s.front() != '.' && s.back() != '.';
}

void make_private_dir(const fs::path& dir, std::error_code& ec) {
    fs::create_directories(dir, ec);
#ifndef _WIN32
    if (!ec) {
        ::chmod(dir.c_str(), 0700);
    }
#endif
}

}  // namespace

bool reserved_project_name(std::string_view name) { return name == kPhpMyAdminHostName; }

PhpMyAdminPaths phpmyadmin_paths(const fs::path& data) {
    PhpMyAdminPaths p;
    p.dir = data / "phpmyadmin";
    p.config = p.dir / "config.inc.php";
    p.signon_script = p.dir / path_from_utf8(kSignonScriptName);
    p.tokens = p.dir / "tokens";
    p.sessions = p.dir / "sessions";
    p.temp = p.dir / "tmp";
    return p;
}

bool ensure_phpmyadmin_dirs(const PhpMyAdminPaths& paths, std::string& error) {
    std::error_code ec;
    make_private_dir(paths.dir, ec);
    if (!ec) {
        make_private_dir(paths.tokens, ec);
    }
    if (!ec) {
        make_private_dir(paths.sessions, ec);
    }
    if (!ec) {
        make_private_dir(paths.temp, ec);
    }
    if (ec) {
        error = "can't make phpMyAdmin's folders in " + path_to_utf8(paths.dir) + ": " + error_text(ec);
        return false;
    }
    return true;
}

std::string phpmyadmin_lang(std::string_view code) {
    std::string c(code.substr(0, code.find_first_of("-_")));
    std::transform(c.begin(), c.end(), c.begin(),
                   [](char ch) { return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : ch; });
    if (c == "ar") {
        return "ar";
    }
    if (c == "ku" || c == "ckb") {
        return "ckb";
    }
    return "en";
}

std::string signon_session_name(std::string_view series) {
    std::string name = "FloServerSignon_";
    for (const char c : series) {
        name += (c >= '0' && c <= '9') ? c : '_';
    }
    return name;
}

std::vector<PhpMyAdminServer> phpmyadmin_servers(std::vector<PhpMyAdminServer> servers) {
    std::erase_if(servers, [](const PhpMyAdminServer& s) { return !is_series(s.series) || s.port == 0; });
    std::stable_sort(servers.begin(), servers.end(), [](const PhpMyAdminServer& a, const PhpMyAdminServer& b) {
        const auto va = Version::parse(a.series);
        const auto vb = Version::parse(b.series);
        if (va && vb && *va != *vb) {
            return *va < *vb;
        }
        return a.series < b.series;
    });
    servers.erase(std::unique(servers.begin(), servers.end(),
                              [](const PhpMyAdminServer& a, const PhpMyAdminServer& b) { return a.series == b.series; }),
                  servers.end());
    return servers;
}

std::optional<std::string> phpmyadmin_config(const PhpMyAdminConfigSpec& spec, std::string& error) {
    if (spec.blowfish_hex.size() != kSignonTokenBytes * 2 || !from_hex(spec.blowfish_hex)) {
        error = "phpMyAdmin's secret isn't 32 bytes";
        return std::nullopt;
    }
    const auto temp = php_path(spec.paths.temp);
    const auto sessions = php_path(spec.paths.sessions);
    if (!temp || !sessions) {
        error = "phpMyAdmin's folder path can't be written into its config";
        return std::nullopt;
    }
    const std::string lang = phpmyadmin_lang(spec.lang);
    std::string out =
        "<?php\n"
        "// Written by FloServer whenever its settings change, so edits here are lost.\n"
        "// The secret below is this computer's own; don't share this file.\n"
        "declare(strict_types=1);\n\n"
        "$floserver_https = !empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off';\n"
        "$floserver_cookie = ['lifetime' => 0, 'path' => '/', 'domain' => '', 'secure' => $floserver_https,\n"
        "                     'httponly' => true, 'samesite' => 'Lax'];\n\n"
        "$cfg['blowfish_secret'] = hex2bin('" + spec.blowfish_hex + "');\n"
        "$cfg['TempDir'] = " + *temp + ";\n"
        "$cfg['SessionSavePath'] = " + *sessions + ";\n"
        "$cfg['DefaultLang'] = '" + lang + "';\n"
        "$cfg['VersionCheck'] = false;\n"
        "$cfg['SendErrorReports'] = 'never';\n"
        // Its own tables for bookmarks and history aren't set up: no banner about them.
        "$cfg['PmaNoRelation_DisableWarning'] = true;\n"
        "$cfg['AllowArbitraryServer'] = false;\n"
        "$cfg['ServerDefault'] = 1;\n\n"
        "$i = 0;\n";
    for (const PhpMyAdminServer& s : phpmyadmin_servers(spec.servers)) {
        const std::string session = signon_session_name(s.series);
        out.append("\n// MariaDB ").append(s.series);
        out.append(": FloServer's one-click login (signon), or phpMyAdmin's own login form\n"
                   "// when it wasn't opened from FloServer.\n"
                   "$i++;\n"
                   "$cfg['Servers'][$i]['verbose'] = 'MariaDB ");
        out.append(s.series).append("';\n$cfg['Servers'][$i]['host'] = '127.0.0.1';\n$cfg['Servers'][$i]['port'] = '");
        out.append(std::to_string(s.port));
        out.append("';\n"
                   "$cfg['Servers'][$i]['compress'] = false;\n"
                   "$cfg['Servers'][$i]['AllowNoPassword'] = false;\n"
                   "$cfg['Servers'][$i]['auth_type'] = isset($_COOKIE['");
        out.append(session).append("']) ? 'signon' : 'cookie';\n$cfg['Servers'][$i]['SignonSession'] = '");
        out.append(session).append("';\n$cfg['Servers'][$i]['SignonCookieParams'] = $floserver_cookie;\n");
        out.append("$cfg['Servers'][$i]['SignonURL'] = './").append(kSignonScriptName).append("';\n");
        out.append("$cfg['Servers'][$i]['LogoutURL'] = './").append(kSignonScriptName).append("?logout=1';\n");
    }
    return out;
}

std::optional<std::string> phpmyadmin_config_lang(std::string_view text) {
    constexpr std::string_view kKey = "$cfg['DefaultLang'] = '";
    const auto at = text.find(kKey);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = at + kKey.size();
    const auto end = text.find('\'', start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return phpmyadmin_lang(text.substr(start, end - start));
}

std::optional<std::string> phpmyadmin_config_stub(const fs::path& config) {
    const auto path = php_path(config);
    if (!path) {
        return std::nullopt;
    }
    return "<?php\n"
           "// Written by FloServer: phpMyAdmin's settings are kept outside this folder,\n"
           "// which an update replaces. Change them in FloServer.\n"
           "require " + *path + ";\n";
}

std::optional<std::string> phpmyadmin_signon_script(const PhpMyAdminPaths& paths) {
    const auto tokens = php_path(paths.tokens);
    const auto sessions = php_path(paths.sessions);
    if (!tokens || !sessions) {
        return std::nullopt;
    }
    std::string out = "<?php\n"
                      "// FloServer's one-click login for phpMyAdmin. Written by FloServer; edits here are lost.\n"
                      "declare(strict_types=1);\n\n"
                      "const FLOSERVER_TOKENS = " + *tokens + ";\n"
                      "const FLOSERVER_SESSIONS = " + *sessions + ";\n";
    out += R"PHP(
header('Cache-Control: no-store');
header('Referrer-Policy: no-referrer');
header('X-Frame-Options: DENY');

function floserver_refuse(string $message): void
{
    http_response_code(403);
    header('Content-Type: text/html; charset=utf-8');
    echo '<!DOCTYPE html><html><head><meta charset="utf-8"><title>phpMyAdmin</title></head>',
        '<body style="font-family: system-ui, sans-serif; margin: 3em; max-width: 40em">',
        '<p>', htmlspecialchars($message), '</p>',
        '<p><a href="./', basename(__FILE__), '?logout=1">Sign in with a MariaDB user name and password</a></p>',
        '</body></html>';
    exit;
}

$https = !empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off';
$cookie = ['lifetime' => 0, 'path' => '/', 'domain' => '', 'secure' => $https, 'httponly' => true,
           'samesite' => 'Lax'];
ini_set('session.use_strict_mode', '1');
ini_set('session.use_only_cookies', '1');
ini_set('session.use_trans_sid', '0');
session_save_path(FLOSERVER_SESSIONS);

// Sign out: forget every FloServer login, then phpMyAdmin shows its own form.
if (isset($_GET['logout'])) {
    foreach ($_COOKIE as $name => $value) {
        if (!is_string($name) || !preg_match('/^FloServerSignon_[0-9_]{1,32}$/D', $name)) {
            continue;
        }
        if (is_string($value) && preg_match('/^[A-Za-z0-9,-]{1,128}$/D', $value)) {
            session_name($name);
            session_id($value);
            session_set_cookie_params($cookie);
            if (@session_start()) {
                $_SESSION = [];
                session_destroy();
            }
        }
        setcookie($name, '', ['expires' => 1, 'path' => '/', 'secure' => $https, 'httponly' => true,
                              'samesite' => 'Lax']);
    }
    header('Location: ./index.php?route=/', true, 303);
    exit;
}

$expired = 'This link has expired or was already used. Open phpMyAdmin again from FloServer.';
$token = $_GET['token'] ?? null;
if (!is_string($token) || !preg_match('/^[0-9a-f]{64}$/D', $token)) {
    floserver_refuse($expired);
}

// Claim the token by renaming its file: only one request can, so it works once.
$file = FLOSERVER_TOKENS . '/' . $token . '.json';
$claimed = $file . '.claimed-' . bin2hex(random_bytes(8));
if (!@rename($file, $claimed)) {
    floserver_refuse($expired);
}
$text = @file_get_contents($claimed, false, null, 0, 65536);
@unlink($claimed);
$login = is_string($text) ? json_decode($text, true) : null;
if (
    !is_array($login)
    || !is_int($login['expires'] ?? null) || $login['expires'] < time()
    || !is_int($login['server'] ?? null) || $login['server'] < 1
    || !is_string($login['session'] ?? null) || !preg_match('/^FloServerSignon_[0-9_]{1,32}$/D', $login['session'])
    || !is_string($login['user'] ?? null) || $login['user'] === ''
    || !is_string($login['password'] ?? null)
    || !is_string($login['host'] ?? null)
    || !is_int($login['port'] ?? null)
    || (isset($login['only_db']) && !is_string($login['only_db']))
) {
    floserver_refuse($expired);
}

session_name($login['session']);
session_set_cookie_params($cookie);
if (!session_start()) {
    floserver_refuse('phpMyAdmin\'s sign-in session can\'t be started.');
}
session_regenerate_id(true);
$_SESSION = [];
$_SESSION['PMA_single_signon_user'] = $login['user'];
$_SESSION['PMA_single_signon_password'] = $login['password'];
$_SESSION['PMA_single_signon_host'] = $login['host'];
$_SESSION['PMA_single_signon_port'] = (string) $login['port'];
$_SESSION['PMA_single_signon_cfgupdate'] = isset($login['only_db']) ? ['only_db' => $login['only_db']] : [];
$_SESSION['PMA_single_signon_HMAC_secret'] = bin2hex(random_bytes(16));
session_write_close();

$lang = $_GET['lang'] ?? '';
$query = 'route=/&server=' . $login['server'];
if (is_string($lang) && in_array($lang, ['en', 'ar', 'ckb'], true)) {
    $query .= '&lang=' . $lang;
}
header('Location: ./index.php?' . $query, true, 303);
)PHP";
    return out;
}

std::optional<std::string> make_blowfish_secret(std::string& error) {
    std::array<std::uint8_t, kSignonTokenBytes> bytes{};
    if (!random_bytes(bytes, error)) {
        return std::nullopt;
    }
    return to_hex(bytes);
}

bool is_signon_token(std::string_view token) {
    return token.size() == kSignonTokenBytes * 2 &&
           std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

void remove_expired_signon_tokens(const fs::path& tokens_dir) {
    const auto cutoff = fs::file_time_type::clock::now() - kSignonTokenLife;
    std::error_code ec;
    std::vector<fs::path> old;
    for (fs::directory_iterator it(tokens_dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code inner;
        const auto written = it->last_write_time(inner);
        if (!inner && written < cutoff) {
            old.push_back(it->path());
        }
    }
    for (const fs::path& p : old) {
        std::error_code ignored;
        fs::remove(p, ignored);
    }
}

std::optional<std::string> create_signon_token(const fs::path& tokens_dir, const SignonLogin& login,
                                               std::chrono::system_clock::time_point now, std::string& error) {
    if (login.user.empty() || login.port == 0 || login.server == 0 || !is_series(login.series)) {
        error = "the login for phpMyAdmin is incomplete";
        return std::nullopt;
    }
    remove_expired_signon_tokens(tokens_dir);
    std::error_code ec;
    make_private_dir(tokens_dir, ec);
    if (ec) {
        error = "can't make " + path_to_utf8(tokens_dir) + ": " + error_text(ec);
        return std::nullopt;
    }
    std::array<std::uint8_t, kSignonTokenBytes> bytes{};
    if (!random_bytes(bytes, error)) {
        return std::nullopt;
    }
    std::string token = to_hex(bytes);
    const auto expires = std::chrono::duration_cast<std::chrono::seconds>((now + kSignonTokenLife).time_since_epoch());
    json doc = {{"expires", expires.count()},
                {"server", login.server},
                {"session", signon_session_name(login.series)},
                {"user", login.user},
                {"password", login.password},
                {"host", login.host},
                {"port", login.port}};
    if (login.only_db) {
        doc["only_db"] = *login.only_db;
    }
    if (!write_private_file_atomically(tokens_dir / path_from_utf8(token + ".json"), doc.dump(-1, ' ', false, json::error_handler_t::replace), error)) {
        return std::nullopt;
    }
    return token;
}

PhpMyAdminConfigWriter::PhpMyAdminConfigWriter(fs::path data, SecretStore secrets, ServerSource servers)
    : data_(std::move(data)), secrets_(std::move(secrets)), servers_(std::move(servers)) {}

void PhpMyAdminConfigWriter::set_lang(std::string_view code) {
    const std::lock_guard lock(mutex_);
    lang_ = phpmyadmin_lang(code);
}

bool PhpMyAdminConfigWriter::update(std::string& error, std::vector<PhpMyAdminServer>* servers_out) {
    const std::vector<PhpMyAdminServer> servers = phpmyadmin_servers(servers_ ? servers_() : std::vector<PhpMyAdminServer>{});
    const std::lock_guard lock(mutex_);
    const PhpMyAdminPaths paths = phpmyadmin_paths(data_);
    if (!lang_) {
        // The language the file already has: the window sets it with each login.
        const auto text = read_small_file(paths.config, std::size_t{64} * 1024);
        lang_ = text ? phpmyadmin_config_lang(*text).value_or("en") : std::string("en");
    }
    if (secret_.empty()) {
        constexpr const char* kSecretName = "phpmyadmin/blowfish";
        std::string saved = secrets_.read(kSecretName).value_or(std::string());
        if (saved.size() != kSignonTokenBytes * 2 || !from_hex(saved)) {
            saved = make_blowfish_secret(error).value_or(std::string());
            if (saved.empty() || !secrets_.save(kSecretName, saved, error)) {
                return false;
            }
        }
        secret_ = std::move(saved);
    }
    PhpMyAdminConfigSpec spec;
    spec.blowfish_hex = secret_;
    spec.paths = paths;
    spec.servers = servers;
    spec.lang = lang_.value_or("en");
    const auto text = phpmyadmin_config(spec, error);
    if (!text) {
        return false;
    }
    if (servers_out != nullptr) {
        *servers_out = servers;
    }
    std::error_code ec;
    if (*text == written_ && fs::is_regular_file(paths.config, ec)) {
        return true;
    }
    if (!ensure_phpmyadmin_dirs(paths, error) || !write_private_file_atomically(paths.config, *text, error)) {
        return false;
    }
    written_ = *text;
    return true;
}

std::string signon_url(std::string_view address, std::string_view token, std::string_view lang) {
    std::string url(address);
    while (url.ends_with('/')) {
        url.pop_back();
    }
    return url + "/" + std::string(kSignonScriptName) + "?token=" + std::string(token) +
           "&lang=" + phpmyadmin_lang(lang);
}

std::optional<std::string> phpmyadmin_login(DatabaseService& databases, PhpMyAdminConfigWriter& writer,
                                            const fs::path& data, std::string_view address,
                                            const PhpMyAdminLoginRequest& request, ServiceRefusal& refusal) {
    if (!request.lang.empty()) {
        writer.set_lang(request.lang);
    }
    SignonLogin login;
    if (!request.project.empty()) {
        auto own = databases.project_login(request.project, refusal, true);  // asked for now: a failed start is tried again
        if (!own) {
            return std::nullopt;
        }
        // Its series: the server on its port.
        const json status = databases.status();
        if (status.contains("servers") && status.at("servers").is_array()) {
            for (const json& s : status.at("servers")) {
                if (s.is_object() && s.contains("port") && s.at("port").is_number_unsigned() &&
                    s.at("port").get<std::uint64_t>() == own->port && s.contains("series") && s.at("series").is_string()) {
                    login.series = s.at("series").get<std::string>();
                }
            }
        }
        login.user = std::move(own->user);
        login.password = std::move(own->password);
        login.port = own->port;
        login.only_db = std::move(own->database);
    } else {
        const auto series = mariadb_request_series(request.series);
        if (!series) {
            refusal = {"bad_params", "name a MariaDB series such as 11.4, or a project"};
            return std::nullopt;
        }
        auto root = databases.root_login(*series, refusal);
        if (!root) {
            return std::nullopt;
        }
        login.series = *series;
        login.user = std::move(root->user);
        login.password = std::move(root->password);
        login.port = root->port;
    }
    std::string error;
    std::vector<PhpMyAdminServer> listed;
    if (!writer.update(error, &listed)) {
        refusal = {"phpmyadmin_failed", error};
        return std::nullopt;
    }
    const auto index = std::find_if(listed.begin(), listed.end(), [&login](const PhpMyAdminServer& s) {
        return s.series == login.series && s.port == login.port;
    });
    if (login.series.empty() || index == listed.end()) {
        // Its server was replaced meanwhile (a new port, a newer release).
        refusal = {"database_off", "MariaDB " + (login.series.empty() ? std::string("for this project") : login.series) +
                                       " is moving to another port; try again in a few seconds"};
        return std::nullopt;
    }
    login.server = static_cast<std::size_t>(index - listed.begin()) + 1;
    const auto token = create_signon_token(phpmyadmin_paths(data).tokens, login, std::chrono::system_clock::now(), error);
    if (!token) {
        refusal = {"phpmyadmin_failed", error};
        return std::nullopt;
    }
    return signon_url(address, *token, request.lang);
}

}  // namespace floserver::core
