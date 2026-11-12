// Mailpit run by the service (D27), with the real program: woken by an SMTP
// connection and by an inbox request, asleep after the idle time even
// though the service asks its API after each connection, the counts and the
// new-mail notice, Clear, Keep running, mail.localhost through the real
// nginx with the password added, and the password never in Mailpit's
// arguments, its log or the status.
//
// Needs Mailpit on this machine: FLOSERVER_TEST_MAILPIT, else
// /usr/local/bin/mailpit (the release's linux-amd64 build). Skipped without it.

#ifndef _WIN32

#include "floserver/core/certificates.h"
#include "floserver/core/mail_service.h"
#include "floserver/core/mailpit.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/web_service.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

using namespace floserver::core;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::optional<fs::path> tool(const char* env, const char* fallback) {
    const auto set = get_env(env);
    const fs::path p = set ? fs::path(*set) : fs::path(fallback);
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? std::optional<fs::path>(p) : std::nullopt;
}

void fake_runtime(const fs::path& data, const std::string& name, const std::string& version) {
    const fs::path dir = runtimes_dir(data) / name / version;
    fs::create_directories(dir);
    test_util::write_file(dir / ".floserver-runtime.json",
                          nlohmann::json{{"schema", 1}, {"name", name}, {"version", version},
                                         {"platform", std::string(current_platform())}, {"variant", ""},
                                         {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
}

// A whole HTTP/1.0 exchange; the reply, or "" when it couldn't connect.
std::string http(Port port, const std::string& host, const std::string& path, const std::string& headers = "") {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval tv{40, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        close(s);
        return "";
    }
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n" + headers + "\r\n";
    (void)send(s, req.data(), req.size(), 0);
    std::string resp;
    char buf[4096];  // NOLINT(modernize-avoid-c-arrays)
    for (ssize_t n = 0; (n = recv(s, buf, sizeof buf, 0)) > 0;) {
        resp.append(buf, static_cast<std::size_t>(n));
    }
    close(s);
    return resp;
}

std::string status_code(const std::string& reply) { return reply.size() >= 12 ? reply.substr(9, 3) : ""; }

template <class F>
bool eventually(F&& f, std::chrono::milliseconds limit = 15s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (f()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return f();
}

// The running mailpit processes started from `program`: pid and command line.
std::vector<std::pair<int, std::string>> mailpit_processes(const fs::path& data) {
    std::vector<std::pair<int, std::string>> out;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        std::string cmd = test_util::read_file(it->path() / "cmdline");
        std::replace(cmd.begin(), cmd.end(), '\0', ' ');
        if (cmd.find("mailpit") != std::string::npos && cmd.find(data.string()) != std::string::npos) {
            out.emplace_back(std::stoi(name), cmd);
        }
    }
    return out;
}

// U+202E (right-to-left override), as bytes: a literal would trip clang-tidy.
const std::string kRlo = {'\xe2', '\x80', '\xae'};

// A whole HTTP/1.0 exchange on a Unix-domain socket; the reply, or "".
std::string http_unix(const fs::path& socket, const std::string& path, const std::string& headers = "") {
    const auto c = connect_local(socket);
    if (!c) {
        return "";
    }
    std::string resp;
    if (send_all(*c, "GET " + path + " HTTP/1.0\r\nHost: localhost\r\n" + headers + "\r\n")) {
        while (const auto got = receive_some(*c, 20s)) {
            if (got->empty()) {
                break;
            }
            resp += *got;
        }
    }
    close_socket(*c);
    return resp;
}

// One email sent by hand to `port`, with `subject` as it is; true when taken.
bool send_raw(Port port, const std::string& subject) {
    const auto c = connect_loopback(port);
    if (!c) {
        return false;
    }
    std::string got;
    const auto expect = [&](const std::string& code) {
        got.clear();
        while (got.find("\r\n") == std::string::npos) {
            const auto more = receive_some(*c, 30s);
            if (!more || more->empty()) {
                return false;
            }
            got += *more;
        }
        return got.starts_with(code);
    };
    const bool ok = expect("220") && send_all(*c, "HELO t\r\n") && expect("250") &&
                    send_all(*c, "MAIL FROM:<a@t.test>\r\n") && expect("250") &&
                    send_all(*c, "RCPT TO:<b@t.test>\r\n") && expect("250") && send_all(*c, "DATA\r\n") &&
                    expect("354") &&
                    send_all(*c, "Subject: " + subject + "\r\nTo: \"Bob" + kRlo + "\x1b[2J\" <b@t.test>\r\n\r\nhi\r\n.\r\n") &&
                    expect("250");
    (void)send_all(*c, "QUIT\r\n");
    close_socket(*c);
    return ok;
}

}  // namespace

TEST_CASE("Mailpit wakes on SMTP and on the inbox, sleeps when idle, and the service's own calls don't wake it") {
    const auto mailpit = tool("FLOSERVER_TEST_MAILPIT", "/usr/local/bin/mailpit");
    if (!mailpit) {
        MESSAGE("mailpit isn't installed here (FLOSERVER_TEST_MAILPIT); skipped");
        return;
    }
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_runtime(data, "mailpit", "1.33.0");
    fs::create_symlink(*mailpit, runtimes_dir(data) / "mailpit" / "1.33.0" / "mailpit");
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), AppConfig{}, error));

    MailServiceConfig config;
    config.data_dir = data;
    config.idle_after = 2s;
    config.reload_every = 1s;
    MailService mail(config, SecretStore(data));
    mail.start();
    auto st = mail.status();
    INFO(st.dump(2));
    REQUIRE(st["installed"] == true);
    CHECK(st["state"] == "asleep");
    CHECK(st["notice_seq"] == 0);
    CHECK(mailpit_processes(data).empty());
    const auto password = SecretStore(data).read(std::string(kMailSecretName));
    REQUIRE(password);
    // A socket file left by a Mailpit that was killed is cleared before the start.
    const MailpitPaths paths = mailpit_paths(data);
    test_util::write_file(paths.smtp_socket, "stale");

    // An email wakes it: the first connection waits for Mailpit to start.
    ServiceRefusal refusal;
    const bool sent = mail.send_test("dev@example.com", refusal);
    if (!sent) {
        MESSAGE(refusal.message << " " << mail.status().dump() << " "
                                << test_util::read_file(mailpit_paths(data).log));
    }
    REQUIRE(sent);
    REQUIRE(eventually([&] { return mail.status()["notice_seq"] == 1; }));
    st = mail.status();
    CHECK(st["state"] == "running");
    CHECK(st["total"] == 1);
    CHECK(st["unread"] == 1);
    CHECK(st["latest"]["subject"] == "FloServer test email");
    CHECK(st["latest"]["to"] == "dev@example.com");
    CHECK(st["latest"]["new_in_batch"] == 1);
    CHECK(st["size_bytes"].get<std::uint64_t>() > 0);

    // The password is nowhere a person or another program sees it.
    const auto running = mailpit_processes(data);
    REQUIRE(running.size() == 1);
    const std::string cmd = running.front().second;
    CHECK(cmd.find(*password) == std::string::npos);
    CHECK(cmd.find("--ui-auth-file") != std::string::npos);
    CHECK(test_util::read_file(mailpit_paths(data).log).find(*password) == std::string::npos);
    CHECK(mail.status().dump().find(*password) == std::string::npos);
    // Mailpit listens on its two sockets only: no TCP port another program could take first.
    CHECK(cmd.find("--smtp unix:" + mailpit_socket_text(paths.smtp_socket) + ":0600") != std::string::npos);
    CHECK(cmd.find("--listen unix:" + mailpit_socket_text(paths.http_socket) + ":0600") != std::string::npos);
    CHECK(cmd.find("127.0.0.1") == std::string::npos);
    CHECK(fs::is_socket(fs::symlink_status(paths.smtp_socket)));
    CHECK(fs::is_socket(fs::symlink_status(paths.http_socket)));
    CHECK((fs::status(paths.dir).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    CHECK((fs::status(paths.http_socket).permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
          fs::perms::none);
    // Straight to Mailpit's own socket: its 401 without the password.
    CHECK(status_code(http_unix(paths.http_socket, "/api/v1/info")) == "401");
    const std::string login = "Authorization: " + mailpit_basic_auth(*password) + "\r\n";
    CHECK(status_code(http_unix(paths.http_socket, "/api/v1/info", login)) == "200");

    // Whatever an email says is shown cleaned: no escapes, no bidi tricks, cut short.
    REQUIRE(send_raw(*mail.smtp_port(), "Pay" + kRlo + "fdp.exe\x1b]0;x\x07 " + std::string(400, 'z')));
    REQUIRE(eventually([&] { return mail.status()["notice_seq"] == 2; }));
    st = mail.status();
    const std::string subject = st["latest"]["subject"].get<std::string>();
    CHECK(subject.starts_with("Payfdp.exe ]0;x  zzz"));
    CHECK(subject.find('\x1b') == std::string::npos);
    CHECK(subject.size() <= 203);
    CHECK(st["latest"]["to"].get<std::string>().find('\x1b') == std::string::npos);
    CHECK(st["latest"]["to"].get<std::string>().find(kRlo) == std::string::npos);
    CHECK(st["total"] == 2);

    // Asleep after the idle time, though the service asked its API after the email.
    const auto inbox = mail.inbox();
    REQUIRE(inbox);
    REQUIRE(eventually([&] { return mail.status()["state"] == "asleep"; }, 10s));
    CHECK(eventually([&] { return mailpit_processes(data).empty(); }, 5s));  // stopped just after
    CHECK(mail.status()["total"] == 2);  // the last values stand

    // The sockets of a Mailpit that was ended outright stay behind (always
    // on Windows): the next start clears them before it touches the folder.
    if (!fs::exists(fs::symlink_status(paths.http_socket))) {
        test_util::write_file(paths.http_socket, "left behind");
    }
    CHECK(fs::exists(fs::symlink_status(paths.http_socket)));
    // The inbox wakes it: without the header nginx adds, Mailpit's own 401.
    CHECK(status_code(http(inbox->port, "mail.localhost", "/api/v1/info")) == "401");
    CHECK(mail.status()["state"] == "running");
    CHECK(status_code(http(inbox->port, "mail.localhost", "/api/v1/info", login)) == "200");
    REQUIRE(eventually([&] { return mail.status()["state"] == "asleep"; }, 10s));
    CHECK(mail.status()["notice_seq"] == 2);  // inbox requests aren't new email

    // mail.recent (D32, flo mcp): it wakes Mailpit to list them, newest first, cleaned.
    const auto recent = mail.recent(10, refusal);
    INFO(refusal.message);
    REQUIRE(recent);
    CHECK((*recent)["total"] == 2);
    REQUIRE((*recent)["messages"].size() == 2);
    const nlohmann::json& newest = (*recent)["messages"][0];
    CHECK(newest["subject"].get<std::string>().starts_with("Payfdp.exe ]0;x  zzz"));
    CHECK(newest["subject"].get<std::string>().find('\x1b') == std::string::npos);
    CHECK_FALSE(newest["id"].get<std::string>().empty());
    CHECK_FALSE(newest["time"].get<std::string>().empty());
    CHECK(newest["read"] == false);
    CHECK((*recent)["messages"][1]["subject"] == "FloServer test email");
    CHECK((*recent)["messages"][1]["to"] == "dev@example.com");
    CHECK(mail.recent(1, refusal)->at("messages").size() == 1);
    CHECK(mail.recent(500, refusal)->at("messages").size() == 2);
    CHECK(mail.status()["total"] == 2);
    CHECK(mail.status()["unread"] == 2);  // listing marks nothing read
    REQUIRE(eventually([&] { return mail.status()["state"] == "asleep"; }, 10s));

    // Clear wakes it and empties the inbox.
    REQUIRE(mail.clear(refusal));
    CHECK(mail.status()["total"] == 0);
    CHECK_FALSE(mail.status().contains("latest"));
    // With nothing caught, recent doesn't wake it.
    REQUIRE(eventually([&] { return mail.status()["state"] == "asleep"; }, 10s));
    const auto none = mail.recent(5, refusal);
    REQUIRE(none);
    CHECK((*none)["messages"].empty());
    CHECK(mail.status()["state"] == "asleep");

    // Keep running: up without a connection, past the idle time.
    REQUIRE(eventually([&] { return mail.status()["state"] == "asleep"; }, 10s));
    REQUIRE(mail.set({{"keep_running", true}}, refusal));
    REQUIRE(eventually([&] { return mail.status()["state"] == "running"; }, 10s));
    std::this_thread::sleep_for(3500ms);
    CHECK(mail.status()["state"] == "running");
    REQUIRE(mail.set({{"keep_running", false}}, refusal));
    REQUIRE(eventually([&] { return mail.status()["state"] == "asleep"; }, 10s));

    // Each SMTP connection that delivered email is one more notice.
    REQUIRE(mail.send_test("", refusal));
    REQUIRE(eventually([&] { return mail.status()["notice_seq"] == 3; }));
    CHECK(mail.status()["latest"]["to"] == "you@example.com");

    // Removing it: stopped and held, the emails deleted when asked.
    {
        const auto hold = mail.hold_for_removal();
        CHECK(mailpit_processes(data).empty());
        CHECK_FALSE(mail.send_test("", refusal));
        REQUIRE(mail.delete_emails(error));
        CHECK_FALSE(fs::exists(mailpit_paths(data).database));
    }
    mail.stop();
    CHECK(mailpit_processes(data).empty());
}

TEST_CASE("mail.localhost through the real nginx: the inbox's password added, nothing else served by that name") {
    const auto mailpit = tool("FLOSERVER_TEST_MAILPIT", "/usr/local/bin/mailpit");
    const auto nginx = tool("FLOSERVER_TEST_NGINX", "/usr/sbin/nginx");
    if (!mailpit || !nginx) {
        MESSAGE("mailpit or nginx isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_runtime(data, "mailpit", "1.33.0");
    fs::create_symlink(*mailpit, runtimes_dir(data) / "mailpit" / "1.33.0" / "mailpit");
    fake_runtime(data, "nginx", "1.24.0");
    const fs::path nginx_dir = runtimes_dir(data) / "nginx" / "1.24.0";
    fs::create_directories(nginx_dir / "conf");
    fs::create_symlink(*nginx, nginx_dir / "nginx");
    for (const char* f : {"mime.types", "fastcgi_params"}) {
        std::error_code ec;
        fs::copy_file(fs::path("/etc/nginx") / f, nginx_dir / "conf" / f, ec);
        if (ec) {
            test_util::write_file(nginx_dir / "conf" / f, std::string(f) == "mime.types" ? "types { text/html html; }\n" : "\n");
        }
    }
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), AppConfig{}, error));

    MailServiceConfig mail_config;
    mail_config.data_dir = data;
    mail_config.idle_after = 2s;
    MailService mail(mail_config, SecretStore(data));
    CertStore certs(data);
    const auto ports = pick_web_ports([](Port p) { return p > 1024 && can_listen_on_loopback(p); });
    REQUIRE(ports);
    WebServiceConfig config;
    config.data_dir = data;
    config.fixed_ports = WebPorts{static_cast<Port>(ports->http + 21000), static_cast<Port>(ports->https + 21000), true};
    config.idle_after = 2s;
    config.reload_every = 1s;
    config.mail_inbox = [&mail] { return mail.inbox(); };
    mail.start();
    WebService web(config, certs);
    web.start();
    const Port port = config.fixed_ports->http;
    auto st = web.status();
    INFO(st.dump(2));
    REQUIRE(st["nginx"]["running"] == true);
    CHECK(st["mail"]["address"] == "http://mail.localhost:" + std::to_string(port));

    // nginx adds the password: Mailpit answers its API.
    const std::string reply = http(port, "mail.localhost", "/api/v1/info");
    CHECK(status_code(reply) == "200");
    CHECK(reply.find("\"Messages\"") != std::string::npos);
    CHECK(http(port, "other.localhost", "/api/v1/info").find("404") != std::string::npos);
    // Another site can't make the browser use the inbox's password: only a
    // plain visit to its pages comes in; an Origin must be the inbox's own.
    const std::string own = "http://mail.localhost:" + std::to_string(port);
    const auto code = [&](const std::string& path, const std::string& headers) {
        // With the port, as a browser sends it (Mailpit compares Origin with Host).
        return status_code(http(port, "mail.localhost:" + std::to_string(port), path, headers));
    };
    CHECK(code("/api/v1/message/latest/link-check", "Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: no-cors\r\n") == "403");
    CHECK(code("/api/v1/info", "Sec-Fetch-Site: same-site\r\nSec-Fetch-Mode: cors\r\n") == "403");
    CHECK(code("/api/v1/info", "Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: navigate\r\n") == "403");
    CHECK(code("/proxy?url=x", "Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: navigate\r\n") == "403");
    CHECK(code("/", "Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-Dest: document\r\n") == "200");
    // In a frame on another site (clickjacking): refused, and never framed anyway.
    CHECK(code("/", "Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-Dest: iframe\r\n") == "403");
    CHECK(code("/", "Sec-Fetch-Site: same-site\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-Dest: iframe\r\n") == "403");
    CHECK(code("/", "Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: navigate\r\n") == "403");  // no Dest: not a page
    {
        const std::string page = http(port, "mail.localhost:" + std::to_string(port), "/");
        CHECK(status_code(page) == "200");
        CHECK(page.find("Content-Security-Policy: frame-ancestors 'self'") != std::string::npos);
        CHECK(page.find("X-Frame-Options: SAMEORIGIN") != std::string::npos);
        CHECK(page.find("Referrer-Policy: no-referrer") != std::string::npos);
    }
    CHECK(code("/api/v1/info", "Origin: http://evil.test\r\n") == "403");
    CHECK(code("/api/v1/info", "Origin: null\r\n") == "403");
    CHECK(code("/api/v1/info", "Origin: " + own + "\r\nSec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: cors\r\n") == "200");
    CHECK(code("/api/v1/info", "Sec-Fetch-Site: none\r\nSec-Fetch-Mode: navigate\r\n") == "200");
    // nginx.conf holds no secret; the header's file is owner-only.
    const auto password = SecretStore(data).read(std::string(kMailSecretName));
    REQUIRE(password);
    const std::string conf = test_util::read_file(web_dir(data) / "nginx.conf");
    CHECK(conf.find(*password) == std::string::npos);
    CHECK(conf.find(mailpit_basic_auth(*password)) == std::string::npos);
    CHECK(conf.find("server_name mail.localhost;") != std::string::npos);
    CHECK((fs::status(mailpit_paths(data).nginx_auth).permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
          fs::perms::none);

    // A project named mail keeps its address: the inbox isn't served.
    {
        AppConfig app;
        app.projects.push_back(Project{"mail", tmp.path() / "mail", true, false, {}});
        test_util::write_file(tmp.path() / "mail" / "index.html", "the project");
        REQUIRE(save_app_config(app_config_file(data), app, error));
    }
    st = web.reload();
    CHECK(st["mail"]["name_taken"] == true);
    CHECK_FALSE(st["mail"].contains("address"));
    CHECK(http(port, "mail.localhost", "/").find("the project") != std::string::npos);
    mail.reload();
    CHECK(mail.status()["inbox_blocked_by"] == "mail");

    web.stop();
    mail.stop();
    CHECK(mailpit_processes(data).empty());
}

TEST_CASE("PHP's mail() reaches Mailpit through sendmail_path, from a data folder with a space in its path") {
    const auto mailpit = tool("FLOSERVER_TEST_MAILPIT", "/usr/local/bin/mailpit");
    const auto php = tool("FLOSERVER_TEST_PHP_CGI", "/usr/bin/php-cgi");
    if (!mailpit || !php) {
        MESSAGE("mailpit or php-cgi isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "Application Support" / "FloServer";  // as on every Mac
    fake_runtime(data, "mailpit", "1.33.0");
    // A copy, not a link: sendmail_path names the install's own program.
    fs::copy_file(*mailpit, runtimes_dir(data) / "mailpit" / "1.33.0" / "mailpit");
    fs::permissions(runtimes_dir(data) / "mailpit" / "1.33.0" / "mailpit", fs::perms::owner_all);
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), AppConfig{}, error));
    MailServiceConfig config;
    config.data_dir = data;
    config.idle_after = 5s;
    MailService mail(config, SecretStore(data));
    mail.start();
    const auto app = load_app_config(app_config_file(data), error);
    REQUIRE(app);
    const auto lines = mail_php_ini(*app, list_installed(runtimes_dir(data)));
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().second.find("Application Support") != std::string::npos);
    CHECK_FALSE(mail.status().contains("php_error"));
    std::string ini = "display_errors = stderr\n";
    for (const auto& [key, value] : lines) {
        ini.append(key).append(" = ").append(value).append("\n");
    }
    test_util::write_file(tmp.path() / "php.ini", ini);
    test_util::write_file(tmp.path() / "send.php",
                          "<?php echo mail('dev@example.com', 'From PHP', 'Sent by mail()') ? 'sent' : 'failed';");
    const std::string cmd = "'" + php->string() + "' -n -c '" + (tmp.path() / "php.ini").string() + "' -f '" +
                            (tmp.path() / "send.php").string() + "' 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c): a test, with fixed arguments
    REQUIRE(pipe != nullptr);
    std::string out;
    char buf[1024];  // NOLINT(modernize-avoid-c-arrays)
    while (fgets(buf, sizeof buf, pipe) != nullptr) {
        out += buf;
    }
    pclose(pipe);
    INFO(out);
    CHECK(out.find("sent") != std::string::npos);
    REQUIRE(eventually([&] { return mail.status()["notice_seq"] == 1; }, 30s));
    CHECK(mail.status()["latest"]["subject"] == "From PHP");
    mail.stop();
}

#endif
