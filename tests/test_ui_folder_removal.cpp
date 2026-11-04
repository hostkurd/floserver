// Which project folders may go to the Recycle Bin with the project
// (src/ui-win32/model/folder_removal.h, approved 4 Nov).

#include "model/folder_removal.h"

#include <doctest/doctest.h>

using namespace floserver::ui;

namespace {

FolderGuard guard() {
    FolderGuard g;
    g.kept = {R"(C:\Users\Me)", R"(C:\Users\Me\Desktop)", R"(C:\Users\Me\Documents)", R"(C:\Users\Me\Downloads)",
              R"(C:\Users\Me\FloServer Projects)", R"(C:\Users\Me\www)"};
    g.kept_with_contents = {R"(C:\Users\Me\AppData\Local\FloServer)"};
    g.other_projects = {R"(D:\sites\blog)", R"(C:\Users\Me\FloServer Projects\shop)"};
    return g;
}

}  // namespace

TEST_CASE("folder removal: paths made comparable") {
    CHECK(comparable_path(R"(C:\Users\Me\Sites\Shop)") == R"(c:\users\me\sites\shop)");
    CHECK(comparable_path(R"(c:/users//me/./sites/shop/)") == R"(c:\users\me\sites\shop)");
    CHECK(comparable_path(R"(\\?\C:\Users\Me\x\..\Shop)") == R"(c:\users\me\shop)");
    CHECK(comparable_path(R"(\\?\UNC\nas\work\shop)") == R"(\\nas\work\shop)");
    CHECK(comparable_path(R"(\\NAS\Work\)") == R"(\\nas\work)");
    CHECK(comparable_path(R"(C:\)") == R"(c:\)");
    CHECK(comparable_path(R"(C:\..\..)") == R"(c:\)");
    CHECK(comparable_path("C:") == "");
    CHECK(comparable_path(R"(sites\shop)") == "");
    CHECK(comparable_path(R"(\sites\shop)") == "");
    CHECK(comparable_path(R"(\\nas)") == "");
    CHECK(comparable_path(R"(\\.\C:\x)") == "");
    CHECK(comparable_path("") == "");

    CHECK(same_or_inside(R"(c:\a\b)", R"(c:\a)"));
    CHECK(same_or_inside(R"(c:\a)", R"(c:\a)"));
    CHECK(same_or_inside(R"(c:\a)", R"(c:\)"));
    CHECK_FALSE(same_or_inside(R"(c:\ab)", R"(c:\a)"));
    CHECK_FALSE(same_or_inside(R"(c:\a)", R"(c:\a\b)"));
    CHECK_FALSE(same_or_inside("", R"(c:\a)"));
}

TEST_CASE("folder removal: an ordinary project folder may go") {
    const FolderGuard g = guard();
    CHECK(folder_removal_refusal(R"(C:\Users\Me\FloServer Projects\my-shop)", {}, g) == FolderRefusal::None);
    CHECK(folder_removal_refusal(R"(C:\Users\Me\Documents\work\site)", {}, g) == FolderRefusal::None);
    CHECK(folder_removal_refusal(R"(C:\Users\Me\www\old)", {}, g) == FolderRefusal::None);
    CHECK(folder_removal_refusal(R"(\\nas\work\site)", {}, g) == FolderRefusal::None);
}

TEST_CASE("folder removal: what never goes") {
    const FolderGuard g = guard();
    CHECK(folder_removal_refusal("", {}, g) == FolderRefusal::NotFullPath);
    CHECK(folder_removal_refusal("shop", {}, g) == FolderRefusal::NotFullPath);
    CHECK(folder_removal_refusal(R"(C:\)", {}, g) == FolderRefusal::DriveRoot);
    CHECK(folder_removal_refusal(R"(D:\.)", {}, g) == FolderRefusal::DriveRoot);
    CHECK(folder_removal_refusal(R"(\\nas\work)", {}, g) == FolderRefusal::DriveRoot);
    CHECK(folder_removal_refusal(R"(\\?\D:\)", {}, g) == FolderRefusal::DriveRoot);
    // Windows' own folders and FloServer's, written any way, nor one holding them.
    CHECK(folder_removal_refusal(R"(c:\users\me\desktop\)", {}, g) == FolderRefusal::KeptFolder);
    CHECK(folder_removal_refusal(R"(C:/Users/ME/Downloads)", {}, g) == FolderRefusal::KeptFolder);
    CHECK(folder_removal_refusal(R"(\\?\C:\Users\Me\Documents)", {}, g) == FolderRefusal::KeptFolder);
    CHECK(folder_removal_refusal(R"(C:\Users\Me)", {}, g) == FolderRefusal::KeptFolder);
    CHECK(folder_removal_refusal(R"(C:\Users)", {}, g) == FolderRefusal::KeptFolder);
    CHECK(folder_removal_refusal(R"(C:\Users\Me\FloServer Projects)", {}, g) == FolderRefusal::KeptFolder);
    CHECK(folder_removal_refusal(R"(C:\Users\Me\www)", {}, g) == FolderRefusal::KeptFolder);
    CHECK(folder_removal_refusal(R"(C:\Users\Me\AppData\Local\FloServer)", {}, g) == FolderRefusal::DataFolder);
    CHECK(folder_removal_refusal(R"(C:\Users\Me\AppData\Local\FloServer\runtimes\php)", {}, g) ==
          FolderRefusal::DataFolder);
    CHECK(folder_removal_refusal(R"(C:\Users\Me\AppData\Local)", {}, g) == FolderRefusal::DataFolder);
    // Another project: it, inside it, or holding it.
    CHECK(folder_removal_refusal(R"(D:\sites\blog)", {}, g) == FolderRefusal::OtherProject);
    CHECK(folder_removal_refusal(R"(D:\sites\blog\wp-content)", {}, g) == FolderRefusal::OtherProject);
    CHECK(folder_removal_refusal(R"(D:\sites)", {}, g) == FolderRefusal::OtherProject);
    CHECK(folder_removal_refusal(R"(D:\sites\blogs)", {}, g) == FolderRefusal::None);
}

TEST_CASE("folder removal: where it really is counts too") {
    const FolderGuard g = guard();
    // A folder reached through a junction above it that is really Documents.
    CHECK(folder_removal_refusal(R"(E:\link\docs)", R"(\\?\C:\Users\Me\Documents)", g) == FolderRefusal::KeptFolder);
    CHECK(folder_removal_refusal(R"(E:\link\x)", R"(\\?\D:\sites\blog\x)", g) == FolderRefusal::OtherProject);
    CHECK(folder_removal_refusal(R"(E:\link\x)", R"(\\?\E:\real\x)", g) == FolderRefusal::None);
    CHECK(folder_removal_refusal(R"(E:\link\x)", "not a path", g) == FolderRefusal::NotFullPath);
}
