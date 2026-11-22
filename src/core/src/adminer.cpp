#include "floserver/core/adminer.h"

#include "floserver/core/crypto.h"
#include "floserver/core/database_engine.h"
#include "floserver/core/database_service.h"
#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/phpmyadmin.h"

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

void make_private_dir(const fs::path& dir, std::error_code& ec) {
    fs::create_directories(dir, ec);
#ifndef _WIN32
    if (!ec) {
        ::chmod(dir.c_str(), 0700);
    }
#endif
}

bool is_pg_series(std::string_view s) {
    return !s.empty() && s.size() <= 16 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || c == '.';
    }) && s.front() != '.' && s.back() != '.';
}

// Everything after the paths: the same for every install.
constexpr std::string_view kIndexBody = R"PHP(
session_save_path(FLOSERVER_SESSIONS);
ini_set('session.use_strict_mode', '1');
ini_set('session.use_only_cookies', '1');
ini_set('session.use_trans_sid', '0');

function floserver_refuse(string $why): void
{
    http_response_code(403);
    header('Content-Type: text/html; charset=utf-8');
    header('Cache-Control: no-store');
    echo '<!DOCTYPE html><meta charset="utf-8"><title>Adminer</title>',
        '<p style="font: 16px/1.5 system-ui, sans-serif; margin: 40px">', htmlspecialchars($why, ENT_QUOTES), '</p>';
    exit;
}

// FloServer's PostgreSQL servers: the only ones Adminer may sign in to.
$floserver_servers = [];
$text = @file_get_contents(FLOSERVER_SERVERS, false, null, 0, 65536);
$list = is_string($text) ? json_decode($text, true) : null;
if (is_array($list)) {
    foreach ($list as $server) {
        if (is_string($server) && preg_match('/^127\.0\.0\.1:[0-9]{1,5}$/D', $server)) {
            $floserver_servers[] = $server;
        }
    }
}
$open_again = 'Open Adminer again from FloServer.';

// PostgreSQL only: MariaDB and MySQL have phpMyAdmin.
foreach (['server', 'sqlite', 'sqlite2', 'oracle', 'mssql', 'mongo', 'elastic', 'firebird', 'clickhouse', 'simpledb', 'imap'] as $driver) {
    if (isset($_GET[$driver])) {
        floserver_refuse('In FloServer, Adminer opens PostgreSQL databases. Use phpMyAdmin for MariaDB and MySQL.');
    }
}
if (isset($_GET['username']) && !isset($_GET['pgsql'])) {
    floserver_refuse('In FloServer, Adminer opens PostgreSQL databases. Use phpMyAdmin for MariaDB and MySQL.');
}
if (isset($_GET['pgsql']) && !in_array($_GET['pgsql'], $floserver_servers, true)) {
    floserver_refuse('That isn\'t one of FloServer\'s PostgreSQL servers. ' . $open_again);
}
if (isset($_POST['auth'])) {
    $auth = $_POST['auth'];
    if (!is_array($auth) || ($auth['driver'] ?? null) !== 'pgsql' || !in_array($auth['server'] ?? null, $floserver_servers, true)) {
        floserver_refuse('Adminer signs in to FloServer\'s PostgreSQL servers only. ' . $open_again);
    }
    // No "remember me": a kept login would skip FloServer's list of servers.
    $_POST['auth']['permanent'] = '';
}
// Adminer's "remember me" cookie signs in without a form, so it isn't used.
if (isset($_COOKIE['adminer_permanent'])) {
    unset($_COOKIE['adminer_permanent']);
    setcookie('adminer_permanent', '', ['expires' => 1, 'path' => '/']);
}
$floserver_one_click = false;

// One click from FloServer: claim the token by renaming its file, so it works once.
if (isset($_GET['floserver_token'])) {
    $expired = 'This link has expired or was already used. ' . $open_again;
    $token = $_GET['floserver_token'];
    if (!is_string($token) || !preg_match('/^[0-9a-f]{64}$/D', $token)) {
        floserver_refuse($expired);
    }
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
        || !is_string($login['user'] ?? null) || $login['user'] === ''
        || !is_string($login['password'] ?? null)
        || !is_int($login['port'] ?? null)
        || (isset($login['db']) && !is_string($login['db']))
    ) {
        floserver_refuse($expired);
    }
    $server = '127.0.0.1:' . $login['port'];
    if (!in_array($server, $floserver_servers, true)) {
        floserver_refuse($expired);
    }
    $lang = $_GET['lang'] ?? '';
    if (is_string($lang) && in_array($lang, ['en', 'ar'], true)) {
        $https = ($_SERVER['HTTPS'] ?? '') !== '' && $_SERVER['HTTPS'] !== 'off';
        setcookie('adminer_lang', $lang, ['expires' => time() + 31536000, 'path' => '/', 'secure' => $https,
                                          'httponly' => true, 'samesite' => 'Lax']);
        $_COOKIE['adminer_lang'] = $lang;
    }
    // Adminer's own login form, filled in: it checks the login, keeps it in
    // its session and goes on to the database.
    $_SERVER['REQUEST_METHOD'] = 'POST';
    $_POST = ['auth' => ['driver' => 'pgsql', 'server' => $server, 'username' => $login['user'],
                         'password' => $login['password'], 'db' => $login['db'] ?? '', 'permanent' => '']];
    unset($_GET['floserver_token'], $_GET['lang']);
    // Adminer goes on to its own address: without the used token in it.
    $query = http_build_query($_GET, '', '&', PHP_QUERY_RFC3986);
    $_SERVER['REQUEST_URI'] = explode('?', (string) ($_SERVER['REQUEST_URI'] ?? '/'), 2)[0] . ($query === '' ? '' : '?' . $query);
    $_SERVER['QUERY_STRING'] = $query;
    // The token above stands in for the form's own token, which this login has none of.
    $floserver_one_click = true;
}

function adminer_object()
{
    return new class extends \Adminer\Adminer {
        public function name(): string
        {
            return 'Adminer';
        }

        public function verifyLoginToken(): bool
        {
            return $GLOBALS['floserver_one_click'] ? false : parent::verifyLoginToken();
        }
    };
}

require FLOSERVER_ADMINER;
)PHP";

}  // namespace

AdminerPaths adminer_paths(const fs::path& data) {
    AdminerPaths p;
    p.dir = data / "adminer";
    p.index = p.dir / "index.php";
    p.servers = p.dir / "servers.json";
    p.tokens = p.dir / "tokens";
    p.sessions = p.dir / "sessions";
    return p;
}

bool ensure_adminer_dirs(const AdminerPaths& paths, std::string& error) {
    std::error_code ec;
    fs::create_directories(paths.dir, ec);
    if (!ec) {
        make_private_dir(paths.tokens, ec);
    }
    if (!ec) {
        make_private_dir(paths.sessions, ec);
    }
    if (ec) {
        error = "can't make " + path_to_utf8(paths.dir) + ": " + error_text(ec);
        return false;
    }
    return true;
}

fs::path adminer_file(const fs::path& install, std::string_view version) {
    return install / path_from_utf8("adminer-" + std::string(version) + ".php");
}

std::optional<std::string> adminer_index_script(const AdminerPaths& paths, const fs::path& adminer_php) {
    const auto adminer = php_path(adminer_php);
    const auto servers = php_path(paths.servers);
    const auto tokens = php_path(paths.tokens);
    const auto sessions = php_path(paths.sessions);
    if (!adminer || !servers || !tokens || !sessions) {
        return std::nullopt;
    }
    std::string out = "<?php\n"
                      "// FloServer's Adminer (PostgreSQL only). Written by FloServer; edits here are lost.\n"
                      "declare(strict_types=1);\n\n"
                      "const FLOSERVER_ADMINER = " + *adminer + ";\n"
                      "const FLOSERVER_SERVERS = " + *servers + ";\n"
                      "const FLOSERVER_TOKENS = " + *tokens + ";\n"
                      "const FLOSERVER_SESSIONS = " + *sessions + ";\n";
    out += kIndexBody;
    return out;
}

std::string adminer_servers_json(std::vector<Port> ports) {
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    json list = json::array();
    for (const Port p : ports) {
        if (p != 0) {
            list.push_back("127.0.0.1:" + std::to_string(p));
        }
    }
    return list.dump() + "\n";
}

std::string adminer_lang(std::string_view code) { return code == "ar" ? "ar" : "en"; }

std::optional<std::string> create_adminer_token(const fs::path& tokens_dir, const AdminerLogin& login,
                                                std::chrono::system_clock::time_point now, std::string& error) {
    if (login.user.empty() || login.port == 0) {
        error = "the login for Adminer is incomplete";
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
    json doc = {{"expires", expires.count()}, {"user", login.user}, {"password", login.password}, {"port", login.port}};
    if (login.database) {
        doc["db"] = *login.database;
    }
    if (!write_private_file_atomically(tokens_dir / path_from_utf8(token + ".json"),
                                       doc.dump(-1, ' ', false, json::error_handler_t::replace), error)) {
        return std::nullopt;
    }
    return token;
}

std::string adminer_url(std::string_view address, std::string_view token, std::string_view lang) {
    std::string url(address);
    while (url.ends_with('/')) {
        url.pop_back();
    }
    return url + "/?floserver_token=" + std::string(token) + "&lang=" + adminer_lang(lang);
}

std::string adminer_not_postgresql_message() {
    return "In FloServer, Adminer opens PostgreSQL databases. Open this project's database in phpMyAdmin instead";
}

std::optional<std::string> adminer_login(DatabaseService& databases, const fs::path& data, std::string_view address,
                                         const AdminerLoginRequest& request, ServiceRefusal& refusal) {
    AdminerLogin login;
    if (!request.project.empty()) {
        auto own = databases.project_login(request.project, refusal, true);  // asked for now: a failed start is tried again
        if (!own) {
            return std::nullopt;
        }
        if (own->family != kPostgreSqlFamily) {
            refusal = {"not_postgresql", adminer_not_postgresql_message()};
            return std::nullopt;
        }
        login.user = std::move(own->user);
        login.password = std::move(own->password);
        login.port = own->port;
        login.database = std::move(own->database);
    } else {
        if (!is_pg_series(request.series)) {
            refusal = {"bad_params", "name a PostgreSQL series such as 18, or a project"};
            return std::nullopt;
        }
        auto root = databases.root_login(request.series, refusal, kPostgreSqlFamily);
        if (!root) {
            return std::nullopt;
        }
        login.user = std::move(root->user);
        login.password = std::move(root->password);
        login.port = root->port;
    }
    // servers.json: every PostgreSQL server FloServer has now, and this one.
    std::vector<Port> ports{login.port};
    const json status = databases.status();
    if (status.contains("servers") && status.at("servers").is_array()) {
        for (const json& s : status.at("servers")) {
            if (s.is_object() && s.value("family", std::string()) == kPostgreSqlFamily && s.contains("port") &&
                s.at("port").is_number_unsigned()) {
                const auto p = s.at("port").get<std::uint64_t>();
                if (p > 0 && p <= 65535) {
                    ports.push_back(static_cast<Port>(p));
                }
            }
        }
    }
    const AdminerPaths paths = adminer_paths(data);
    std::string error;
    if (!ensure_adminer_dirs(paths, error) || !write_file_atomically(paths.servers, adminer_servers_json(ports), error)) {
        refusal = {"io_error", error};
        return std::nullopt;
    }
    const auto token = create_adminer_token(paths.tokens, login, std::chrono::system_clock::now(), error);
    if (!token) {
        refusal = {"io_error", error};
        return std::nullopt;
    }
    return adminer_url(address, *token, request.lang);
}

}  // namespace floserver::core
