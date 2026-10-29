#include "model/ui_strings.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <iterator>

namespace floserver::ui {

namespace {

// U+2066 and U+2069 in UTF-8, for code words inside the Arabic and Kurdish text.
#define FLO_LRI "\xE2\x81\xA6"
#define FLO_PDI "\xE2\x81\xA9"

struct Entry {
    Str key;
    std::string_view en;
    std::string_view ar;
    std::string_view ku;
};

// Arabic and Kurdish need a native read before release (see the step 5a report).
// Words the mockups already use: يعمل / نائم / متوقف.
constexpr std::array kTable{
    Entry{Str::Projects, "Projects", "المشاريع", "پڕۆژەکان"},
    Entry{Str::SearchProjects, "Search projects", "ابحث في المشاريع", "گەڕان لە پڕۆژەکان"},
    Entry{Str::NewProject, "+ New project", "+ مشروع جديد", "+ پڕۆژەی نوێ"},
    Entry{Str::ComingSoon, "Coming soon", "قريبًا", "بەم زووانە"},
    Entry{Str::Settings, "Settings", "الإعدادات", "ڕێکخستنەکان"},
    Entry{Str::StateOn, "On", "يعمل", "کار دەکات"},
    Entry{Str::StateAsleep, "Asleep", "نائم", "خەوتووە"},
    Entry{Str::StateOff, "Off", "متوقف", "کوژاوە"},
    Entry{Str::StateNeedsDownload, "Needs download", "يحتاج تنزيلًا", "پێویستی بە داگرتنە"},
    Entry{Str::StateProblem, "Problem", "مشكلة", "کێشە"},
    Entry{Str::PhpCantStart, "PHP can't start", "تعذّر تشغيل " FLO_LRI "PHP" FLO_PDI,
          FLO_LRI "PHP" FLO_PDI " دەست پێ ناکات"},
    Entry{Str::Needs, "Needs {0}", "يحتاج {0}", "پێویستی بە {0} هەیە"},
    Entry{Str::Connecting, "Connecting to FloServer…", "جارٍ الاتصال بـ " FLO_LRI "FloServer" FLO_PDI "…",
          "پەیوەندی بە " FLO_LRI "FloServer" FLO_PDI " دەکرێت…"},
    Entry{Str::StartingService, "Starting FloServer…", "جارٍ تشغيل " FLO_LRI "FloServer" FLO_PDI "…",
          FLO_LRI "FloServer" FLO_PDI " دەست پێ دەکات…"},
    Entry{Str::ServiceFailed, "FloServer didn't start: {0}", "لم يبدأ " FLO_LRI "FloServer" FLO_PDI ": {0}",
          FLO_LRI "FloServer" FLO_PDI " دەستی پێ نەکرد: {0}"},
    Entry{Str::ServiceStopped, "FloServer isn't running. Click here to start it.",
          FLO_LRI "FloServer" FLO_PDI " لا يعمل. انقر هنا لتشغيله.",
          FLO_LRI "FloServer" FLO_PDI " کار ناکات. کرتە لێرە بکە بۆ دەستپێکردنی."},
    Entry{Str::ConnectionLost, "FloServer stopped answering. Trying again…",
          "توقف " FLO_LRI "FloServer" FLO_PDI " عن الرد. نعيد المحاولة…",
          FLO_LRI "FloServer" FLO_PDI " وەڵام ناداتەوە. هەوڵ دەدرێتەوە…"},
    Entry{Str::NoProjects, "No projects yet. Add a folder with: {0}", "لا توجد مشاريع بعد. أضف مجلدًا بالأمر: {0}",
          "هێشتا هیچ پڕۆژەیەک نییە. بوخچەیەک زیاد بکە بە: {0}"},
    Entry{Str::NoMatches, "No project matches “{0}”", "لا يوجد مشروع يطابق «{0}»",
          "هیچ پڕۆژەیەک لەگەڵ «{0}» ناگونجێت"},
    Entry{Str::FooterCounts, "{0} running · {1} asleep", "يعمل: {0} · نائم: {1}", "کار دەکات: {0} · خەوتووە: {1}"},
    Entry{Str::FooterNginx, "nginx on ports {0} and {1}", FLO_LRI "nginx" FLO_PDI " على المنفذين {0} و{1}",
          FLO_LRI "nginx" FLO_PDI " لەسەر پۆرتەکانی {0} و {1}"},
    Entry{Str::FooterNginxOff, "nginx isn't running", FLO_LRI "nginx" FLO_PDI " لا يعمل",
          FLO_LRI "nginx" FLO_PDI " کار ناکات"},
    Entry{Str::FooterMemory, "Service memory: {0}", "ذاكرة الخدمة: {0}", "بیرگەی خزمەتگوزاری: {0}"},
    Entry{Str::ChangeFailed, "Couldn't change {0}: {1}", "تعذّر تغيير {0}: {1}", "نەتوانرا {0} بگۆڕدرێت: {1}"},
    Entry{Str::TrayOpen, "Open FloServer", "فتح " FLO_LRI "FloServer" FLO_PDI,
          "کردنەوەی " FLO_LRI "FloServer" FLO_PDI},
    Entry{Str::TrayQuit, "Quit FloServer", "إنهاء " FLO_LRI "FloServer" FLO_PDI,
          "داخستنی " FLO_LRI "FloServer" FLO_PDI},
    Entry{Str::Stopping, "Stopping FloServer…", "جارٍ إيقاف " FLO_LRI "FloServer" FLO_PDI "…",
          FLO_LRI "FloServer" FLO_PDI " ڕادەگیرێت…"},
};

#undef FLO_LRI
#undef FLO_PDI

constexpr bool table_in_order() {
    for (std::size_t i = 0; i < kTable.size(); ++i) {
        if (static_cast<std::size_t>(kTable[i].key) != i) {
            return false;
        }
    }
    return kTable.size() == static_cast<std::size_t>(Str::Count_);
}
static_assert(table_in_order(), "kTable must list every Str once, in order");

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

std::optional<Lang> parse_lang(std::string_view code) {
    const std::size_t cut = code.find_first_of("-_.");
    const std::string base = lower_ascii(code.substr(0, cut));
    if (base == "en") {
        return Lang::En;
    }
    if (base == "ar") {
        return Lang::Ar;
    }
    if (base == "ku" || base == "ckb") {
        return Lang::Ku;
    }
    return std::nullopt;
}

Lang lang_from_setting(const char* value) {
    if (value == nullptr) {
        return Lang::En;
    }
    return parse_lang(value).value_or(Lang::En);
}

bool is_rtl(Lang lang) { return lang != Lang::En; }

std::string_view text(Lang lang, Str key) {
    const auto index = static_cast<std::size_t>(key);
    if (index >= kTable.size()) {
        return {};
    }
    const Entry& e = kTable.at(index);
    switch (lang) {
        case Lang::Ar:
            return e.ar;
        case Lang::Ku:
            return e.ku;
        case Lang::En:
            break;
    }
    return e.en;
}

// Byte by byte, not as literals: a lone isolate in a literal looks like a
// Trojan Source trick to the compiler checks.
constexpr char kLri[] = {'\xE2', '\x81', '\xA6'};
constexpr char kFsi[] = {'\xE2', '\x81', '\xA8'};
constexpr char kPdi[] = {'\xE2', '\x81', '\xA9'};

std::string isolated(const char (&open)[3], std::string_view s) {
    std::string out(std::begin(open), std::end(open));
    out += s;
    out.append(std::begin(kPdi), std::end(kPdi));
    return out;
}

std::string isolate_ltr(std::string_view s) { return isolated(kLri, s); }
std::string isolate_auto(std::string_view s) { return isolated(kFsi, s); }

std::string fill_pattern(std::string_view pattern, bool isolate, std::initializer_list<std::string_view> values) {
    std::string out;
    out.reserve(pattern.size() + 16);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '{' && i + 2 < pattern.size() && pattern[i + 2] == '}' && pattern[i + 1] >= '0' &&
            pattern[i + 1] <= '9') {
            const auto n = static_cast<std::size_t>(pattern[i + 1] - '0');
            if (n < values.size()) {
                const std::string_view v = *(values.begin() + static_cast<std::ptrdiff_t>(n));
                out += isolate ? isolate_ltr(v) : std::string(v);
            }
            i += 2;
            continue;
        }
        out += c;
    }
    return out;
}

std::string fill(Lang lang, Str key, std::initializer_list<std::string_view> values) {
    return fill_pattern(text(lang, key), is_rtl(lang), values);
}

}  // namespace floserver::ui
