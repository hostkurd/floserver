// Remove FloServer… in Settings > General (src/ui-win32/model/uninstall.h, D21):
// the question's words, the folders it goes by, what's said after.

#include "model/uninstall.h"

#include "floserver/core/paths.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace floserver::ui;
namespace fs = std::filesystem;
using test_util::TempDir;
using test_util::write_file;

namespace {

const std::string kLri{'\xE2', '\x80', '\xAA'};
const std::string kPdi{'\xE2', '\x80', '\xAC'};

bool has(const std::vector<std::string>& list, const std::string& s) {
    return std::find(list.begin(), list.end(), s) != list.end();
}

}  // namespace

TEST_CASE("remove FloServer: the boxes as the mockup has them, and as they're posted") {
    const UninstallTicks start;
    CHECK(start.runtimes);
    CHECK_FALSE(start.databases);
    CHECK_FALSE(start.projects);
    for (unsigned bits = 0; bits < 8; ++bits) {
        CAPTURE(bits);
        CHECK(uninstall_ticks_bits(uninstall_ticks_from(bits)) == bits);
    }
    CHECK(uninstall_ticks_from(uninstall_ticks_bits({false, true, true})) == UninstallTicks{false, true, true});
    CHECK(uninstall_ticks_from(0) == UninstallTicks{false, false, false});
}

TEST_CASE("remove FloServer: the question's words, the sizes once they're counted") {
    const UninstallQuestion counting = uninstall_question(Lang::En, R"(C:\Users\Me\FloServer\projects)", {});
    CHECK(counting.title == "Remove FloServer from this computer?");
    CHECK(counting.text.starts_with("FloServer stops your projects and its services"));
    CHECK(counting.also_delete == "Also delete");
    CHECK(counting.runtimes == "Downloaded runtimes and caches");
    CHECK(counting.runtimes_note ==
          "PHP, MariaDB, Node.js, Python and the web servers · …. They can be downloaded again.");
    CHECK(counting.databases == "Databases");
    CHECK(counting.databases_note == "Every project's database · …. This can't be undone.");
    CHECK(counting.projects == "The projects folder");
    CHECK(counting.projects_note == R"(C:\Users\Me\FloServer\projects, moved to the Recycle Bin. )"
                                    "Projects you added from other folders are never touched.");
    CHECK(counting.remove == "Remove FloServer");
    CHECK(counting.cancel == "Cancel");

    const UninstallQuestion counted =
        uninstall_question(Lang::En, R"(C:\p)", {std::uint64_t{1503238553}, std::uint64_t{0}});
    CHECK(counted.runtimes_note.find(disk_size_text(1503238553)) != std::string::npos);
    CHECK(counted.databases_note.find(" · " + disk_size_text(0) + ".") != std::string::npos);

    // No projects folder (no home folder, a config.json that can't be read): not offered.
    const UninstallQuestion none = uninstall_question(Lang::En, "", {});
    CHECK(none.projects.empty());
    CHECK(none.projects_note.empty());

    // Arabic and Kurdish: the size and the path read left to right.
    const UninstallQuestion ar = uninstall_question(Lang::Ar, R"(C:\p)", {std::uint64_t{2048}, std::nullopt});
    CHECK(ar.runtimes_note.find(kLri + disk_size_text(2048) + kPdi) != std::string::npos);
    CHECK(ar.databases_note.find(kLri + "…" + kPdi) != std::string::npos);
    CHECK(ar.projects_note.find(kLri + R"(C:\p)" + kPdi) != std::string::npos);
    const UninstallQuestion ku = uninstall_question(Lang::Ku, R"(C:\p)", {});
    CHECK(ku.remove == std::string(text(Lang::Ku, Str::UninstallConfirm)));
    CHECK(ku.cancel == std::string(text(Lang::Ku, Str::WizardCancel)));
}

TEST_CASE("remove FloServer: the folders come from config.json") {
    const TempDir data;
    const TempDir home;
    const fs::path projects = home.path() / "Sites";
    const fs::path www = home.path() / "www";
    fs::create_directories(www);
    write_file(data.path() / "config.json",
               nlohmann::json{{"projects_dir", floserver::core::path_to_utf8(projects)},
                              {"projects",
                               {{{"name", "shop"}, {"path", floserver::core::path_to_utf8(projects / "shop")}},
                                {{"name", "blog"}, {"path", floserver::core::path_to_utf8(home.path() / "blog")}}}},
                              {"shared", {{"dir", floserver::core::path_to_utf8(www)}}}}
                   .dump());
    const UninstallFolders f = uninstall_folders(data.path());
    CHECK(f.projects_dir == floserver::core::path_to_utf8(fs::path(projects).make_preferred()));
    REQUIRE(f.project_dirs.size() == 2);
    CHECK(f.project_dirs[0] == floserver::core::path_to_utf8(fs::path(projects / "shop").make_preferred()));
    CHECK(f.shared_dir == floserver::core::path_to_utf8(fs::path(www).make_preferred()));

    // A config.json that can't be read: the projects folder isn't offered.
    write_file(data.path() / "config.json", "{ cut short");
    const UninstallFolders damaged = uninstall_folders(data.path());
    CHECK(damaged.projects_dir.empty());
    CHECK(damaged.project_dirs.empty());
}

TEST_CASE("remove FloServer: the projects folder's guard keeps everything but the projects folder") {
    FolderGuard base;
    base.kept = {R"(C:\Users\Me)", R"(C:\Users\Me\Documents)"};
    base.kept_with_contents = {R"(C:\Users\Me\AppData\Local\FloServer)"};
    base.system_trees = {R"(C:\Windows)"};
    base.other_projects = {R"(C:\Users\Me\FloServer\projects\shop)"};
    UninstallFolders folders;
    folders.projects_dir = R"(C:\Users\Me\FloServer\projects)";
    folders.project_dirs = {R"(C:\Users\Me\FloServer\projects\shop)", R"(D:\work\blog)", R"(C:\Users\Me\FloServer)"};
    folders.shared_dir = R"(C:\Users\Me\FloServer\www)";

    const FolderGuard g = projects_folder_guard(base, folders);
    CHECK(g.kept == std::vector<std::string>{R"(C:\Users\Me)", R"(C:\Users\Me\Documents)", R"(C:\Users\Me\FloServer\www)"});
    CHECK(g.kept_with_contents == base.kept_with_contents);
    CHECK(g.system_trees == base.system_trees);
    // The projects inside it go with it; the ones added from elsewhere are guarded.
    CHECK_FALSE(has(g.other_projects, R"(C:\Users\Me\FloServer\projects\shop)"));
    CHECK(has(g.other_projects, R"(D:\work\blog)"));
    CHECK(has(g.other_projects, R"(C:\Users\Me\FloServer)"));

    // So the folder itself may go, unless a project added from elsewhere holds it.
    CHECK(folder_removal_refusal(folders.projects_dir, {}, g) == FolderRefusal::OtherProject);
    folders.project_dirs.pop_back();
    const FolderGuard ok = projects_folder_guard(base, folders);
    CHECK(folder_removal_refusal(folders.projects_dir, {}, ok) == FolderRefusal::None);
    // A www folder inside it keeps it where it is.
    folders.shared_dir = R"(C:\Users\Me\FloServer\projects\www)";
    CHECK(folder_removal_refusal(folders.projects_dir, {}, projects_folder_guard(base, folders)) ==
          FolderRefusal::KeptFolder);
    // Never the profile or a folder Windows keeps, even when it's the one set.
    folders.shared_dir.clear();
    for (const char* dir : {R"(C:\Users\Me\Documents)", R"(C:\Users\Me)", R"(C:\Users\Me\AppData\Local\FloServer\p)"}) {
        CAPTURE(dir);
        folders.projects_dir = dir;
        CHECK(folder_removal_refusal(folders.projects_dir, {}, projects_folder_guard(base, folders)) !=
              FolderRefusal::None);
    }
}

TEST_CASE("remove FloServer: what's said after the clean-up") {
    UninstallReport all_went;
    all_went.projects = FolderOutcome::Recycled;
    CHECK(uninstall_problems(all_went, Lang::En).empty());
    CHECK(uninstall_done_text(all_went, true, Lang::En) == "Windows' uninstall opens next.");
    CHECK(uninstall_done_text(all_went, false, Lang::En) ==
          "The clean-up is done. FloServer's program files were left, because FloServer wasn't installed by its "
          "installer.");

    UninstallReport left;
    left.skipped = {{"runtimes", "in_use", "in use by another program", {}}};
    left.projects = FolderOutcome::Stayed;
    left.projects_reason = "The process cannot access the file";
    const auto problems = uninstall_problems(left, Lang::En);
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].starts_with("Some of what you chose stayed: runtimes ("));
    CHECK(problems[1] == "The projects folder stayed where it was: The process cannot access the file");
    CHECK(uninstall_done_text(left, true, Lang::En) == problems[0] + "\n" + problems[1] + "\n\nWindows' uninstall opens next.");

    left.skipped.clear();
    left.projects = FolderOutcome::Cancelled;
    CHECK(uninstall_problems(left, Lang::En) == std::vector<std::string>{"The projects folder stayed where it was."});
    left.projects = FolderOutcome::Deleted;
    CHECK(uninstall_problems(left, Lang::Ar) ==
          std::vector<std::string>{std::string(text(Lang::Ar, Str::UninstallProjectsDeleted))});
}

TEST_CASE("remove FloServer: only a product code goes on msiexec's command line") {
    CHECK(is_product_code("{3AC2E747-DDE1-465C-87CC-277C97C81D0F}"));
    CHECK(is_product_code("{3ac2e747-dde1-465c-87cc-277c97c81d0f}"));
    CHECK_FALSE(is_product_code("3AC2E747-DDE1-465C-87CC-277C97C81D0F"));
    CHECK_FALSE(is_product_code("{3AC2E747-DDE1-465C-87CC-277C97C81D0G}"));
    CHECK_FALSE(is_product_code("{3AC2E747-DDE1-465C-87CC-277C97C81D0F} /qn"));
    CHECK_FALSE(is_product_code("{3AC2E747DDE1-465C-87CC-277C97C81D0F0}"));
    CHECK_FALSE(is_product_code(""));
    CHECK(is_product_code(kUpgradeCode));
    CHECK(msiexec_uninstall_args("{3AC2E747-DDE1-465C-87CC-277C97C81D0F}") == "/x {3AC2E747-DDE1-465C-87CC-277C97C81D0F}");
    CHECK(msiexec_uninstall_args("{x} & calc").empty());
}

TEST_CASE("remove FloServer: every new string is there in every language") {
    for (const Str key : {Str::GeneralRemoveTitle, Str::GeneralRemoveNote, Str::GeneralRemoveButton, Str::UninstallTitle,
                          Str::UninstallText, Str::UninstallAlsoDelete, Str::UninstallRuntimes,
                          Str::UninstallRuntimesNote, Str::UninstallDatabases, Str::UninstallDatabasesNote,
                          Str::UninstallProjects, Str::UninstallProjectsNote, Str::UninstallConfirm,
                          Str::UninstallRemovingCa, Str::UninstallStopping, Str::UninstallCleaning, Str::UninstallCaNo,
                          Str::UninstallFailed, Str::UninstallStopFailed, Str::UninstallLeft,
                          Str::UninstallProjectsStayed, Str::UninstallProjectsCancelled, Str::UninstallProjectsDeleted,
                          Str::UninstallDevBuild, Str::UninstallNext, Str::UninstallStartFailed, Str::UninstallWait}) {
        CAPTURE(static_cast<int>(key));
        for (const Lang lang : {Lang::En, Lang::Ar, Lang::Ku}) {
            CHECK_FALSE(text(lang, key).empty());
        }
        // A value's place in every language when English has one.
        const bool en_has = text(Lang::En, key).find("{0}") != std::string_view::npos;
        CHECK((text(Lang::Ar, key).find("{0}") != std::string_view::npos) == en_has);
        CHECK((text(Lang::Ku, key).find("{0}") != std::string_view::npos) == en_has);
        // Arabic and Kurdish say it in their own words, not English's.
        CHECK(text(Lang::Ar, key) != text(Lang::En, key));
        CHECK(text(Lang::Ku, key) != text(Lang::En, key));
    }
}
