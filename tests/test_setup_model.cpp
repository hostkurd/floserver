// FloServer's setup and uninstall screens (D25): what the setup decides
// (src/setup-win32/model/setup_model.h), the feather's path data reader
// (svg_path.h), and the uninstall's notes to FloServer.exe's copy
// (src/ui-win32/model/uninstall.h).

#include "dll/feather_paths.h"
#include "model/general_page.h"
#include "model/setup_model.h"
#include "model/svg_path.h"
#include "model/uninstall.h"

#include "floserver/core/paths.h"

#include <doctest/doctest.h>

#include <string>

using namespace floserver::setup;
namespace ui = floserver::ui;

TEST_CASE("setup: versions compare part by part") {
    CHECK(compare_versions("0.1.0", "0.2.0") < 0);
    CHECK(compare_versions("0.10.0", "0.9.9") > 0);
    CHECK(compare_versions("1.2", "1.2.0") == 0);
    CHECK(compare_versions("1.2.0.1", "1.2.0") > 0);
    CHECK(compare_versions("x", "1.0.0") == 0);  // unreadable: no claim either way
    CHECK_FALSE(parse_version("1..2").has_value());
    CHECK_FALSE(parse_version("1.2.3.4.5").has_value());
    CHECK_FALSE(parse_version("").has_value());
    CHECK(parse_version("10.0.1") == std::vector<unsigned>{10, 0, 1});
}

TEST_CASE("setup: what it does depends on what's installed") {
    const Product ours{"{AAAAAAAA-0000-0000-0000-000000000002}", "0.2.0"};
    CHECK(decide_setup_mode(std::nullopt, ours) == SetupMode::Install);
    CHECK(decide_setup_mode(Product{"{AAAAAAAA-0000-0000-0000-000000000001}", "0.1.0"}, ours) == SetupMode::Update);
    // Another build of the same version replaces it (AllowSameVersionUpgrades).
    CHECK(decide_setup_mode(Product{"{AAAAAAAA-0000-0000-0000-000000000009}", "0.2.0"}, ours) == SetupMode::Update);
    CHECK(decide_setup_mode(ours, ours) == SetupMode::Reinstall);
    CHECK(decide_setup_mode(Product{"{AAAAAAAA-0000-0000-0000-000000000003}", "0.3.0"}, ours) == SetupMode::Newer);
}

TEST_CASE("setup: the licence again only when it changed") {
    CHECK(licence_needed(SetupMode::Install, "abc", "abc"));
    CHECK_FALSE(licence_needed(SetupMode::Update, "abc", "abc"));
    CHECK(licence_needed(SetupMode::Update, "abc", "def"));
    CHECK(licence_needed(SetupMode::Update, "", "def"));  // installed by the plain MSI: never accepted here
    CHECK_FALSE(licence_needed(SetupMode::Reinstall, "def", "def"));
}

TEST_CASE("setup: Windows never restarts by itself; a reinstall writes everything again") {
    CHECK(msi_properties(SetupMode::Install) == L"REBOOT=ReallySuppress");
    CHECK(msi_properties(SetupMode::Update) == L"REBOOT=ReallySuppress");
    CHECK(msi_properties(SetupMode::Reinstall) == L"REBOOT=ReallySuppress REINSTALL=ALL REINSTALLMODE=vamus");
}

TEST_CASE("setup: Windows Installer's actions move the steps") {
    CHECK(step_for_action("InstallValidate") == InstallStep::Checked);
    CHECK(step_for_action("RemoveExistingProducts") == InstallStep::Copying);
    CHECK(step_for_action("InstallFiles") == InstallStep::Copying);
    CHECK(step_for_action("WriteEnvironmentStrings") == InstallStep::Path);
    CHECK(step_for_action("CreateShortcuts") == InstallStep::StartMenu);
    CHECK_FALSE(step_for_action("CostFinalize").has_value());
    CHECK_FALSE(step_for_action("installfiles").has_value());
}

TEST_CASE("setup: sizes and arguments") {
    CHECK(format_size(10276044) == "9.8 MB");
    CHECK(format_size(1500) == "2 KB");
    CHECK(format_size(0) == "0 KB");
    CHECK(parse_setup_args({}).quiet == false);
    CHECK(parse_setup_args({"--quiet"}).quiet);
    CHECK(parse_setup_args({"/qn"}).quiet);
    const SetupArgs a = parse_setup_args({"/quiet", "--log", "C:\\setup.log"});
    CHECK(a.quiet);
    CHECK(a.log == "C:\\setup.log");
    CHECK(parse_setup_args({"--log"}).log.empty());
}

TEST_CASE("svg path: moves, lines and curves, absolute and relative") {
    const auto figures = parse_svg_path("M10,20 l5,0 h5 v5 H10 Z m1-1c1,2,3,4,5,6s1.5.5,2,2Z");
    REQUIRE(figures.has_value());
    REQUIRE(figures->size() == 2);
    const auto& a = (*figures)[0];
    CHECK(a.closed);
    REQUIRE(a.points.size() == 1 + (4 * 3));
    CHECK(a.points[0] == PathPoint{10, 20});
    CHECK(a.points[3] == PathPoint{15, 20});   // l5,0
    CHECK(a.points[6] == PathPoint{20, 20});   // h5
    CHECK(a.points[9] == PathPoint{20, 25});   // v5
    CHECK(a.points[12] == PathPoint{10, 25});  // H10
    const auto& b = (*figures)[1];
    // After Z the pen is back at (10,20); m1-1 moves from there.
    CHECK(b.points[0] == PathPoint{11, 19});
    REQUIRE(b.points.size() == 7);
    CHECK(b.points[1] == PathPoint{12, 21});
    CHECK(b.points[3] == PathPoint{16, 25});
    // S mirrors the last control (14,23) about (16,25): (18,27).
    CHECK(b.points[4] == PathPoint{18, 27});
    CHECK(b.points[5] == PathPoint{17.5, 25.5});
    CHECK(b.points[6] == PathPoint{18, 27});
}

TEST_CASE("svg path: SVG's compact numbers") {
    const auto figures = parse_svg_path("M.5-.5L1e1,2E-1z");
    REQUIRE(figures.has_value());
    CHECK((*figures)[0].points[0] == PathPoint{0.5, -0.5});
    CHECK((*figures)[0].points[3] == PathPoint{10, 0.2});
}

TEST_CASE("svg path: anything it doesn't know draws nothing") {
    CHECK_FALSE(parse_svg_path("").has_value());
    CHECK_FALSE(parse_svg_path("L1,2").has_value());           // no M first
    CHECK_FALSE(parse_svg_path("M0,0 A1,1 0 0 1 2,2").has_value());  // arcs aren't read
    CHECK_FALSE(parse_svg_path("M0,0 L1").has_value());       // half a point
    CHECK_FALSE(parse_svg_path("1,2").has_value());
    CHECK_FALSE(parse_svg_path("M0,0Z 1,2").has_value());     // numbers after Z
}

TEST_CASE("uninstall: the copy's notes go there and back, and only with a product code") {
    ui::RemoveProgramPlan plan;
    plan.product = "{3AC2E747-DDE1-465C-87CC-277C97C81D0F}";
    plan.parent = 4242;
    plan.x = -1200;
    plan.y = 80;
    plan.deleted = "Downloaded runtimes and caches were deleted too.";
    plan.kept = {"Databases: C:\\Users\\J\xC3\xB6rg\\AppData\\Local\\FloServer\\data"};
    plan.problems = {};
    plan.data_dir = "C:\\Users\\J\xC3\xB6rg\\AppData\\Local\\FloServer\\data";
    const auto back = ui::parse_remove_program_json(ui::remove_program_json(plan));
    REQUIRE(back.has_value());
    CHECK(*back == plan);

    CHECK_FALSE(ui::parse_remove_program_json("not json").has_value());
    plan.product = "{3AC2E747} & calc.exe";
    CHECK_FALSE(ui::parse_remove_program_json(ui::remove_program_json(plan)).has_value());
    CHECK_FALSE(ui::parse_remove_program_json(R"({"product":"{3AC2E747-DDE1-465C-87CC-277C97C81D0F}"})").has_value());
    plan.product = "{3AC2E747-DDE1-465C-87CC-277C97C81D0F}";
    std::string text = ui::remove_program_json(plan);
    text.replace(text.find("4242"), 4, "-1");  // not a process id
    CHECK_FALSE(ui::parse_remove_program_json(text).has_value());
}

TEST_CASE("uninstall: the Done page's words") {
    CHECK(ui::uninstall_deleted_text({true, false, false}, true) == "Downloaded runtimes and caches were deleted too.");
    CHECK(ui::uninstall_deleted_text({true, true, false}, true) ==
          "Downloaded runtimes and caches and databases were deleted too.");
    CHECK(ui::uninstall_deleted_text({true, true, true}, true) ==
          "Downloaded runtimes and caches, databases and the projects folder (to the Recycle Bin) were deleted too.");
    CHECK(ui::uninstall_deleted_text({false, false, true}, false) ==
          "Your projects, databases and downloaded runtimes are still on this computer.");
    const auto kept = ui::uninstall_kept_lines({true, false, false}, "D", "R", "P");
    CHECK(kept == std::vector<std::string>{"Databases: D", "Projects: P"});
    CHECK(ui::uninstall_kept_lines({true, true, true}, "D", "R", "P").empty());
    CHECK(ui::uninstall_kept_lines({false, true, true}, "D", "R", "").size() == 1);
}

TEST_CASE("uninstall: the copy's command line") {
    CHECK(ui::parse_remove_program_args({"--remove-program", "C:\\t\\plan.json"}) == "C:\\t\\plan.json");
    CHECK_FALSE(ui::parse_remove_program_args({"--remove-program"}).has_value());
    CHECK_FALSE(ui::parse_remove_program_args({"--tray"}).has_value());
    CHECK(ui::remove_program_command("C:\\t\\FloServer.exe", "C:\\t\\plan.json") ==
          "\"C:\\t\\FloServer.exe\" --remove-program \"C:\\t\\plan.json\"");
    CHECK(ui::remove_program_command("C:\\t\\Flo\"Server.exe", "x").empty());
    CHECK(ui::parse_start_args({"--uninstall"}).uninstall);
    CHECK_FALSE(ui::parse_start_args({"--tray"}).uninstall);
}

TEST_CASE("uninstall: the boxes, from the question's words") {
    const ui::UninstallQuestion q = ui::uninstall_question(ui::Lang::En, R"(C:\Users\me\Documents\FloServer)", {});
    const auto choices = ui::uninstall_choices(q, ui::UninstallTicks{});
    REQUIRE(choices.size() == 3);
    CHECK(choices[0].checked);
    CHECK_FALSE(choices[1].checked);
    CHECK_FALSE(choices[2].checked);
    CHECK(ui::uninstall_ticks_from(choices[0].bit | choices[2].bit) == ui::UninstallTicks{true, false, true});
    CHECK(ui::uninstall_choices(ui::uninstall_question(ui::Lang::En, "", {}), {}).size() == 2);
}

TEST_CASE("svg path: the app icon's feather, as the setup screens draw it") {
    const auto light = parse_svg_path(floserver::setup::look::kFeatherLight);
    const auto white = parse_svg_path(floserver::setup::look::kFeatherWhite);
    REQUIRE(light.has_value());
    REQUIRE(white.has_value());
    for (const auto* figures : {&*light, &*white}) {
        for (const PathFigure& f : *figures) {
            CHECK(f.closed);
            CHECK((f.points.size() - 1) % 3 == 0);
            for (const PathPoint& p : f.points) {
                CHECK(p.x > 150);  // inside the icon's 1254 square, clear of its edge
                CHECK(p.x < 1150);
                CHECK(p.y > 100);
                CHECK(p.y < 1200);
            }
        }
    }
}

TEST_CASE("uninstall: projects in the recycled projects folder leave the list, others stay") {
    floserver::core::AppConfig config;
    const auto project = [](const char* name, const char* dir) {
        floserver::core::Project p;
        p.name = name;
        p.dir = floserver::core::path_from_utf8(dir);
        return p;
    };
    // Windows' paths: the projects folder only exists on Windows for now.
    const std::string projects = R"(C:\Users\me\FloServer\projects)";
    config.projects = {project("nodeapp", R"(C:\Users\me\FloServer\Projects\nodeapp)"),
                       project("shop", R"(C:\work\shop)"),
                       project("api", R"(C:\Users\me\FloServer\projects\api)"),
                       project("other", R"(C:\Users\me\FloServer\projects-old\x)")};
    CHECK(ui::forget_projects_inside(config, projects) == 2);
    REQUIRE(config.projects.size() == 2);
    CHECK(config.projects[0].name == "shop");
    CHECK(config.projects[1].name == "other");  // a folder whose name only starts the same
    CHECK(ui::forget_projects_inside(config, "") == 0);
    CHECK(config.projects.size() == 2);
}
