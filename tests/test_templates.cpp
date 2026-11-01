#include "floserver/core/template_service.h"

#include "floserver/core/paths.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/templates.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace floserver::core;
using nlohmann::json;
namespace fs = std::filesystem;
using test_util::TempDir;

namespace {

json good_template() {
    return json::parse(R"({
        "id": "php-app", "order": 10,
        "title": {"en": "PHP app", "ar": "تطبيق PHP"},
        "description": {"en": "An empty PHP project"},
        "mark": "php", "mark_bg": "#E8EAF6", "mark_fg": "#3949AB",
        "chips": ["PHP", "nginx"],
        "config": {"php": "8.5", "root": "public"},
        "files": [{"path": "public/index.php", "text": "<?php echo 'hi';"}]
    })");
}

std::string problem_of(const json& j) {
    std::string problem;
    CHECK_FALSE(parse_template(j, problem));
    return problem;
}

void fake_install(const fs::path& data, const std::string& name, const std::string& version) {
    const fs::path dir = runtimes_dir(data) / name / version;
    test_util::write_file(dir / std::string(kRuntimeMarker),
                          json{{"schema", 1}, {"name", name}, {"version", version}, {"platform", std::string(current_platform())},
                               {"variant", name == "php" ? "nts" : ""}, {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
}

json wait_for(const TemplateService& service, std::int64_t id) {
    for (int i = 0; i < 400; ++i) {
        const auto job = service.job(id);
        REQUIRE(job);
        if ((*job)["state"] != "running") {
            return *job;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    FAIL("the job didn't finish");
    return {};
}

}  // namespace

TEST_CASE("a template is read by its rules") {
    std::string problem;
    const auto t = parse_template(good_template(), problem);
    REQUIRE(t);
    CHECK(t->id == "php-app");
    CHECK(t->title.at("ar") == "تطبيق PHP");
    CHECK(t->parsed.php == std::optional<std::string>("8.5"));
    CHECK(t->parsed.root == "public");
    REQUIRE(t->files.size() == 1);

    json j = good_template();
    j["id"] = "Bad";
    CHECK(problem_of(j) == "has no usable id");
    j = good_template();
    j["title"] = {{"ar", "x"}};
    CHECK(problem_of(j) == "has no English title");
    j = good_template();
    j["files"][0]["path"] = "../escape.php";
    CHECK(problem_of(j) == "has a bad file");
    for (const char* path : {"/abs.php", "a/./b", "public/con.txt", "a\\b", "x.", "a//b", "public/"}) {
        CAPTURE(path);
        j["files"][0]["path"] = path;
        CHECK(problem_of(j) == "has a bad file");
    }
    j = good_template();
    j["steps"] = json::array({{{"tool", "cmd"}, {"args", json::array({"/c", "del"})}}});
    CHECK(problem_of(j) == "has a bad step");
    j = good_template();
    j["config"]["command"] = "rm -rf /";
    CHECK(problem_of(j) == "has a config with a command");
    j = good_template();
    j["config"]["php"] = 8;
    CHECK(problem_of(j) == "has a config floserver.json doesn't accept");
    j = good_template();
    j["env"] = true;
    CHECK(problem_of(j) == "writes database settings but has no database");
    j = good_template();
    j["copy"] = "wordpress";
    CHECK(problem_of(j) == "copies a runtime it doesn't need");
    j = good_template();
    j["mark_bg"] = "red";
    CHECK(problem_of(j) == "has no usable mark_bg");

    std::vector<std::string> warnings;
    json second = good_template();
    second["id"] = "first";
    second["order"] = 1;
    const auto list = parse_templates(json::array({good_template(), second, good_template(), 5}), warnings);
    REQUIRE(list.size() == 2);
    CHECK(list[0].id == "first");  // card order
    CHECK(warnings.size() == 2);
}

TEST_CASE("step arguments get the folder and the name") {
    const TemplateStep step{"composer", {"create-project", "hostkurd/flocms", "{dir}", "--x={name}-{name}", "{nope}"}};
    CHECK(fill_step_args(step, "C:/Sites/shop", "shop") ==
          std::vector<std::string>{"create-project", "hostkurd/flocms", "C:/Sites/shop", "--x=shop-shop", "{nope}"});
    CHECK(is_template_tool("composer"));
    CHECK_FALSE(is_template_tool("cmd"));
}

TEST_CASE("wp-config.php gets the login and new salts") {
    const std::string sample = "define( 'DB_NAME', 'database_name_here' );\n"
                               "define( 'DB_USER', 'username_here' );\n"
                               "define( 'DB_PASSWORD', 'password_here' );\n"
                               "define( 'DB_HOST', 'localhost' );\n"
                               "define( 'AUTH_KEY',         'put your unique phrase here' );\n"
                               "define( 'SECURE_AUTH_KEY',  'put your unique phrase here' );\n";
    const DatabaseLogin login{3307, "shop", "shop", "Secret123"};
    const auto config = make_wp_config(sample, login);
    REQUIRE(config);
    CHECK(config->find("'DB_NAME', 'shop'") != std::string::npos);
    CHECK(config->find("'DB_PASSWORD', 'Secret123'") != std::string::npos);
    CHECK(config->find("'DB_HOST', '127.0.0.1:3307'") != std::string::npos);
    CHECK(config->find("put your unique phrase here") == std::string::npos);
    CHECK_FALSE(make_wp_config("<?php // nothing to fill\n", login));
}

TEST_CASE("a template job makes the folder, installs what's missing, adds the project and fills .env") {
    const TempDir tmp;
    const fs::path data = tmp.path() / "data";
    const fs::path sites = tmp.path() / "Sites";
    fs::create_directories(sites);
    fake_install(data, "nginx", "1.30.6");

    json tj = good_template();
    tj["config"]["database"] = "mariadb-11.4";
    tj["files"].push_back({{"path", ".env.example"}, {"text", "APP_NAME=x\nDB_TYPE=sqlite\n"}});
    tj["env"] = true;
    std::string problem;
    const auto t = parse_template(tj, problem);
    REQUIRE(t);

    std::vector<std::string> installed;
    std::vector<std::string> added;
    std::atomic<bool> hold{false};  // the job waits in its first install while set
    TemplateHooks hooks;
    hooks.find_template = [&](const std::string& id, std::string& error) -> std::optional<ProjectTemplate> {
        if (id == t->id) {
            ProjectTemplate copy = *t;
            return copy;
        }
        error = "no such template";
        return std::nullopt;
    };
    hooks.install = [&](const std::string& name, const std::string& version, const std::atomic<bool>&,
                        const std::function<void(const std::string&)>& progress, std::string&) {
        progress("Downloading " + name);
        while (hold) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        installed.push_back(name + " " + version);
        fake_install(data, name, name == "php" ? "8.4.27" : version + ".13");
        return true;
    };
    hooks.add_project = [&](const fs::path& dir, const std::string& name, std::string&) -> std::optional<std::string> {
        added.push_back(path_to_utf8(dir));
        return name;
    };
    hooks.database_login = [](const std::string& project, std::string&) -> std::optional<DatabaseLogin> {
        return DatabaseLogin{3306, project, project, "pw"};
    };

    TemplateService service(data, hooks);
    ServiceRefusal refusal;

    SUBCASE("made") {
        const CreateRequest request{"php-app", "shop", sites, {{"php", "8.4"}}};
        const auto id = service.create(request, refusal);
        REQUIRE(id);
        const json job = wait_for(service, *id);
        INFO(job.dump(2));
        CHECK(job["state"] == "done");
        CHECK(job["project"] == "shop");
        CHECK_FALSE(service.cancel(*id));  // finished: nothing to cancel
        CHECK(installed == std::vector<std::string>{"php 8.4", "mariadb 11.4"});  // nginx was there
        REQUIRE(added.size() == 1);
        CHECK(fs::equivalent(path_from_utf8(added[0]), sites / "shop"));
        CHECK(read_small_file(sites / "shop" / "public" / "index.php", 1000) == std::optional<std::string>("<?php echo 'hi';"));
        const auto config = read_small_file(sites / "shop" / "floserver.json", 1000);
        REQUIRE(config);
        CHECK(config->find("\"php\": \"8.4\"") != std::string::npos);
        CHECK(config->find("\"root\": \"public\"") != std::string::npos);
        const auto env = read_small_file(sites / "shop" / ".env", 1000);
        REQUIRE(env);
        CHECK(env->find("APP_NAME=x") != std::string::npos);  // started from .env.example
        CHECK(env->find("DB_NAME=shop") != std::string::npos);
        CHECK(env->find("DB_TYPE=mysql") != std::string::npos);
        const bool work_cleared = !fs::exists(data / "templates" / "job-1");
        CHECK(work_cleared);
    }
    SUBCASE("refused before anything is made") {
        CHECK_FALSE(service.create({"php-app", "Bad Name", sites, {}}, refusal));
        CHECK(refusal.code == "bad_name");
        CHECK_FALSE(service.create({"php-app", "shop", "relative/folder", {}}, refusal));
        CHECK(refusal.code == "bad_folder");
        test_util::write_file(sites / "taken" / "keep.txt", "mine");
        CHECK_FALSE(service.create({"php-app", "taken", sites, {}}, refusal));
        CHECK(refusal.code == "folder_exists");
        CHECK_FALSE(service.create({"php-app", "shop", sites, {{"root", "/"}}}, refusal));
        CHECK(refusal.code == "bad_params");
        // A link where the folder would go: never followed.
        fs::create_directories(tmp.path() / "elsewhere");
        std::error_code ec;
        fs::create_directory_symlink(tmp.path() / "elsewhere", sites / "linked", ec);
        if (!ec) {
            CHECK_FALSE(service.create({"php-app", "linked", sites, {}}, refusal));
            CHECK(refusal.code == "folder_exists");
        }
    }
    SUBCASE("the leftover clean-up and a new project hold each other off") {
        // While a clean-up runs, no project is started (its work folder would be a leftover).
        REQUIRE(service.begin_cleanup());
        CHECK_FALSE(service.begin_cleanup());
        CHECK_FALSE(service.create({"php-app", "shop", sites, {{"php", "8.4"}}}, refusal));
        CHECK(refusal.code == "busy");
        CHECK(refusal.message.find("leftover downloads") != std::string::npos);
        service.end_cleanup();
        // While one is being made, no clean-up.
        hold = true;
        const auto id = service.create({"php-app", "shop", sites, {{"php", "8.4"}}}, refusal);
        REQUIRE(id);
        CHECK(service.busy());
        CHECK_FALSE(service.begin_cleanup());
        hold = false;
        CHECK(wait_for(service, *id)["state"] == "done");
        CHECK_FALSE(service.busy());
        REQUIRE(service.begin_cleanup());
        service.end_cleanup();
    }
    SUBCASE("an unknown template fails the job") {
        const auto id = service.create({"nope", "shop", sites, {}}, refusal);
        REQUIRE(id);
        const json job = wait_for(service, *id);
        CHECK(job["state"] == "failed");
        CHECK(job["error"] == "no such template");
        CHECK_FALSE(fs::exists(sites / "shop"));
    }
}

TEST_CASE("a step whose runtime is missing fails and leaves the folder") {
    const TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_install(data, "nginx", "1.30.6");
    json tj = good_template();
    tj["config"] = {{"node", "24"}};
    tj["files"] = json::array();
    tj["steps"] = json::array({{{"tool", "npm"}, {"args", json::array({"install"})}}});
    std::string problem;
    const auto t = parse_template(tj, problem);
    REQUIRE(t);
    TemplateHooks hooks;
    hooks.find_template = [&](const std::string&, std::string&) -> std::optional<ProjectTemplate> {
        ProjectTemplate copy = *t;
        return copy;
    };
    // The install "works" but puts nothing usable there, so npm can't be found.
    hooks.install = [&](const std::string& name, const std::string& version, const std::atomic<bool>&,
                        const std::function<void(const std::string&)>&, std::string&) {
        fake_install(data, name, version + ".0.0");
        return true;
    };
    TemplateService service(data, hooks);
    ServiceRefusal refusal;
    const auto id = service.create({"x", "web", tmp.path(), {}}, refusal);
    REQUIRE(id);
    const json job = wait_for(service, *id);
    INFO(job.dump(2));
    CHECK(job["state"] == "failed");
    CHECK(job["error"].get<std::string>().find("is left as it is") != std::string::npos);
    CHECK(fs::is_directory(tmp.path() / "web"));
}
