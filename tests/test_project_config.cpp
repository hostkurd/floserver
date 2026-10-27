#include "floserver/core/project_config.h"

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

TEST_CASE("writing and reading back gives the same config") {
    ProjectConfig c;
    c.php = "8.4";
    c.database = DatabaseSpec{"mariadb", "11.4"};
    c.web = WebServer::Apache;
    c.root = "public";
    c.extensions = {"intl", "pdo_mysql"};
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
