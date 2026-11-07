// Mailpit (D27) without running it: SHA-256-crypt, the password file and
// header, the arguments, config.json's "mail", php.ini's and .env's mail
// lines, the reserved name, nginx's mail.localhost, the SMTP port's choice
// and the clean-up leaving the emails alone.

#include "floserver/core/data_folder.h"
#include "floserver/core/mail_service.h"
#include "floserver/core/mailpit.h"
#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/php_pool.h"
#include "floserver/core/phpmyadmin.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/site_plan.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace floserver::core;
namespace fs = std::filesystem;

namespace {

void fake_mailpit(const fs::path& data, const std::string& version) {
    const fs::path dir = runtimes_dir(data) / "mailpit" / version;
    fs::create_directories(dir);
    test_util::write_file(dir / ".floserver-runtime.json",
                          nlohmann::json{{"schema", 1}, {"name", "mailpit"}, {"version", version},
                                         {"platform", std::string(current_platform())}, {"variant", ""},
                                         {"license", "MIT"}, {"sha256", std::string(64, 'a')}}
                              .dump());
#ifdef _WIN32
    test_util::write_file(dir / "mailpit.exe", "MZ");
#else
    test_util::write_file(dir / "mailpit", "#!/bin/sh\n");
#endif
}

std::string value_of(const std::vector<std::pair<std::string, std::string>>& lines, const std::string& key) {
    for (const auto& [k, v] : lines) {
        if (k == key) {
            return v;
        }
    }
    return "(none)";
}

}  // namespace

TEST_CASE("SHA-256-crypt matches Drepper's vectors and OpenSSL") {
    CHECK(sha256_crypt("Hello world!", "saltstring") == "$5$saltstring$5B8vYYiY.CVt1RlTTf8KbXBH3hsxY/GNooZaBBGWEc5");
    CHECK(sha256_crypt("Hello world!", "saltstringsaltstring", 10000) ==
          "$5$rounds=10000$saltstringsaltst$3xv.VbSHBb41AL9AvLeujZkZRBAwqFMz2.opqey6IcA");
    CHECK(sha256_crypt("we have a short salt string but not a short password", "short", 77777) ==
          "$5$rounds=77777$short$JiO1O3ZpDAxGJeaDIuqCoEFysAe1mZNJRs3pw0KQRd/");
    CHECK(sha256_crypt("a very much longer text to encrypt.  This one even stretches over morethan one line.",
                       "anotherlongsaltstring", 1400) ==
          "$5$rounds=1400$anotherlongsalts$Rx.j8H.h8HjEDGomFU8bDkXm3XIUnzyxf12oP84Bnq1");
    // openssl passwd -5 -salt 'rounds=1000$abcdefgh' secretpw; Mailpit 1.33 accepts it.
    CHECK(sha256_crypt("secretpw", "abcdefgh", 1000) == "$5$rounds=1000$abcdefgh$IteDL9K6eQMZ.k7Gwn5hHKmh3nfQdKCtYhQhIpKtjd0");
}

TEST_CASE("the inbox's password file holds a salted hash only, and nginx's header the Basic login") {
    const std::string password(64, 'f');
    std::string error;
    const auto line = mailpit_auth_line(password, error);
    REQUIRE(line);
    CHECK(line->starts_with("floserver:$5$rounds=1000$"));
    CHECK(line->ends_with("\n"));
    CHECK(line->find(password) == std::string::npos);
    // The salt is random: two lines differ, both check out.
    const auto again = mailpit_auth_line(password, error);
    REQUIRE(again);
    CHECK(*line != *again);
    const std::string hash = line->substr(10, line->size() - 11);
    const std::string salt = hash.substr(15, 16);
    CHECK(sha256_crypt(password, salt, 1000) == hash);

    CHECK(mailpit_basic_auth("pw") == "Basic " + to_base64(std::vector<std::uint8_t>{'f', 'l', 'o', 's', 'e', 'r', 'v', 'e',
                                                                                      'r', ':', 'p', 'w'}));
    const std::string conf = mailpit_nginx_auth("pw");
    CHECK(conf.find("proxy_set_header Authorization \"" + mailpit_basic_auth("pw") + "\";") != std::string::npos);
}

TEST_CASE("Mailpit's arguments: its two sockets (never TCP), its database, no secret, no relay") {
    const MailpitPaths paths = mailpit_paths(path_from_utf8("/data"));
    const auto args = mailpit_args(paths, 500);
    const auto has = [&args](const std::string& a) { return std::find(args.begin(), args.end(), a) != args.end(); };
    const auto after = [&args](const std::string& a) {
        const auto it = std::find(args.begin(), args.end(), a);
        return it != args.end() && it + 1 != args.end() ? *(it + 1) : std::string();
    };
    CHECK(after("--smtp") == "unix:/data/mailpit/smtp.sock:0600");
    CHECK(after("--listen") == "unix:/data/mailpit/http.sock:0600");
    for (const std::string& a : args) {
        CHECK(a.find("127.0.0.1") == std::string::npos);
    }
    CHECK_FALSE(mailpit_socket_problem(paths));
    // sun_path's limit: a longer folder is refused, never TCP instead.
    const MailpitPaths deep = mailpit_paths(path_from_utf8("/" + std::string(120, 'd')));
    REQUIRE(mailpit_socket_problem(deep));
    CHECK(mailpit_socket_problem(deep)->find("too long") != std::string::npos);
    CHECK(after("--database") == path_to_utf8(paths.database));
    CHECK(after("--max") == "500");
    CHECK(after("--ui-auth-file") == path_to_utf8(paths.ui_auth));
    CHECK(has("--smtp-auth-accept-any"));
    CHECK(has("--smtp-auth-allow-insecure"));
    CHECK(has("--disable-version-check"));
    for (const std::string& a : args) {
        CHECK(a.find("relay") == std::string::npos);
        CHECK(a.find("pop3") == std::string::npos);
        CHECK(a.find("webhook") == std::string::npos);
    }
    CHECK(paths.database.filename() == "mailpit.db");
    CHECK(paths.dir.filename() == "mailpit");
    const auto& env = mailpit_env_names();
    CHECK(std::find(env.begin(), env.end(), "MP_SMTP_RELAY_HOST") != env.end());
    CHECK(std::find(env.begin(), env.end(), "MP_UI_AUTH") != env.end());
}

TEST_CASE("no Mailpit older than 1.30.0 is offered or run") {
    CHECK_FALSE(runtime_version_allowed("mailpit", "1.29.9"));
    CHECK_FALSE(runtime_version_allowed("mailpit", "1.2"));
    CHECK_FALSE(runtime_version_allowed("mailpit", "nonsense"));
    CHECK(runtime_version_allowed("mailpit", "1.30.0"));
    CHECK(runtime_version_allowed("mailpit", "1.33.0"));
    CHECK(runtime_version_allowed("php", "5.6.0"));

    const test_util::TempDir tmp;
    fake_mailpit(tmp.path(), "1.29.0");
    auto installed = list_installed(runtimes_dir(tmp.path()));
    CHECK(newest_mailpit(installed) == nullptr);
    REQUIRE(newest_mailpit_any(installed) != nullptr);
    fake_mailpit(tmp.path(), "1.33.0");
    installed = list_installed(runtimes_dir(tmp.path()));
    REQUIRE(newest_mailpit(installed) != nullptr);
    CHECK(newest_mailpit(installed)->version == "1.33.0");
    CHECK(mailpit_program(newest_mailpit(installed)->dir));
}

TEST_CASE("config.json's \"mail\": defaults, values, bad ones, and other keys kept") {
    AppConfig app;
    MailSettings s = read_mail_settings(app);
    CHECK(s == MailSettings{});
    CHECK(s.php);
    CHECK(s.notices);
    CHECK_FALSE(s.keep_running);
    CHECK(s.max == 500);
    CHECK_FALSE(s.port);

    app.other["mail"] = {{"php", false}, {"max", 50}, {"port", 1026}, {"future", "kept"}};
    s = read_mail_settings(app);
    CHECK_FALSE(s.php);
    CHECK(s.max == 50);
    REQUIRE(s.port);
    CHECK(*s.port == 1026);
    s.keep_running = true;
    write_mail_settings(app, s);
    CHECK(app.other["mail"]["future"] == "kept");
    CHECK(app.other["mail"]["keep_running"] == true);
    CHECK(app.other["mail"]["port"] == 1026);
    CHECK(app.other["mail"]["notices"] == true);

    app.other["mail"] = {{"php", "yes"}, {"max", -1}, {"port", 70000}};
    std::vector<std::string> problems;
    s = read_mail_settings(app, &problems);
    CHECK(s.php);
    CHECK(s.max == 500);
    CHECK_FALSE(s.port);
    CHECK(problems.size() == 3);
}

TEST_CASE("PHP's mail() goes to Mailpit only when it's installed, the switch is on and the port chosen") {
    const test_util::TempDir tmp;
    AppConfig app;
    const std::vector<InstalledRuntime> none;
    CHECK(mail_php_ini(app, none).empty());

    fake_mailpit(tmp.path(), "1.33.0");
    const auto installed = list_installed(runtimes_dir(tmp.path()));
    CHECK(mail_php_ini(app, installed).empty());  // no port chosen yet
    MailSettings s;
    s.port = 1026;
    write_mail_settings(app, s);
    const auto lines = mail_php_ini(app, installed);
#ifdef _WIN32
    CHECK(value_of(lines, "SMTP") == "127.0.0.1");
    CHECK(value_of(lines, "smtp_port") == "1026");
    CHECK(value_of(lines, "sendmail_from") == "floserver@localhost");
#else
    const auto program = mailpit_program(installed.front().dir);
    REQUIRE(program);
    CHECK(value_of(lines, "sendmail_path") == "\"'" + path_to_utf8(*program) + "' sendmail -S 127.0.0.1:1026\"");
    CHECK_FALSE(mail_php_problem(app, installed));
    // A space is fine inside the shell's quotes (every macOS data folder has one).
    CHECK(value_of(mail_php_ini_lines(1025, "/Users/me/Library/Application Support/FloServer/mailpit"),
                   "sendmail_path") ==
          "\"'/Users/me/Library/Application Support/FloServer/mailpit' sendmail -S 127.0.0.1:1025\"");
    // What the shell or php.ini would read as more than a path is refused.
    for (const char* bad : {"/x/it's/mailpit", "/x/$HOME/mailpit", "/x/${A}/mailpit", "/x/`id`/mailpit",
                            "/x/\"q\"/mailpit", "/x/back\\slash/mailpit", "/x/new\nline/mailpit"}) {
        INFO(bad);
        CHECK(mail_php_ini_lines(1025, bad).empty());
        CHECK(mail_sendmail_problem(bad));
    }
#endif
    CHECK(mail_php_ini_lines(1025, "/x/mailpit").size() == (current_platform().starts_with("windows") ? 3U : 1U));

    s.php = false;
    write_mail_settings(app, s);
    CHECK(mail_php_ini(app, installed).empty());

    // In a pool's php.ini they come before the user's own lines, and change its key.
    PhpIniSettings ini;
    ini.series = "8.4";
    ini.custom_ini = "SMTP = mine.example\n";
    const std::string plain_key = php_settings_key(ini);
    ini.mail_ini = mail_php_ini_lines(1026, "/x/mailpit");
    CHECK(php_settings_key(ini) != plain_key);
    const std::string text = make_php_ini(tmp.path(), ini, tmp.path() / "e.log").text;
    const auto mail_at = text.find("Mailpit");
    const auto custom_at = text.find("SMTP = mine.example");
    REQUIRE(mail_at != std::string::npos);
    REQUIRE(custom_at != std::string::npos);
    CHECK(mail_at < custom_at);
    // A pool without Mailpit keeps the key it had before D27.
    PhpIniSettings bare;
    bare.series = "8.4";
    CHECK(php_settings_key(bare).empty());
}

TEST_CASE("the pools' php.ini get Mailpit's lines from the plan, and lose them when it's turned off") {
    const test_util::TempDir tmp;
    const fs::path& data = tmp.path();
    const fs::path php = runtimes_dir(data) / "php" / "8.4.1";
    fs::create_directories(php);
    test_util::write_file(php / ".floserver-runtime.json",
                          nlohmann::json{{"schema", 1}, {"name", "php"}, {"version", "8.4.1"},
                                         {"platform", std::string(current_platform())}, {"variant", "nts"},
                                         {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
    fake_mailpit(data, "1.33.0");
    const fs::path shop = data / "shop";
    test_util::write_file(shop / "index.php", "<?php mail('a@b.c', 'x', 'y');");
    AppConfig app;
    std::string error;
    REQUIRE(add_project(app, shop, "", error));
    app.projects.front().on = true;
    MailSettings s;
    s.port = 1025;
    write_mail_settings(app, s);
    const auto installed = list_installed(runtimes_dir(data));
    WebPlan plan = plan_sites(app, read_project_config_file, installed, std::nullopt);
    REQUIRE(plan.pools.size() == 1);
    CHECK_FALSE(plan.pools.front().settings.mail_ini.empty());
    const std::string with_mail = plan.pools.front().key;

    s.php = false;
    write_mail_settings(app, s);
    plan = plan_sites(app, read_project_config_file, installed, std::nullopt);
    REQUIRE(plan.pools.size() == 1);
    CHECK(plan.pools.front().settings.mail_ini.empty());
    CHECK(plan.pools.front().key != with_mail);  // a new key: the pool restarts with the new php.ini
}

TEST_CASE(".env mail lines: Laravel, Python, others, and none for FloCMS") {
    const auto laravel = mail_env_values(MailEnvKind::Laravel, 1026);
    CHECK(value_of(laravel, "MAIL_MAILER") == "smtp");
    CHECK(value_of(laravel, "MAIL_HOST") == "127.0.0.1");
    CHECK(value_of(laravel, "MAIL_PORT") == "1026");
    const auto python = mail_env_values(MailEnvKind::Python, 1026);
    CHECK(value_of(python, "EMAIL_HOST") == "127.0.0.1");
    CHECK(value_of(python, "EMAIL_PORT") == "1026");
    CHECK(value_of(python, "SMTP_URL") == "smtp://127.0.0.1:1026");
    const auto other = mail_env_values(MailEnvKind::Other, 1025);
    CHECK(value_of(other, "MAIL_HOST") == "127.0.0.1");
    CHECK(value_of(other, "MAIL_PORT") == "1025");
    CHECK(value_of(other, "SMTP_URL") == "smtp://127.0.0.1:1025");
    CHECK(mail_env_values(MailEnvKind::None, 1025).empty());

    const test_util::TempDir tmp;
    const fs::path l = tmp.path() / "laravel";
    test_util::write_file(l / ".env.example", "APP_NAME=x\nDB_CONNECTION=mysql\nDB_DATABASE=x\n");
    CHECK(mail_env_kind(l, false) == MailEnvKind::Laravel);
    const fs::path f = tmp.path() / "flocms";
    test_util::write_file(f / "composer.json", R"({"name": "me/site", "require": {"hostkurd/flocms-core": "^1.0"}})");
    CHECK(mail_env_kind(f, false) == MailEnvKind::None);
    const fs::path p = tmp.path() / "py";
    fs::create_directories(p);
    CHECK(mail_env_kind(p, true) == MailEnvKind::Python);
    CHECK(mail_env_kind(p, false) == MailEnvKind::Other);
}

TEST_CASE("text from an email is cleaned and cut short before it's shown") {
    CHECK(clean_mail_text("Hello") == "Hello");
    CHECK(clean_mail_text("a\x1b[31mred\x07\r\nb\t") == "a [31mred   b ");
    // C1 controls, bidi overrides, zero-width and separators.
    CHECK(clean_mail_text("x\xc2\x85y") == "x y");
    const std::string rlo = {'\xe2', '\x80', '\xae'};  // U+202E, as bytes (a literal trips clang-tidy)
    CHECK(clean_mail_text("pay" + rlo + "exe.pdf") == "payexe.pdf");
    CHECK(clean_mail_text("a\xe2\x80\x8b" "b\xe2\x80\xa8" "c\xef\xbb\xbf" "d\xd8\x9c" "e") == "abcde");  // 200B 2028 FEFF 061C
    CHECK(clean_mail_text("\xe2\x81\xa6x\xe2\x81\xa9\xe2\x81\xa0") == "x");                  // 2066 2069 2060
    // Arabic and Kurdish stay as they are.
    CHECK(clean_mail_text("\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85") == "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85");
    // Bad UTF-8 becomes U+FFFD; overlong forms aren't let through.
    CHECK(clean_mail_text("a\xff\xc0\xaf" "b") == "a\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd" "b");
    // Cut on a character, never inside one.
    std::string arabic;
    for (int i = 0; i < 300; ++i) {
        arabic += "\xd8\xb3";
    }
    const std::string cut = clean_mail_text(arabic);
    CHECK(cut.size() == 200 * 2 + 3);
    CHECK(cut.ends_with("\xe2\x80\xa6"));
    CHECK(clean_mail_text(std::string(500, 'a')).size() == 203);
    CHECK(clean_error_text("line one\nline two\x1b]0;title\x07") == "line one line two ]0;title ");
}

TEST_CASE("mail is kept for Mailpit's inbox: new projects can't take it, one that has it keeps it") {
    CHECK(reserved_project_name("mail"));
    CHECK(reserved_project_name("phpmyadmin"));
    CHECK_FALSE(reserved_project_name("mailer"));
    CHECK(reserved_name_message("mail") == "mail.localhost is kept for Mailpit's inbox; choose another name");
    CHECK(reserved_name_message("phpmyadmin") == "phpmyadmin.localhost is kept for phpMyAdmin; choose another name");

    const test_util::TempDir tmp;
    const fs::path dir = tmp.path() / "mail";
    fs::create_directories(dir);
    AppConfig app;
    std::string error;
    CHECK_FALSE(add_project(app, dir, "mail", error));
    CHECK(error == "mail.localhost is kept for Mailpit's inbox; choose another name");
    const auto added = add_project(app, dir, "", error);  // named from the folder: steps aside
    REQUIRE(added);
    CHECK(added->name == "mail-2");

    // One added before D27 stays as it is.
    app.projects.push_back(Project{"mail", dir, true, false, {}});
    CHECK(find_project(app, "mail") != nullptr);
}

TEST_CASE("nginx serves mail.localhost only, the header from an owner-only file of its own") {
    NginxSpec spec;
    spec.nginx_dir = path_from_utf8("/opt/nginx");
    spec.work_dir = path_from_utf8("/data/web");
    spec.ports = {8080, 8443, true};
    NginxMail mail;
    mail.port = 9500;
    mail.auth_include = path_from_utf8("/data/mailpit/nginx-auth.conf");
    spec.mail = mail;
    std::string error;
    auto text = make_nginx_config(spec, error);
    REQUIRE(text);
    INFO(*text);
    CHECK(text->find("server_name mail.localhost;") != std::string::npos);
    CHECK(text->find("proxy_pass http://127.0.0.1:9500;") != std::string::npos);
    CHECK(text->find("include \"/data/mailpit/nginx-auth.conf\";") != std::string::npos);
    CHECK(text->find("Authorization") == std::string::npos);  // no secret in nginx.conf
    CHECK(text->find("default_server") != std::string::npos);
    CHECK(text->find("There's no FloServer project at this address.") != std::string::npos);
    CHECK(text->find("map $http_upgrade $floserver_connection") != std::string::npos);
    CHECK(text->find("keepalive 8") == std::string::npos);  // no kept connection holds Mailpit awake
    CHECK(text->find("return 302") == std::string::npos);   // http only
    // Another site may only send a plain visit to the inbox's pages.
    CHECK(text->find("map \"$http_sec_fetch_site:$http_sec_fetch_mode:$request_method:$uri\" $floserver_mail_cross {") !=
          std::string::npos);
    CHECK(text->find("\"~*^(cross-site|same-site):navigate:GET:/(?!api(/|$)|proxy)\" 0;") != std::string::npos);
    CHECK(text->find("\"~*^(cross-site|same-site):\" 1;") != std::string::npos);
    CHECK(text->find("\"http://mail.localhost:8080\" 0;") != std::string::npos);
    CHECK(text->find("if ($floserver_mail_cross) {\n            return 403;") != std::string::npos);
    CHECK(text->find("if ($floserver_mail_origin) {\n            return 403;") != std::string::npos);

    spec.mail->tls = NginxTls{path_from_utf8("/c/mail.pem"), path_from_utf8("/c/mail.key")};
    text = make_nginx_config(spec, error);
    REQUIRE(text);
    CHECK(text->find("return 302 https://mail.localhost:8443$request_uri;") != std::string::npos);
    CHECK(text->find("ssl_reject_handshake on;") != std::string::npos);
    CHECK(text->find("\"https://mail.localhost:8443\" 0;") != std::string::npos);  // the origin as served
    spec.mail.reset();
    text = make_nginx_config(spec, error);
    REQUIRE(text);
    CHECK(text->find("floserver_mail") == std::string::npos);
    spec.mail = mail;

    spec.mail->auth_include = path_from_utf8("/data/$bad/nginx-auth.conf");
    CHECK_FALSE(make_nginx_config(spec, error));
}

TEST_CASE("Mailpit's SMTP port: 1025, else the next free one, kept in config.json; its files owner-only") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_mailpit(data, "1.33.0");
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), AppConfig{}, error));

    // Whatever holds 1025 now (this test, or another program), Mailpit gets another.
    const auto held = LoopbackListener::open(kFirstMailPort, error);
    MailServiceConfig config;
    config.data_dir = data;
    int php_changes = 0;
    config.php_changed = [&php_changes] { ++php_changes; };
    Port chosen = 0;
    {
        MailService mail(config, SecretStore(data));
        mail.reload();
        const auto st = mail.status();
        INFO(st.dump(2));
        CHECK(st["installed"] == true);
        CHECK(st["state"] == "asleep");
        chosen = st["smtp_port"].get<Port>();
        CHECK(chosen != kFirstMailPort);
        CHECK(chosen > kFirstMailPort);
        CHECK(php_changes == 1);
        const auto app = load_app_config(app_config_file(data), error);
        REQUIRE(app);
        REQUIRE(read_mail_settings(*app).port);
        CHECK(*read_mail_settings(*app).port == chosen);
        const auto smtp = mail.smtp_port();
        REQUIRE(smtp);
        CHECK(*smtp == chosen);
        const auto inbox = mail.inbox();
        REQUIRE(inbox);
        // [::1] on the same port where the machine has it: nobody else gets mail sent to localhost.
        if (loopback_ipv6_available()) {
            std::string why;
            CHECK_FALSE(LoopbackListener::open_ipv6(chosen, why));
        }
        CHECK(inbox->port >= kFirstMailInboxPort);
        CHECK(inbox->auth_include == mailpit_paths(data).nginx_auth);

        // The password is in the secure store; the files hold its hash and the header.
        const MailpitPaths paths = mailpit_paths(data);
        const auto password = SecretStore(data).read(std::string(kMailSecretName));
        REQUIRE(password);
        CHECK(password->size() == 64);
        const std::string auth = test_util::read_file(paths.ui_auth);
        CHECK(auth.starts_with("floserver:$5$"));
        CHECK(auth.find(*password) == std::string::npos);
        CHECK(test_util::read_file(paths.nginx_auth) == mailpit_nginx_auth(*password));
        CHECK(st.dump().find(*password) == std::string::npos);
#ifndef _WIN32
        CHECK((fs::status(paths.ui_auth).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
        CHECK((fs::status(paths.nginx_auth).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
        CHECK((fs::status(paths.dir).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
        // Reloading again keeps the same files (no new salt each time).
        mail.reload();
        CHECK(test_util::read_file(paths.ui_auth) == auth);
        CHECK(php_changes == 1);
    }
    // Kept after a restart, even with 1025 free again.
    {
        MailService mail(config, SecretStore(data));
        mail.reload();
        CHECK(mail.status()["smtp_port"] == chosen);
    }
    // Not installed any more: no port held, nothing served.
    fs::remove_all(runtimes_dir(data) / "mailpit");
    {
        MailService mail(config, SecretStore(data));
        mail.reload();
        CHECK(mail.status()["state"] == "not_installed");
        CHECK_FALSE(mail.inbox());
        CHECK_FALSE(mail.smtp_port());
    }
}

TEST_CASE("a too-old Mailpit isn't run, and a project named mail blocks the inbox") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_mailpit(data, "1.20.0");
    AppConfig app;
    app.projects.push_back(Project{"mail", tmp.path() / "mail", true, false, {}});
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), app, error));
    MailServiceConfig config;
    config.data_dir = data;
    MailService mail(config, SecretStore(data));
    mail.reload();
    const auto st = mail.status();
    CHECK(st["state"] == "failed");
    CHECK(st["error"].get<std::string>().find("1.30.0") != std::string::npos);
    CHECK(st["inbox_blocked_by"] == "mail");
    CHECK_FALSE(mail.inbox());
    ServiceRefusal refusal;
    CHECK_FALSE(mail.send_test("", refusal));
    CHECK(refusal.code == "mail_failed");
    CHECK_FALSE(mail.set({{"max", "lots"}}, refusal));
    CHECK(refusal.code == "bad_params");
    CHECK_FALSE(mail.set({{"colour", true}}, refusal));
    CHECK(mail.set({{"max", 50}, {"notices", false}}, refusal));
    CHECK(mail.status()["settings"]["max"] == 50);
    CHECK(mail.status()["settings"]["notices"] == false);
    const auto saved = load_app_config(app_config_file(data), error);
    REQUIRE(saved);
    CHECK(saved->other["mail"]["max"] == 50);
}

TEST_CASE("Settings → Folders never touches the emails") {
    const test_util::TempDir tmp;
    const fs::path& data = tmp.path();
    const MailpitPaths paths = mailpit_paths(data);
    test_util::write_file(paths.database, "emails");
    test_util::write_file(fs::path(paths.database.native() + fs::path("-wal").native()), "wal");
    test_util::write_file(paths.log, "old log\n");
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(48);
    fs::last_write_time(paths.log, old);
    fs::last_write_time(paths.database, old);
    const auto logs = log_files(data);
    const auto left = leftovers(data, false, false, std::chrono::steady_clock::now() + std::chrono::seconds(5));
    for (const auto& list : {logs, left}) {
        for (const CleanItem& item : list) {
            CHECK(item.path.parent_path() != paths.dir);
        }
    }
    // Mailpit's own log is one Clear old logs looks after.
    CHECK(std::any_of(logs.begin(), logs.end(), [&](const CleanItem& i) { return i.path == paths.log; }));
}
