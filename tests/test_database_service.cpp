// The database service with the real MariaDB (D11, D12): set up on first
// use, root's password set, a project's own database and login, the
// service relaying the public port, sleep when idle, wake on a connection,
// and nothing left running after stop().
//
// Needs MariaDB on this machine (Linux CI installs mariadb-server). The
// system's files are laid out as a FloServer runtime with symlinks.

#ifndef _WIN32

#include "floserver/core/database_service.h"
#include "floserver/core/mariadb_settings.h"
#include "floserver/core/project_service.h"
#include "floserver/core/shared_sites.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <csignal>
#include <cstdio>
#include <unistd.h>

#include <thread>

using namespace floserver::core;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

void fake_runtime(const fs::path& data, const std::string& name, const std::string& version, const std::string& variant) {
    const fs::path dir = runtimes_dir(data) / name / version;
    fs::create_directories(dir);
    test_util::write_file(dir / ".floserver-runtime.json",
                          nlohmann::json{{"schema", 1}, {"name", name}, {"version", version},
                                         {"platform", std::string(current_platform())}, {"variant", variant},
                                         {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
}

// The mariadb client's output for `sql`, or "ERROR..." when it failed.
std::string query(Port port, const std::string& user, const std::string& password, const std::string& sql) {
    const std::string cmd = "MYSQL_PWD='" + password + "' /usr/bin/mariadb --no-defaults --protocol=TCP --host=127.0.0.1 --port=" +
                            std::to_string(port) + " --user=" + user + " --batch --skip-column-names -e \"" + sql + "\" 2>&1";
    FILE* p = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c): a test, fixed text
    if (p == nullptr) {
        return "ERROR popen";
    }
    std::string out;
    char buf[512];  // NOLINT(modernize-avoid-c-arrays)
    while (fgets(buf, sizeof buf, p) != nullptr) {
        out += buf;
    }
    const int rc = pclose(p);
    while (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return rc == 0 ? out : "ERROR " + out;
}

nlohmann::json server_status(const DatabaseService& db) {
    const auto s = db.status();
    return s["servers"].empty() ? nlohmann::json() : s["servers"][0];
}

template <class F>
bool eventually(F&& f, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (f()) {
            return true;
        }
        std::this_thread::sleep_for(100ms);
    }
    return f();
}

}  // namespace

TEST_CASE("a project's database runs on the real MariaDB, sleeps when idle and wakes on a connection") {
    std::error_code ec;
    if (!fs::is_regular_file("/usr/sbin/mariadbd", ec) || !fs::is_regular_file("/usr/bin/mariadb-install-db", ec)) {
        // CI installs it: there a skip would hide a broken setup.
        REQUIRE_MESSAGE(!get_env("CI"), "MariaDB is missing from this CI machine");
        MESSAGE("MariaDB isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    // A long path: MariaDB's socket must still fit (107 bytes on Linux).
    const fs::path data = tmp.path() / std::string(90, 'd') / "data";
    fake_runtime(data, "php", "8.3.6", "nts");
    fake_runtime(data, "mariadb", "10.11.14", "");
    const fs::path m = runtimes_dir(data) / "mariadb" / "10.11.14";
    fs::create_directories(m / "bin");
    fs::create_directories(m / "sbin");
    for (const char* t : {"mariadb", "mariadb-admin", "mariadb-install-db", "my_print_defaults", "resolveip"}) {
        fs::create_symlink(fs::path("/usr/bin") / t, m / "bin" / t, ec);
    }
    fs::create_symlink("/usr/sbin/mariadbd", m / "sbin" / "mariadbd");
    fs::create_directory_symlink("/usr/share", m / "share");

    const fs::path shop = tmp.path() / "my-shop";
    test_util::write_file(shop / "index.php", "<?php echo 1;");
    test_util::write_file(shop / "floserver.json", R"({"php": "8.3", "database": "mariadb-10.11"})");
    const fs::path plain = tmp.path() / "plain";
    test_util::write_file(plain / "index.php", "<?php echo 1;");
    // "root" would be MariaDB's root account.
    const fs::path root_project = tmp.path() / "root";
    test_util::write_file(root_project / "floserver.json", R"({"php": "8.3", "database": "mariadb-10.11"})");
    AppConfig app;
    std::string error;
    REQUIRE(add_project(app, shop, "", error));
    REQUIRE(add_project(app, plain, "", error));
    REQUIRE(add_project(app, root_project, "", error));
    for (Project& p : app.projects) {
        p.on = true;
    }
    REQUIRE(save_app_config(app_config_file(data), app, error));

    const SecretStore secrets(data);
    DatabaseServiceConfig cfg;
    cfg.data_dir = data;
    cfg.idle_after = 3s;
    cfg.reload_every = 1s;
    // Never a port a MariaDB of this machine might use.
    cfg.probe = [](Port p) { return p != 3306 && can_listen_on_loopback(p); };
    DatabaseService db(cfg, secrets);
    db.start();

    auto s = server_status(db);
    REQUIRE(s.is_object());
    CHECK(s["series"] == "10.11");
    CHECK(s["state"] == "sleeping");
    CHECK(s["projects"] == nlohmann::json{"my-shop", "root"});
    const fs::path folder = data / "data" / "mariadb-10.11";
    const Port port = s["port"].get<Port>();
    CHECK(port != 3306);

    SUBCASE("shared sites' login: www, its own databases only (D19)") {
        ServiceRefusal refusal;
        CHECK_FALSE(db.shared_login(refusal));
        CHECK(refusal.code == "shared_off");
        SharedConfig shared;
        shared.on = true;
        shared.dir = tmp.path() / "www";
        fs::create_directories(*shared.dir);
        shared.php = "8.3";
        shared.web = WebServer::Nginx;
        write_shared_config(app, shared);
        REQUIRE(save_app_config(app_config_file(data), app, error));
        CHECK_FALSE(db.shared_login(refusal));
        CHECK(refusal.code == "no_database");
        shared.database = DatabaseSpec{"mariadb", "10.11"};
        write_shared_config(app, shared);
        REQUIRE(save_app_config(app_config_file(data), app, error));
        CHECK(server_status(db).is_object());

        const auto login = db.shared_login(refusal);
        REQUIRE_MESSAGE(login, refusal.message);
        CHECK(login->user == "www");
        CHECK(login->database == "www");
        CHECK(login->port == port);
        CHECK(query(port, "www", login->password, "USE www; CREATE TABLE t (x INT); SHOW TABLES") == "t");
        // It makes its own www_ databases, and touches nothing else.
        CHECK(query(port, "www", login->password, "CREATE DATABASE www_blog; USE www_blog; CREATE TABLE p (x INT); "
                                                  "SHOW TABLES") == "p");
        CHECK(query(port, "www", login->password, "CREATE DATABASE wwwx").starts_with("ERROR"));
        CHECK(query(port, "www", login->password, "CREATE DATABASE blog").starts_with("ERROR"));
        const auto shop_login = db.project_login("my-shop", refusal);
        REQUIRE(shop_login);
        CHECK(query(port, "www", login->password, "USE my_shop").starts_with("ERROR"));
        CHECK(query(port, "my_shop", shop_login->password, "USE www_blog").starts_with("ERROR"));
        const auto again = db.shared_login(refusal);
        REQUIRE(again);
        CHECK(again->password == login->password);

        // '_' in a GRANT is a wildcard: a project named w-w (user w_w) must
        // not reach www, nor my-shop another project's myashop.
        const fs::path ww = tmp.path() / "w-w";
        test_util::write_file(ww / "floserver.json", R"({"php": "8.3", "database": "mariadb-10.11"})");
        REQUIRE(add_project(app, ww, "", error));
        app.projects.back().on = true;
        REQUIRE(save_app_config(app_config_file(data), app, error));
        auto ww_login = db.project_login("w-w", refusal);
        REQUIRE_MESSAGE(ww_login, refusal.message);
        CHECK(query(port, "w_w", ww_login->password, "USE www").starts_with("ERROR"));
        CHECK(query(port, "w_w", ww_login->password, "USE w_w; SELECT 1") == "1");
        CHECK(query(port, "my_shop", shop_login->password, "CREATE DATABASE myashop").starts_with("ERROR"));
        // The unescaped grant earlier versions made is removed on the next login.
        const auto root = secrets.read("mariadb-10.11/root");
        REQUIRE(root);
        CHECK(query(port, "root", *root, "GRANT ALL PRIVILEGES ON w_w.* TO 'w_w'@'127.0.0.1'; FLUSH PRIVILEGES; SELECT 1") == "1");
        CHECK(query(port, "w_w", ww_login->password, "USE www; SELECT 1") == "1");  // the old hole
        ww_login = db.project_login("w-w", refusal);
        REQUIRE(ww_login);
        CHECK(query(port, "w_w", ww_login->password, "USE www").starts_with("ERROR"));
        CHECK(query(port, "w_w", ww_login->password, "USE w_w; SELECT 1") == "1");
        // _shared isn't listed as a project.
        const auto servers = db.status()["servers"];
        REQUIRE(servers.size() == 1);
        CHECK(servers[0]["shared"] == true);
        for (const auto& name : servers[0]["projects"]) {
            CHECK(name.get<std::string>() != "_shared");
        }

        // A project from before shared sites with a www name stops the shared login.
        const fs::path old = tmp.path() / "www-blog";
        fs::create_directories(old);
        REQUIRE(add_project(app, old, "", error));
        REQUIRE(save_app_config(app_config_file(data), app, error));
        CHECK_FALSE(db.shared_login(refusal));
        CHECK(refusal.code == "reserved_name");
    }

    SUBCASE("logins") {
        ServiceRefusal refusal;
        CHECK_FALSE(db.project_login("plain", refusal));
        CHECK(refusal.code == "no_database");
        CHECK_FALSE(db.project_login("nobody", refusal));
        CHECK(refusal.code == "unknown_project");
        CHECK_FALSE(db.project_login("root", refusal));
        CHECK(refusal.code == "reserved_name");

        const auto login = db.project_login("my-shop", refusal);
        REQUIRE_MESSAGE(login, refusal.message);
        CHECK(login->port == port);
        CHECK(login->database == "my_shop");
        CHECK(login->user == "my_shop");
        CHECK(login->password.size() == 24);
        s = server_status(db);
        CHECK(s["state"] == "running");
        const auto pid = s["pid"].get<std::uint32_t>();

        // Through the service's port, as the project's own user.
        CHECK(query(port, "my_shop", login->password, "SELECT DATABASE(); USE my_shop; CREATE TABLE t (x INT); SELECT COUNT(*) FROM t") ==
              "NULL\n0");
        CHECK(query(port, "my_shop", login->password, "USE my_shop; SHOW TABLES") == "t");
        CHECK(query(port, "my_shop", "wrong", "SELECT 1").starts_with("ERROR"));
        CHECK(query(port, "root", "", "SELECT 1").starts_with("ERROR"));
        const auto root = secrets.read("mariadb-10.11/root");
        REQUIRE(root);
        CHECK(query(port, "root", *root, "SELECT 1") == "1");
        // Only its own database.
        CHECK(query(port, "my_shop", login->password, "CREATE DATABASE other").starts_with("ERROR"));
        // The init file with root's password is gone.
        CHECK_FALSE(fs::exists(folder / "data" / "init.sql"));
        CHECK(fs::exists(folder / "installed"));

        // Asking again gives the same login (the password is kept).
        const auto again = db.project_login("my-shop", refusal);
        REQUIRE(again);
        CHECK(again->password == login->password);

        // Idle: shut down cleanly; the port stays and a connection wakes it.
        CHECK(eventually([&] { return server_status(db)["state"] == "sleeping"; }, 20s));
        CHECK(eventually([&] { return ::kill(static_cast<pid_t>(pid), 0) != 0; }, 10s));
        CHECK(query(port, "my_shop", login->password, "USE my_shop; SHOW TABLES") == "t");
        s = server_status(db);
        CHECK(s["state"] == "running");
        const auto pid2 = s["pid"].get<std::uint32_t>();
        CHECK(pid2 != pid);

        // Turning the projects off: the server goes, and a connection
        // doesn't bring it back.
        app.projects[0].on = false;
        app.projects[2].on = false;
        REQUIRE(save_app_config(app_config_file(data), app, error));
        CHECK(db.reload()["servers"].empty());
        CHECK(eventually([&] { return ::kill(static_cast<pid_t>(pid2), 0) != 0; }, 10s));
        CHECK(query(port, "my_shop", login->password, "SELECT 1").starts_with("ERROR"));
        CHECK_FALSE(db.project_login("my-shop", refusal));
        CHECK(refusal.code == "database_off");
        std::this_thread::sleep_for(1s);
        CHECK(server_status(db).is_null());  // still off: nothing started again

        // On again: the same port (.env files have it).
        app.projects[0].on = true;
        REQUIRE(save_app_config(app_config_file(data), app, error));
        CHECK(eventually([&] { return server_status(db).is_object(); }, 15s));
        CHECK(server_status(db)["port"] == port);
        CHECK(query(port, "my_shop", login->password, "USE my_shop; SHOW TABLES") == "t");
    }

    SUBCASE("a lost root password is replaced") {
        ServiceRefusal refusal;
        const auto login = db.project_login("my-shop", refusal);
        REQUIRE_MESSAGE(login, refusal.message);
        db.stop();
        REQUIRE(secrets.remove("mariadb-10.11/root"));
        DatabaseService again(cfg, secrets);
        again.start();
        CHECK(server_status(again)["port"] == port);  // kept
        const auto login2 = again.project_login("my-shop", refusal);
        REQUIRE_MESSAGE(login2, refusal.message);
        CHECK(login2->password == login->password);
        const auto root = secrets.read("mariadb-10.11/root");
        REQUIRE(root);
        CHECK(query(port, "root", *root, "SELECT 1") == "1");
        // A wrong saved password is replaced too.
        again.stop();
        std::string err;
        REQUIRE(secrets.save("mariadb-10.11/root", "wrong", err));
        DatabaseService third(cfg, secrets);
        third.start();
        REQUIRE_MESSAGE(third.project_login("my-shop", refusal), refusal.message);
        CHECK(secrets.read("mariadb-10.11/root") != std::optional<std::string>("wrong"));
    }

    SUBCASE("a half-set-up data folder is moved aside and set up again") {
        fs::create_directories(folder / "data" / "mysql");
        test_util::write_file(folder / "data" / "leftover", "x");
        ServiceRefusal refusal;
        REQUIRE_MESSAGE(db.project_login("my-shop", refusal), refusal.message);
        bool aside = false;
        for (const auto& e : fs::directory_iterator(folder)) {
            aside = aside || (path_to_utf8(e.path().filename()).starts_with("data.failed-") &&
                              fs::exists(e.path() / "leftover"));
        }
        CHECK(aside);
    }

    SUBCASE("stop ends mariadbd") {
        CHECK(query(port, "root", "x", "SELECT 1").starts_with("ERROR"));  // wakes it; root's password is wrong
        s = server_status(db);
        REQUIRE_MESSAGE(s["state"] == "running", s.dump());
        const auto pid = s["pid"].get<std::uint32_t>();
        db.stop();
        CHECK(::kill(static_cast<pid_t>(pid), 0) != 0);
        CHECK(db.status()["servers"].empty());
    }
}

TEST_CASE("Settings > Database on the real MariaDB: my.ini values, custom.ini's filter, a restart, root's new password") {
    std::error_code ec;
    if (!fs::is_regular_file("/usr/sbin/mariadbd", ec) || !fs::is_regular_file("/usr/bin/mariadb-install-db", ec)) {
        REQUIRE_MESSAGE(!get_env("CI"), "MariaDB is missing from this CI machine");
        MESSAGE("MariaDB isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_runtime(data, "php", "8.3.6", "nts");
    fake_runtime(data, "mariadb", "10.11.14", "");
    const fs::path m = runtimes_dir(data) / "mariadb" / "10.11.14";
    fs::create_directories(m / "bin");
    fs::create_directories(m / "sbin");
    for (const char* t : {"mariadb", "mariadb-admin", "mariadb-install-db", "my_print_defaults", "resolveip"}) {
        fs::create_symlink(fs::path("/usr/bin") / t, m / "bin" / t, ec);
    }
    fs::create_symlink("/usr/sbin/mariadbd", m / "sbin" / "mariadbd");
    fs::create_directory_symlink("/usr/share", m / "share");
    const fs::path blog = tmp.path() / "blog";
    test_util::write_file(blog / "floserver.json", R"({"php": "8.3", "database": "mariadb-10.11"})");
    AppConfig app;
    std::string error;
    REQUIRE(add_project(app, blog, "", error));
    app.projects.front().on = true;
    app.other["mariadb"] = {{"10.11", {{"ini", {{"max_connections", "77"}}}}}};
    REQUIRE(save_app_config(app_config_file(data), app, error));
    // Lines that would let anyone in, or open it to the network, are left out.
    test_util::write_file(mariadb_custom_ini_file(data, "10.11"),
                          "skip-grant-tables\nloose-bind-address=0.0.0.0\nwait_timeout=123\n");

    const SecretStore secrets(data);
    DatabaseServiceConfig cfg;
    cfg.data_dir = data;
    cfg.idle_after = 600s;
    cfg.reload_every = 1s;
    cfg.probe = [](Port p) { return p != 3306 && can_listen_on_loopback(p); };
    DatabaseService db(cfg, secrets);
    db.start();
    ServiceRefusal refusal;
    const auto login = db.project_login("blog", refusal);
    REQUIRE_MESSAGE(login, refusal.message);
    const Port port = login->port;
    const auto root = secrets.read("mariadb-10.11/root");
    REQUIRE(root);
    CHECK(query(port, "root", *root, "SELECT @@max_connections") == "77");
    CHECK(query(port, "root", *root, "SELECT @@wait_timeout") == "123");
    CHECK(query(port, "root", *root, "SELECT @@bind_address") == "127.0.0.1");
    CHECK(query(port, "root", "wrong", "SELECT 1").starts_with("ERROR"));
    CHECK(query(port, "blog", login->password, "USE blog; CREATE TABLE kept (x INT); INSERT INTO kept VALUES (7)").empty());

    auto st = db.series_status("10.11");
    CHECK(st["databases"] == nlohmann::json{"blog"});
    REQUIRE(st["server"].is_object());
    CHECK(st["server"]["state"] == "running");
    const auto pid = st["server"]["pid"].get<std::uint32_t>();

    // A new value through the project service (as database.set does): only this server restarts.
    ProjectService projects(data);
    bool changed = false;
    REQUIRE(projects.database_set("10.11", {{"max-connections", "88"}}, nullptr, nullptr, changed, refusal));
    CHECK(changed);
    CHECK(db.restart_if_running("10.11"));
    CHECK(eventually([&] { return query(port, "root", *root, "SELECT @@max_connections") == "88"; }, 60s));
    st = db.series_status("10.11");
    CHECK(st["server"]["pid"].get<std::uint32_t>() != pid);
    CHECK(::kill(static_cast<pid_t>(pid), 0) != 0);
    CHECK(query(port, "blog", login->password, "SELECT x FROM blog.kept") == "7");  // a clean restart keeps the data

    const auto final_ini = db.final_ini("10.11", refusal);
    REQUIRE(final_ini);
    const std::string text = read_small_file(*final_ini, 65536).value_or("");
    CHECK(text.find("max_connections=88") != std::string::npos);
    CHECK(text.find("\nskip-grant-tables") == std::string::npos);
    CHECK(text.find("# FloServer left out line 1:") != std::string::npos);

    // root's new password: the old one stops working.
    CHECK_FALSE(db.set_root_password("10.11", "short", refusal));
    CHECK(refusal.code == "bad_params");
    REQUIRE_MESSAGE(db.set_root_password("10.11", "New-Root_pass9", refusal), refusal.message);
    CHECK(query(port, "root", *root, "SELECT 1").starts_with("ERROR"));
    CHECK(query(port, "root", "New-Root_pass9", "SELECT 1") == "1");
    CHECK(db.root_password("10.11", refusal) == std::optional<std::string>("New-Root_pass9"));
    CHECK_FALSE(secrets.read("mariadb-10.11/root-new"));
    // The next clean shutdown logs in with it: a stop leaves no process behind.
    const auto pid2 = db.series_status("10.11")["server"]["pid"].get<std::uint32_t>();
    REQUIRE(db.server_action("10.11", "stop", refusal));
    CHECK(eventually([&] { return db.series_status("10.11")["server"]["state"] == "sleeping"; }, 60s));
    CHECK(::kill(static_cast<pid_t>(pid2), 0) != 0);
    CHECK_FALSE(db.server_action("10.11", "jump", refusal));
    CHECK_FALSE(db.server_action("11.4", "start", refusal));
    CHECK(refusal.code == "database_off");
    // A connection wakes it with the new password, and the data is there.
    CHECK(query(port, "root", "New-Root_pass9", "SELECT x FROM blog.kept") == "7");

    // A port chosen in Settings: the server moves there.
    const auto other = pick_port_from(3400, {port}, cfg.probe);
    REQUIRE(other);
    const nlohmann::json chosen = *other;
    REQUIRE_MESSAGE(projects.database_set("10.11", nullptr, &chosen, nullptr, changed, refusal), refusal.message);
    CHECK_FALSE(changed);
    (void)db.reload();
    CHECK(eventually([&] { return query(*other, "root", "New-Root_pass9", "SELECT 1") == "1"; }, 60s));
    CHECK(server_status(db)["port"] == *other);
    db.stop();
}

#endif
