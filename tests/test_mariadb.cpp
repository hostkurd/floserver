#include "floserver/core/mariadb.h"
#include "floserver/core/secret_store.h"
#include "test_util.h"

#include <doctest/doctest.h>

using namespace floserver::core;
namespace fs = std::filesystem;

TEST_CASE("MariaDB series, files and config") {
    CHECK(mariadb_series("11.4.13") == "11.4");
    CHECK(mariadb_series("10.11") == "10.11");
    CHECK(mariadb_series("11").empty());
    CHECK(mariadb_series("x").empty());

    const test_util::TempDir tmp;
    test_util::write_file(tmp.path() / "inst" / "scripts" / "mariadb-install-db", "");
    const MariaDbFiles f = mariadb_files(tmp.path() / "inst", "11.4.13", tmp.path() / "data");
    CHECK(f.series == "11.4");
    CHECK(f.folder == tmp.path() / "data" / "data" / "mariadb-11.4");
    CHECK(f.data_dir == f.folder / "data");
#ifndef _WIN32
    CHECK(f.install_db == tmp.path() / "inst" / "scripts" / "mariadb-install-db");  // the bintar's place
#endif
    CHECK_FALSE(mariadb_initialized(f));
    fs::create_directories(f.data_dir / "mysql");
    CHECK_FALSE(mariadb_initialized(f));  // install-db didn't finish
    test_util::write_file(f.installed_marker, "1\n");
    CHECK(mariadb_initialized(f));

    const std::string c = mariadb_config(f, 33100, false, false);
    CHECK(c.find("port=33100") != std::string::npos);
    CHECK(c.find("bind-address=127.0.0.1") != std::string::npos);
    CHECK(c.find("datadir=\"" + f.data_dir.generic_string() + "\"") != std::string::npos);
    CHECK(c.find('\\') == std::string::npos);  // MariaDB reads \ as an escape
    CHECK(c.find("pid-file=mariadbd.pid") != std::string::npos);
    CHECK(c.find("init-file") == std::string::npos);
    CHECK(mariadb_config(f, 33100, false, true).find("init-file=init.sql") != std::string::npos);
#ifdef _WIN32
    CHECK(c.find("basedir") == std::string::npos);
#endif
    CHECK(c.find("user=root") == std::string::npos);
    CHECK(mariadb_config(f, 33100, true, false).find("user=root") != std::string::npos);
}

TEST_CASE("SQL is quoted and a project's database is its own") {
    CHECK(sql_identifier("my`db") == "`my``db`");
    CHECK(sql_string("it's \\ ok") == "'it''s \\\\ ok'");
    CHECK(project_database_name("my-shop") == "my_shop");
    CHECK(reserved_database_name("root"));
    CHECK(reserved_database_name(project_database_name("performance-schema")));
    CHECK(reserved_database_name("mysql"));
    CHECK_FALSE(reserved_database_name("shop"));
    const std::string sql = project_database_sql("my_shop", "my_shop", "Pw23");
    CHECK(sql.find("CREATE DATABASE IF NOT EXISTS `my_shop` CHARACTER SET utf8mb4") != std::string::npos);
    CHECK(sql.find("GRANT ALL PRIVILEGES ON `my_shop`.* TO 'my_shop'@'localhost';") != std::string::npos);
    CHECK(sql.find("GRANT ALL PRIVILEGES ON `my_shop`.* TO 'my_shop'@'127.0.0.1';") != std::string::npos);
    CHECK(sql.find("*.*") == std::string::npos);  // never every database
    const std::string root = root_password_sql("Pw23");
    CHECK(root.find("ALTER USER IF EXISTS 'root'@'localhost' IDENTIFIED BY 'Pw23';") != std::string::npos);
    CHECK(root.find("User = ''") != std::string::npos);  // no anonymous logins
    CHECK(root.find("Host NOT IN ('localhost', '127.0.0.1', '::1')") != std::string::npos);
}

TEST_CASE("Python projects get the database as one URL, encoded") {
    const DatabaseLogin login{3307, "my_shop", "my_shop", "a:b@c/d%e f"};
    CHECK(database_url(login) == "mysql+pymysql://my_shop:a%3Ab%40c%2Fd%25e%20f@127.0.0.1:3307/my_shop");
    CHECK(database_url(login, true) == "mysql://my_shop:a%3Ab%40c%2Fd%25e%20f@127.0.0.1:3307/my_shop");
}

TEST_CASE(".env lines follow the project's style and keep its other lines") {
    const test_util::TempDir tmp;
    CHECK(env_style(tmp.path()) == EnvStyle::FloCms);
    test_util::write_file(tmp.path() / ".env.example", "DB_CONNECTION=mysql\nDB_DATABASE=\n");
    CHECK(env_style(tmp.path()) == EnvStyle::Laravel);
    test_util::write_file(tmp.path() / ".env", "DB_TYPE=mysql\nDB_NAME=\n");
    CHECK(env_style(tmp.path()) == EnvStyle::FloCms);  // the real .env wins

    const DatabaseLogin login{3307, "shop", "shop", "Pw23"};
    const auto flocms = env_values(EnvStyle::FloCms, login);
    CHECK(flocms.at(0) == std::pair<std::string, std::string>{"DB_TYPE", "mysql"});
    CHECK(flocms.at(3) == std::pair<std::string, std::string>{"DB_NAME", "shop"});
    CHECK(env_values(EnvStyle::Laravel, login).at(3).first == "DB_DATABASE");

    const std::string before = "APP_NAME=Shop\r\n# DB_NAME=old comment\r\nDB_HOST=localhost\r\nDB_NAME=a\r\nDB_NAME=b\r\nAPP_KEY=x";
    const std::string after = update_env_text(before, flocms);
    CHECK(after ==
          "APP_NAME=Shop\r\n# DB_NAME=old comment\r\nDB_HOST=127.0.0.1\r\nDB_NAME=shop\r\nAPP_KEY=x\r\n"
          "DB_TYPE=mysql\r\nDB_PORT=3307\r\nDB_USERNAME=shop\r\nDB_PASSWORD=Pw23\r\n");
    CHECK(update_env_text("", {{"A", "1"}}) == "A=1\n");
    // Shell style lines are matched too, and replaced without "export".
    CHECK(update_env_text("export DB_HOST=x\r\nA=1\r\n", {{"DB_HOST", "127.0.0.1"}}) == "DB_HOST=127.0.0.1\r\nA=1\r\n");
}

TEST_CASE("secrets are kept and passwords are random letters and digits") {
    const test_util::TempDir tmp;
    const SecretStore store(tmp.path());
    std::string error;
    CHECK(SecretStore::valid_name("mariadb-11.4/root"));
    CHECK_FALSE(SecretStore::valid_name("../x"));
    CHECK_FALSE(SecretStore::valid_name("A"));
#ifndef _WIN32  // Credential Manager is the user's real store: not touched by tests
    CHECK_FALSE(store.read("test/one"));
    REQUIRE(store.save("test/one", "s3cret", error));
    CHECK(store.read("test/one") == "s3cret");
    const auto perms = fs::status(tmp.path() / "secrets" / "test" / "one").permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    CHECK(store.remove("test/one"));
    CHECK_FALSE(store.read("test/one"));
#endif
    const auto a = generate_password();
    const auto b = generate_password();
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->size() == 24);
    CHECK(*a != *b);
    CHECK(std::all_of(a->begin(), a->end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }));
}
