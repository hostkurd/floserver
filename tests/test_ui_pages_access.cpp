// What screen readers are told about New project and the first-run page (src/ui-win32/model/pages_access.h).

#include "model/access_tree.h"
#include "model/pages_access.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>

using namespace floserver::ui;

namespace {

std::optional<AccessNews> news_of(const std::vector<AccessNews>& news, const std::string& key) {
    const auto it = std::find_if(news.begin(), news.end(), [&](const AccessNews& n) { return n.key == key; });
    return it != news.end() ? std::optional<AccessNews>(*it) : std::nullopt;
}

FirstRunRow row_of(std::string name, std::string label, FirstRunRowState state, std::string line) {
    FirstRunRow r;
    r.name = std::move(name);
    r.label = std::move(label);
    r.state = state;
    r.line = std::move(line);
    return r;
}

}  // namespace

TEST_CASE("pages access: news is said once, and again only when it reads otherwise") {
    AccessTree before;
    before.news = {{"step", "Step 1 of 3, Language", false}};
    AccessTree after;
    after.news = {{"step", "Step 1 of 3, Language", false}, {"trust", "", false}};
    CHECK(access_news(before, after).empty());  // the same, and nothing to say
    after.news = {{"step", "Step 2 of 3, Runtimes", false}, {"downloads", "Downloads finished", false}};
    const auto said = access_news(before, after);
    REQUIRE(said.size() == 2);
    CHECK(said[0] == AccessNews{"step", "Step 2 of 3, Runtimes", false});
    CHECK(said[1].key == "downloads");
    // Gone and back: said again.
    const AccessTree gone;
    CHECK(access_news(gone, before).size() == 1);
}

TEST_CASE("pages access: the step strip, in each language") {
    CHECK(step_said(1, 3, "Runtimes", Lang::En) == "Step 2 of 3, Runtimes");
    CHECK(step_said(1, 3, text(Lang::Ar, Str::FirstRunStepRuntimes), Lang::Ar) == "الخطوة 2 من 3، بيئات التشغيل");
    CHECK(step_said(0, 2, "قاڵب", Lang::Ku) == "هەنگاوی 1 لە 2، قاڵب");

    const AccessNode strip = step_strip_node("first-run/steps", Lang::Ar);
    CHECK(strip.role == AccessRole::List);
    CHECK(strip.name == "الخطوات");
    const AccessNode done = step_node("s/0", "Language", 0, 1, 3, Lang::En);
    CHECK(done.name == "Language, done");
    CHECK(done.selected == std::optional<bool>(false));
    CHECK(done.position == 1);
    CHECK(done.set_size == 3);
    const AccessNode now = step_node("s/1", "Runtimes", 1, 1, 3, Lang::En);
    CHECK(now.name == "Runtimes");
    CHECK(now.selected == std::optional<bool>(true));
    CHECK(step_node("s/0", "زمان", 0, 2, 3, Lang::Ku).name == "زمان، تەواو بوو");

    CHECK(heading_node("t", "Welcome").heading == 1);
    CHECK(heading_node("t", "Welcome").role == AccessRole::Text);
}

TEST_CASE("pages access: New project's steps and titles") {
    CHECK(wizard_step_names(true, Lang::En) == std::vector<std::string>{"Template", "Name and folder", "Versions"});
    CHECK(wizard_step_names(false, Lang::Ar) == std::vector<std::string>{"القالب", "الاسم والمجلد"});
    CHECK(wizard_step_names(true, Lang::Ku).back() == "وەشانەکان");
    CHECK(wizard_strip_step(3, true) == 2);
    CHECK(wizard_strip_step(3, false) == 1);
    CHECK(wizard_strip_step(0, false) == 0);

    CHECK(wizard_title(0, "", "", Lang::En) == "What are you building?");
    CHECK(access_text(wizard_title(3, "running", "my-shop", Lang::En)) == "Making my-shop…");
    CHECK(access_text(wizard_title(3, "done", "my-shop", Lang::Ar)) == "my-shop جاهز");
    CHECK(access_text(wizard_title(3, "done", "my-shop", Lang::Ku)) == "my-shop ئامادەیە");
    CHECK(wizard_title(3, "cancelled", "my-shop", Lang::En) == "The project wasn't made");

    const FooterText footer;
    CHECK(access_text(wizard_name_hint("my-shop", footer, Lang::En)).starts_with("Its address: "));
    CHECK(wizard_name_hint("My Shop", footer, Lang::En) == text(Lang::En, Str::WizardNameRule));
    CHECK(access_text(wizard_made_in("C:\\Sites", "my-shop", Lang::En)) == "The project folder: C:\\Sites\\my-shop");
    CHECK(access_text(wizard_made_in("C:\\Sites\\", "-", Lang::Ar)) == "مجلد المشروع: C:\\Sites\\…");
}

TEST_CASE("pages access: a template card is a list item") {
    TemplateCard card;
    card.id = "laravel";
    card.title = "Laravel";
    card.description = "A PHP framework";
    card.chips = {"PHP", "MariaDB"};
    const AccessNode n = template_card_node(card, true, 2, 6, Lang::En);
    CHECK(n.id == "wizard/card/laravel");
    CHECK(n.role == AccessRole::ListItem);
    CHECK(n.name == "Laravel, A PHP framework, PHP, MariaDB");
    CHECK(n.selected == std::optional<bool>(true));
    CHECK(n.focusable);
    CHECK(n.invokable);
    CHECK(n.position == 2);
    CHECK(n.set_size == 6);
    CHECK(template_card_node(card, false, 1, 1, Lang::Ar).name == "Laravel، A PHP framework، PHP، MariaDB");
}

TEST_CASE("pages access: what New project says once") {
    WizardNewsState s;
    auto news = wizard_news(s, Lang::En);
    REQUIRE(news.size() == 1);
    CHECK(news[0] == AccessNews{"step", "Step 1 of 3, Template", false});

    s.templates_error = "offline";
    news = wizard_news(s, Lang::En);
    REQUIRE(news_of(news, "templates").has_value());
    CHECK(news_of(news, "templates")->important);

    s = {};
    s.step = 1;
    s.versions = false;
    CHECK(wizard_news(s, Lang::Ar)[0].text == "الخطوة 2 من 2، الاسم والمجلد");

    s.step = 3;
    s.versions = true;
    s.name = "my-shop";
    s.state = "running";
    news = wizard_news(s, Lang::En);
    CHECK(news_of(news, "step")->text == "Step 3 of 3, Versions");
    CHECK(news_of(news, "made")->text == "Making my-shop…");
    s.state = "done";
    CHECK(news_of(wizard_news(s, Lang::En), "made")->text == "my-shop is ready");
    CHECK(news_of(wizard_news(s, Lang::Ku), "made")->text == "my-shop ئامادەیە");
    s.state = "failed";
    s.error = "the folder exists";
    const auto failed = news_of(wizard_news(s, Lang::En), "made");
    REQUIRE(failed.has_value());
    CHECK(failed->text == "Couldn't make the project: the folder exists");
    CHECK(failed->important);
    CHECK(news_of(wizard_news(s, Lang::Ar), "made")->text == "تعذّر إنشاء المشروع: the folder exists");
    s.state = "cancelled";
    CHECK(news_of(wizard_news(s, Lang::En), "made")->text == "The project wasn't made");
}

TEST_CASE("pages access: the language radios") {
    const LanguageRowText same = first_run_language_row(LangChoice::System, Lang::Ar, Lang::En);
    CHECK(same.label == "Same as Windows");
    CHECK(access_text(same.note) == "العربية now");
    const LanguageRowText ku = first_run_language_row(LangChoice::Ku, Lang::En, Lang::Ku);
    CHECK(ku.label == "کوردی");
    CHECK(ku.note == "سۆرانی");
    CHECK(access_text(first_run_language_row(LangChoice::System, Lang::En, Lang::Ar).label) == "مثل Windows");

    const AccessNode n = language_radio_node("first-run/lang/3", ku, true, 4, 4);
    CHECK(n.role == AccessRole::Radio);
    CHECK(n.name == "کوردی");
    CHECK(n.help == "سۆرانی");
    CHECK(n.selected == std::optional<bool>(true));
    CHECK(n.focusable);
    CHECK(n.position == 4);
    CHECK(n.set_size == 4);
}

TEST_CASE("pages access: a runtime offered is a check box") {
    FirstRunOffer php;
    php.name = "php";
    php.label = "PHP 8.5";
    php.what = Str::FirstRunWhatPhp;
    php.size = 31ULL * 1024 * 1024;
    const AccessNode n = offer_node("first-run/offer/php", php, true, 1, 8, Lang::En);
    CHECK(n.role == AccessRole::CheckBox);
    CHECK(n.name == "PHP 8.5, newest, 31 MB");
    CHECK(n.toggled == std::optional<bool>(true));
    CHECK(n.enabled);
    CHECK(n.focusable);
    CHECK(offer_node("x", php, false, 1, 8, Lang::Ar).name == "PHP 8.5، الأحدث، 31 MB");
    CHECK(offer_node("x", php, false, 1, 8, Lang::Ku).toggled == std::optional<bool>(false));

    FirstRunOffer nginx;
    nginx.label = "nginx";
    nginx.what = Str::FirstRunWhatNginx;
    nginx.always = true;
    const AccessNode always = offer_node("first-run/offer/nginx", nginx, false, 8, 8, Lang::En);
    CHECK(always.name == "nginx, always included; it serves every project");
    CHECK(always.toggled == std::optional<bool>(true));
    CHECK_FALSE(always.enabled);
    CHECK_FALSE(always.focusable);
}

TEST_CASE("pages access: a download's row and its progress") {
    FirstRunRow running = row_of("php", "PHP 8.5.12", FirstRunRowState::Running, "Downloading… 19 MB of 31 MB");
    running.fraction = 0.6;
    const AccessNode row = download_row_node("first-run/download/php", running, Lang::En);
    CHECK(row.role == AccessRole::Group);
    CHECK(row.name == "PHP 8.5.12, Downloading…");  // the numbers aren't in the name: not said on every tick
    CHECK(row.help == "Downloading… 19 MB of 31 MB");
    const AccessNode bar = download_progress_node("first-run/download/php/progress", running);
    CHECK(bar.role == AccessRole::Progress);
    CHECK(bar.name == "PHP 8.5.12");
    CHECK(bar.progress == std::optional<double>(0.6));

    const FirstRunRow failed =
        row_of("node", "Node.js 24.1.0", FirstRunRowState::Failed, fill(Lang::Ar, Str::FirstRunRowFailed, {"timeout"}));
    const AccessNode f = download_row_node("x", failed, Lang::Ar);
    CHECK(f.name == "Node.js 24.1.0، فشل: timeout");
    CHECK(f.help.empty());
    CHECK(download_row_node("x", row_of("php", "PHP 8.5.12", FirstRunRowState::Ready, "ئامادەیە"), Lang::Ku).name ==
          "PHP 8.5.12، ئامادەیە");
}

TEST_CASE("pages access: what the first-run page says once") {
    FirstRunNewsState s;
    auto news = first_run_news(s, Lang::En);
    REQUIRE(news.size() == 1);
    CHECK(news[0].text == "Step 1 of 3, Language");

    s.step = FirstRunStep::Runtimes;
    s.list = FirstRunList::Failed;
    s.list_error = "offline";
    news = first_run_news(s, Lang::Ar);
    CHECK(news_of(news, "step")->text == "الخطوة 2 من 3، بيئات التشغيل");
    REQUIRE(news_of(news, "list").has_value());
    CHECK(news_of(news, "list")->important);
    CHECK(news_of(news, "list")->text == "تعذّر جلب قائمة بيئات التشغيل: offline");

    // Downloading: nothing until one fails or they've all ended.
    s.list = FirstRunList::Ready;
    s.started = true;
    s.rows = {row_of("nginx", "nginx 1.28.0", FirstRunRowState::Ready, "Ready"),
              row_of("php", "PHP 8.5.12", FirstRunRowState::Running, "Downloading…")};
    s.trust = FirstRunTrust::Checking;
    news = first_run_news(s, Lang::En);
    CHECK(news.size() == 1);

    s.trust = FirstRunTrust::Asking;
    news = first_run_news(s, Lang::En);
    REQUIRE(news_of(news, "trust").has_value());
    CHECK(news_of(news, "trust")->text.starts_with("Windows is asking whether to trust"));
    CHECK(news_of(news, "trust")->important);

    s.trust = FirstRunTrust::Trusted;
    s.rows[1] = row_of("php", "PHP 8.5.12", FirstRunRowState::Failed, "Failed: checksum");
    news = first_run_news(s, Lang::En);
    CHECK_FALSE(news_of(news, "trust")->important);
    REQUIRE(news_of(news, "row/php").has_value());
    CHECK(news_of(news, "row/php")->text == "PHP 8.5.12, Failed: checksum");
    CHECK(news_of(news, "row/php")->important);
    REQUIRE(news_of(news, "downloads").has_value());
    CHECK(news_of(news, "downloads")->text ==
          "Downloads finished, PHP 8.5.12 didn't download; try again in Settings → Runtimes");

    s.rows[1].state = FirstRunRowState::Ready;
    s.rows[1].line = "جاهز";
    CHECK(news_of(first_run_news(s, Lang::Ar), "downloads")->text == "انتهت التنزيلات");
    CHECK(news_of(first_run_news(s, Lang::Ku), "downloads")->text == "داگرتنەکان تەواو بوون");

    s.step = FirstRunStep::Ready;
    s.folder = "D:\\Sites";
    CHECK(news_of(first_run_news(s, Lang::En), "folder")->text == "New projects go in, D:\\Sites");
}
