#include "floserver/core/project_config.h"

#include "floserver/core/paths.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <random>

using namespace floserver::core;
namespace fs = std::filesystem;

namespace {

bool has_error(const ConfigResult& r, const std::string& key) {
    for (const auto& e : r.errors) {
        if (e.key == key) {
            return true;
        }
    }
    return false;
}

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = fs::temp_directory_path() / ("floserver-test-" + std::to_string(rd()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace

TEST_CASE("the example from the design note parses") {
    const auto r = parse_project_config(R"({
        "php": "8.2", "node": "22", "database": "mariadb-11.4",
        "web": "nginx", "root": "public", "extensions": ["intl", "gd"]
    })");
    REQUIRE(r.errors.empty());
    CHECK(r.warnings.empty());
    REQUIRE(r.config.has_value());
    const auto& c = *r.config;
    CHECK(c.php == "8.2");
    CHECK(c.node == "22");
    REQUIRE(c.database.has_value());
    CHECK(c.database->family == "mariadb");
    CHECK(c.database->version == "11.4");
    CHECK(c.web == WebServer::Nginx);
    CHECK(c.root == "public");
    CHECK(c.extensions == std::vector<std::string>{"intl", "gd"});
}

TEST_CASE("an empty object gives the defaults") {
    const auto r = parse_project_config("{}");
    REQUIRE(r.config.has_value());
    CHECK(*r.config == ProjectConfig{});
    CHECK(r.config->root == ".");
}

TEST_CASE("null means not set") {
    const auto r = parse_project_config(R"({"php": null, "database": null, "root": null})");
    REQUIRE(r.config.has_value());
    CHECK_FALSE(r.config->php.has_value());
    CHECK_FALSE(r.config->database.has_value());
}

TEST_CASE("apache can be chosen per project") {
    const auto r = parse_project_config(R"({"web": "apache"})");
    REQUIRE(r.config.has_value());
    CHECK(r.config->web == WebServer::Apache);
}

TEST_CASE("broken files are reported, not thrown") {
    for (const char* text : {"", "{", "[1,2]", "\"php\"", "{\"php\": 8.2,}", "nul"}) {
        CAPTURE(text);
        const auto r = parse_project_config(text);
        CHECK_FALSE(r.config.has_value());
        CHECK(has_error(r, ""));
    }
}

TEST_CASE("bad values are errors on their own key") {
    CHECK(has_error(parse_project_config(R"({"php": 8.2})"), "php"));
    CHECK(has_error(parse_project_config(R"({"php": "8.x"})"), "php"));
    CHECK(has_error(parse_project_config(R"({"node": "lts"})"), "node"));
    CHECK(has_error(parse_project_config(R"({"web": "caddy"})"), "web"));
    CHECK(has_error(parse_project_config(R"({"database": "mariadb"})"), "database"));
    CHECK(has_error(parse_project_config(R"({"database": "mariadb-latest"})"), "database"));
    CHECK(has_error(parse_project_config(R"({"extensions": "gd"})"), "extensions"));
    CHECK(has_error(parse_project_config(R"({"extensions": ["gd", "GD"]})"), "extensions[1]"));
    CHECK(has_error(parse_project_config(R"({"extensions": ["a b"]})"), "extensions[0]"));
}

TEST_CASE("MySQL is refused with a pointer to MariaDB (Q7)") {
    for (const char* text : {R"({"database": "mysql-8.0"})", R"({"database": "mysql"})"}) {
        CAPTURE(text);
        const auto r = parse_project_config(text);
        REQUIRE(has_error(r, "database"));
        CHECK(r.errors[0].message.find("use MariaDB") != std::string::npos);
    }
}

TEST_CASE("names and versions are tidied") {
    const auto r = parse_project_config(R"({"php": "08.02", "database": "MariaDB-011.4"})");
    REQUIRE(r.config.has_value());
    CHECK(r.config->php == "8.2");
    REQUIRE(r.config->database.has_value());
    CHECK(r.config->database->family == "mariadb");
    CHECK(r.config->database->version == "11.4");
}

TEST_CASE("the document root must stay inside the project") {
    const std::vector<std::string> bad_roots = {
          "/var/www", "../other", "public/../../x", "a/../public", "C:/www", "c:www",
          R"(\\server\share)", "//server/share", R"(\abs)", "public:x",
          // Windows drops trailing dots and spaces: ".. " is "..".
          ".. ", ".. /.. /etc", "public.", "public ", " public", "...",
          // Device names, with or without an extension.
          "CON", "nul", "public/aux", "Com1", "lpt9.txt",
          // Characters that don't belong in a folder name or a server config.
          "a<b", "a|b", "a?b", "a*b", "a\"b", "a;b", "a'b", "a`b", "~", "a\tb",
          std::string("a\0b", 3), "a\nb", "\u00e9t\u00e9", "public\uff0f..\uff0fx"};
    for (const std::string& bad : bad_roots) {
        CAPTURE(bad);
        const std::string text = R"({"root": )" + nlohmann::json(bad).dump() + "}";
        CHECK(has_error(parse_project_config(text), "root"));
    }
}

TEST_CASE("document roots are tidied") {
    const auto root = [](const char* raw) {
        const std::string text = std::string(R"({"root": ")") + raw + "\"}";
        const auto r = parse_project_config(text);
        REQUIRE(r.config.has_value());
        return r.config->root;
    };
    CHECK(root("public") == "public");
    CHECK(root("public/") == "public");
    CHECK(root("./web/public") == "web/public");
    CHECK(root("web\\\\public") == "web/public");
    CHECK(root("public//dist") == "public/dist");
    CHECK(root("my site/web-root_1") == "my site/web-root_1");
    CHECK(root("console") == "console");  // not a device name
    CHECK(root(".well-known") == ".well-known");
    CHECK(root("") == ".");
    CHECK(root(".") == ".");
}

TEST_CASE("unknown keys and duplicate extensions are warnings") {
    const auto r = parse_project_config(R"({"php": "8.2", "deploy": {}, "extensions": ["gd", "gd"]})");
    REQUIRE(r.config.has_value());
    CHECK(r.warnings.size() == 2);
    CHECK(r.config->extensions == std::vector<std::string>{"gd"});
}

TEST_CASE("python is a version like the others (D17)") {
    const auto r = parse_project_config(R"({"python": "3.13"})");
    REQUIRE(r.config.has_value());
    CHECK(r.config->python == "3.13");
    CHECK(r.warnings.empty());
    CHECK_FALSE(parse_project_config(R"({"python": 3.13})").config.has_value());
}

TEST_CASE("a start command is one line of text") {
    const auto ok = parse_project_config(R"({"node": "22", "command": "npm run serve -- --port {port}"})");
    REQUIRE(ok.config.has_value());
    CHECK(ok.config->command == "npm run serve -- --port {port}");
    for (const char* bad : {R"({"command": ""})", R"({"command": ["npm"]})", R"({"command": "a\nb"})",
                            R"({"command": "a\rb"})"}) {
        CAPTURE(bad);
        const auto r = parse_project_config(bad);
        CHECK_FALSE(r.config.has_value());
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].key == "command");
    }
    CHECK_FALSE(parse_project_config(R"({"command": ")" + std::string(1001, 'x') + R"("})").config.has_value());
}

TEST_CASE("writing and reading back gives the same config") {
    ProjectConfig c;
    c.php = "8.4";
    c.database = DatabaseSpec{"mariadb", "11.4"};
    c.web = WebServer::Apache;
    c.root = "public";
    c.extensions = {"intl", "pdo_mysql"};
    c.command = "php -S 127.0.0.1:{port}";
    c.python = "3.13";
    const std::string text = write_project_config(c);
    CHECK(text.find("\"php\": \"8.4\"") != std::string::npos);
    CHECK(text.find("node") == std::string::npos);  // unset keys are left out
    const auto r = parse_project_config(text);
    REQUIRE(r.config.has_value());
    CHECK(*r.config == c);
    CHECK(write_project_config(ProjectConfig{}) == "{}\n");
}

TEST_CASE("find_project_config walks up to the nearest file") {
    const TempDir tmp;
    const fs::path project = tmp.path() / "shop";
    const fs::path deep = project / "src" / "Controllers";
    fs::create_directories(deep);

    // Nothing inside the temp folder yet. (A floserver.json above the temp
    // folder on this machine may be found; it just mustn't be one of ours.)
    const auto none = find_project_config(deep);
    if (none) {
        CHECK(none->string().find(tmp.path().string()) == std::string::npos);
    }

    std::ofstream(project / "floserver.json") << "{}";
    const auto found = find_project_config(deep);
    REQUIRE(found.has_value());
    CHECK(fs::equivalent(*found, project / "floserver.json"));

    // A folder named floserver.json is not a project file.
    fs::create_directories(deep / "floserver.json");
    const auto again = find_project_config(deep);
    REQUIRE(again.has_value());
    CHECK(fs::equivalent(*again, project / "floserver.json"));
}

TEST_CASE("php can be false (never PHP) or \"newest\" (always PHP)") {
    auto r = parse_project_config(R"({"php": false})");
    REQUIRE(r.config);
    CHECK_FALSE(r.config->php);
    CHECK(r.config->php_choice == PhpChoice::Off);
    CHECK(php_off(*r.config));
    CHECK_FALSE(php_asked(*r.config));
    CHECK(write_project_config(*r.config) == "{\n  \"php\": false\n}\n");

    r = parse_project_config(R"({"php": "newest"})");
    REQUIRE(r.config);
    CHECK_FALSE(r.config->php);
    CHECK(r.config->php_choice == PhpChoice::Newest);
    CHECK(php_asked(*r.config));
    CHECK_FALSE(php_off(*r.config));
    CHECK(write_project_config(*r.config) == "{\n  \"php\": \"newest\"\n}\n");

    r = parse_project_config(R"({"php": "8.4"})");
    REQUIRE(r.config);
    CHECK(r.config->php_choice == PhpChoice::Auto);
    CHECK(php_asked(*r.config));

    r = parse_project_config("{}");
    REQUIRE(r.config);
    CHECK_FALSE(php_asked(*r.config));
    CHECK_FALSE(php_off(*r.config));

    for (const char* bad : {R"({"php": true})", R"({"php": "latest"})", R"({"php": 8})", R"({"php": "none"})"}) {
        r = parse_project_config(bad);
        CHECK_FALSE(r.config);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].key == "php");
        CHECK(r.errors[0].message.find("false") != std::string::npos);
    }
}

TEST_CASE("one key of floserver.json is set or removed, the rest kept in order") {
    std::string error;
    const std::string text = R"({"root": "public", "php": "8.2", "extensions": ["gd"], "x-team": 1})";

    SUBCASE("a version") {
        const auto out = set_project_config_value(text, ConfigKey::Php, "php", std::string("8.4.27"), error);
        REQUIRE(out);
        CHECK(*out == "{\n  \"root\": \"public\",\n  \"php\": \"8.4.27\",\n  \"extensions\": [\n    \"gd\"\n  ],\n"
                      "  \"x-team\": 1\n}\n");
    }
    SUBCASE("a new key goes last") {
        const auto out = set_project_config_value(text, ConfigKey::Node, "node", std::string("22"), error);
        REQUIRE(out);
        CHECK(out->find("\"x-team\": 1,\n  \"node\": \"22\"\n}") != std::string::npos);
    }
    SUBCASE("removed") {
        const auto out = set_project_config_value(text, ConfigKey::Php, "php", std::nullopt, error);
        REQUIRE(out);
        CHECK(out->find("php") == std::string::npos);
        CHECK(out->find("x-team") != std::string::npos);
    }
    SUBCASE("database, both ways") {
        auto out = set_project_config_value(text, ConfigKey::Database, "database", std::string("mariadb-11.4"), error);
        REQUIRE(out);
        CHECK(out->find("\"database\": \"mariadb-11.4\"") != std::string::npos);
        out = set_project_config_value(text, ConfigKey::Database, "mariadb", std::string("11.8"), error);
        REQUIRE(out);
        CHECK(out->find("\"database\": \"mariadb-11.8\"") != std::string::npos);
    }
    SUBCASE("web server: nginx is the default, so the key goes") {
        auto out = set_project_config_value(text, ConfigKey::Web, "web", std::string("apache"), error);
        REQUIRE(out);
        CHECK(out->find("\"web\": \"apache\"") != std::string::npos);
        out = set_project_config_value(*out, ConfigKey::Web, "web", std::string("nginx"), error);
        REQUIRE(out);
        CHECK(out->find("web") == std::string::npos);
    }
    SUBCASE("php none writes false, newest stays newest") {
        auto out = set_project_config_value(text, ConfigKey::Php, "php", std::string("none"), error);
        REQUIRE(out);
        CHECK(out->find("\"php\": false,") != std::string::npos);
        out = set_project_config_value(*out, ConfigKey::Php, "php", std::string("newest"), error);
        REQUIRE(out);
        CHECK(out->find("\"php\": \"newest\",") != std::string::npos);
        CHECK(out->find("x-team") != std::string::npos);
        CHECK_FALSE(set_project_config_value(text, ConfigKey::Node, "node", std::string("newest"), error));
    }
    SUBCASE("an empty file") {
        const auto out = set_project_config_value("", ConfigKey::Python, "python", std::string("3.13"), error);
        REQUIRE(out);
        CHECK(*out == "{\n  \"python\": \"3.13\"\n}\n");
    }
    SUBCASE("refused") {
        CHECK_FALSE(set_project_config_value(text, ConfigKey::Php, "php", std::string("latest"), error));
        CHECK(error.find("not a version") != std::string::npos);
        CHECK_FALSE(set_project_config_value(text, ConfigKey::Web, "web", std::string("iis"), error));
        CHECK_FALSE(set_project_config_value(text, ConfigKey::Database, "database", std::string("oracle-1"), error));
        CHECK(error.starts_with("floserver.json: database"));
        CHECK_FALSE(set_project_config_value("[1, 2]", ConfigKey::Php, "php", std::string("8.4"), error));
        CHECK_FALSE(set_project_config_value("{\"php\": ", ConfigKey::Php, "php", std::string("8.4"), error));
        CHECK(error.find("isn't valid JSON") != std::string::npos);
    }
    CHECK(parse_config_key("mariadb") == ConfigKey::Database);
    CHECK_FALSE(parse_config_key("root"));
}

TEST_CASE("setting a key writes the project's own floserver.json") {
    const TempDir dir;
    std::string error;
    REQUIRE(set_project_config_file_value(dir.path(), ConfigKey::Php, "php", std::string("8.4"), error));
    const auto written = read_small_file(dir.path() / "floserver.json", 4096);
    CHECK(written == std::optional<std::string>("{\n  \"php\": \"8.4\"\n}\n"));
    // A folder where the file should be isn't replaced.
    const TempDir other;
    fs::create_directories(other.path() / "floserver.json");
    CHECK_FALSE(set_project_config_file_value(other.path(), ConfigKey::Php, "php", std::string("8.4"), error));
    CHECK(error.find("plain file") != std::string::npos);
}
