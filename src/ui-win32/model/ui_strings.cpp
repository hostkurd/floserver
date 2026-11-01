#include "model/ui_strings.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <iterator>

namespace floserver::ui {

namespace {

// U+202A LEFT-TO-RIGHT EMBEDDING and U+202C POP DIRECTIONAL FORMATTING in
// UTF-8, for code words inside the Arabic and Kurdish text. Not the newer
// isolates (U+2066...): Windows' DrawText shows those as boxes.
#define FLO_LRI "\xE2\x80\xAA"
#define FLO_PDI "\xE2\x80\xAC"

struct Entry {
    Str key;
    std::string_view en;
    std::string_view ar;
    std::string_view ku;
};

// Arabic and Kurdish need a native read before release (see the step 5a report).
// Words the mockups already use: يعمل / نائم / متوقف.
// The size spelled out: deducing it from 280+ entries passes clang's nesting limit.
constexpr std::array<Entry, static_cast<std::size_t>(Str::Count_)> kTable{{
    Entry{Str::Projects, "Projects", "المشاريع", "پڕۆژەکان"},
    Entry{Str::SearchProjects, "Search projects", "ابحث في المشاريع", "گەڕان لە پڕۆژەکان"},
    Entry{Str::NewProject, "+ New project", "+ مشروع جديد", "+ پڕۆژەی نوێ"},
    Entry{Str::ComingSoon, "Coming soon", "قريبًا", "بەم زووانە"},
    Entry{Str::Settings, "Settings", "الإعدادات", "ڕێکخستنەکان"},
    Entry{Str::StateOn, "On", "يعمل", "کار دەکات"},
    Entry{Str::StateAsleep, "Sleeping", "نائم", "خەوتووە"},
    Entry{Str::StateOff, "Off", "متوقف", "کوژاوە"},
    Entry{Str::StateNeedsDownload, "Needs download", "يحتاج تنزيلًا", "پێویستی بە داگرتنە"},
    Entry{Str::StateProblem, "Problem", "مشكلة", "کێشە"},
    Entry{Str::PhpCantStart, "PHP can't start", "تعذّر تشغيل " FLO_LRI "PHP" FLO_PDI,
          FLO_LRI "PHP" FLO_PDI " دەست پێ ناکات"},
    Entry{Str::ServerCantStart, "The project's server can't start", "تعذّر تشغيل خادم المشروع",
          "سێرڤەری پڕۆژەکە ناتوانرێت بخرێتە کار"},
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
    Entry{Str::NoProjects, "No projects yet. Click {0} to add a folder.", "لا توجد مشاريع بعد. انقر {0} لإضافة مجلد.",
          "هێشتا هیچ پڕۆژەیەک نییە. کرتە لە {0} بکە بۆ زیادکردنی بوخچەیەک."},
    Entry{Str::NoMatches, "No project matches “{0}”", "لا يوجد مشروع يطابق «{0}»",
          "هیچ پڕۆژەیەک لەگەڵ «{0}» ناگونجێت"},
    Entry{Str::FooterCounts, "{0} running · {1} sleeping", "يعمل: {0} · نائم: {1}", "کار دەکات: {0} · خەوتووە: {1}"},
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
    // Step 5b: the project panel. Arabic and Kurdish need a native read.
    Entry{Str::StateWaking, "Waking…", "يستيقظ…", "دەخەبەرێتەوە…"},
    Entry{Str::OpenInBrowser, "Open in browser", "فتح في المتصفح", "کردنەوە لە وێبگەڕ"},
    Entry{Str::OpenFolder, "Folder", "المجلد", "بوخچە"},
    Entry{Str::OpenTerminal, "Terminal", "الطرفية", "تێرمیناڵ"},
    Entry{Str::OpenInStudio, "Open in Studio", "فتح في " FLO_LRI "Studio" FLO_PDI,
          "کردنەوە لە " FLO_LRI "Studio" FLO_PDI},
    Entry{Str::ClosePanel, "Close panel", "إغلاق اللوحة", "داخستنی پانێڵ"},
    Entry{Str::Versions, "Versions", "الإصدارات", "وەشانەکان"},
    Entry{Str::FieldPhp, "PHP", FLO_LRI "PHP" FLO_PDI, FLO_LRI "PHP" FLO_PDI},
    Entry{Str::FieldDatabase, "Database", "قاعدة البيانات", "داتابەیس"},
    Entry{Str::FieldWeb, "Web server", "خادم الويب", "سێرڤەری وێب"},
    Entry{Str::FieldNode, "Node.js", FLO_LRI "Node.js" FLO_PDI, FLO_LRI "Node.js" FLO_PDI},
    Entry{Str::FieldPython, "Python", FLO_LRI "Python" FLO_PDI, FLO_LRI "Python" FLO_PDI},
    Entry{Str::ChoiceNone, "None", "بلا", "هیچ"},
    Entry{Str::ChoiceNewest, "Newest installed", "أحدث إصدار مثبّت", "نوێترین وەشانی دامەزراو"},
    Entry{Str::NotInstalled, "{0} (not installed)", "{0} (غير مثبّت)", "{0} (دانەمەزراوە)"},
    Entry{Str::SavedIn, "Saved in {0}", "يُحفظ في {0}", "پاشەکەوت دەکرێت لە {0}"},
    Entry{Str::PhpOffNote, "PHP is off: .php files are sent as they are, not run.",
          FLO_LRI "PHP" FLO_PDI " متوقف: تُرسَل ملفات " FLO_LRI ".php" FLO_PDI " كما هي ولا تُشغَّل.",
          FLO_LRI "PHP" FLO_PDI " کوژاوەتەوە: فایلەکانی " FLO_LRI ".php" FLO_PDI " وەک خۆیان دەنێردرێن، کار پێناکرێن."},
    Entry{Str::DatabaseTitle, "Database {0}", "قاعدة البيانات {0}", "داتابەیسی {0}"},
    Entry{Str::DatabaseDetail, "{0} · user {1}", "{0} · المستخدم {1}", "{0} · بەکارهێنەر {1}"},
    Entry{Str::CopyEnv, "Copy .env lines", "نسخ أسطر " FLO_LRI ".env" FLO_PDI,
          "لەبەرگرتنەوەی دێڕەکانی " FLO_LRI ".env" FLO_PDI},
    Entry{Str::EnvCopied, "The .env lines are on the clipboard", "نُسخت أسطر " FLO_LRI ".env" FLO_PDI " إلى الحافظة",
          "دێڕەکانی " FLO_LRI ".env" FLO_PDI " لەبەرگیرانەوە"},
    Entry{Str::KeepRunning, "Keep running", "إبقاؤه يعمل", "با هەمیشە کار بکات"},
    Entry{Str::KeepRunningOff, "Off: sleeps after {0} minutes without requests",
          "متوقف: ينام بعد {0} دقائق بلا طلبات", "کوژاوە: دوای {0} خولەک بێ داواکاری دەخەوێت"},
    Entry{Str::KeepRunningOn, "On: never sleeps", "مفعّل: لا ينام أبدًا", "هەڵکراوە: هەرگیز ناخەوێت"},
    Entry{Str::Log, "Log", "السجل", "تۆمار"},
    Entry{Str::LogEmpty, "No requests yet", "لا طلبات بعد", "هێشتا هیچ داواکارییەک نییە"},
    Entry{Str::OpenFailed, "Couldn't open {0}", "تعذّر فتح {0}", "نەتوانرا {0} بکرێتەوە"},
    Entry{Str::AddExistingFolder, "Add an existing folder…", "إضافة مجلد موجود…", "زیادکردنی بوخچەیەکی هەبوو…"},
    Entry{Str::AddFailed, "Couldn't add {0}: {1}", "تعذّرت إضافة {0}: {1}", "نەتوانرا {0} زیاد بکرێت: {1}"},
    Entry{Str::DownloadOne, "Download {0}", "تنزيل {0}", "داگرتنی {0}"},
    Entry{Str::DownloadMany, "Download {0} runtimes", "تنزيل بيئات التشغيل ({0})", "داگرتنی ژینگەکانی کارپێکردن ({0})"},
    Entry{Str::Downloading, "Downloading {0}… {1}", "جارٍ تنزيل {0}… {1}", "{0} دادەگیرێت… {1}"},
    Entry{Str::Installing, "Installing {0}…", "جارٍ تثبيت {0}…", "{0} دادەمەزرێت…"},
    Entry{Str::DownloadFailed, "Couldn't download {0}: {1}", "تعذّر تنزيل {0}: {1}", "نەتوانرا {0} دابگیرێت: {1}"},
    Entry{Str::WizardStepTemplate, "1 Template", "1 القالب", "1 قاڵب"},
    Entry{Str::WizardStepName, "2 Name and folder", "2 الاسم والمجلد", "2 ناو و بوخچە"},
    Entry{Str::WizardStepVersions, "3 Versions", "3 الإصدارات", "3 وەشانەکان"},
    Entry{Str::WizardWhat, "What are you building?", "ماذا تبني؟", "چی دروست دەکەیت؟"},
    Entry{Str::WizardNameTitle, "Name and folder", "الاسم والمجلد", "ناو و بوخچە"},
    Entry{Str::WizardVersionsTitle, "Versions", "الإصدارات", "وەشانەکان"},
    Entry{Str::WizardName, "Name", "الاسم", "ناو"},
    Entry{Str::WizardFolder, "Made in", "يُنشأ في", "دروست دەکرێت لە"},
    Entry{Str::WizardBrowse, "Browse…", "استعراض…", "گەڕان…"},
    Entry{Str::WizardAddress, "Its address: {0}", "عنوانه: {0}", "ناونیشانەکەی: {0}"},
    Entry{Str::WizardMadeIn, "The project folder: {0}", "مجلد المشروع: {0}", "بوخچەی پڕۆژەکە: {0}"},
    Entry{Str::WizardNameRule, "Lower-case letters, digits and -, not at the start or end",
          "أحرف إنجليزية صغيرة وأرقام و-، ليس في البداية أو النهاية",
          "پیتی بچووکی ئینگلیزی و ژمارە و -، نە لە سەرەتا و نە لە کۆتایی"},
    Entry{Str::WizardDownloads, "Versions not installed yet are downloaded when the project is made.",
          "الإصدارات غير المثبّتة تُنزَّل عند إنشاء المشروع.",
          "ئەو وەشانانەی دانەمەزراون لە کاتی دروستکردنی پڕۆژەکەدا دادەگیرێن."},
    Entry{Str::WizardBack, "Back", "رجوع", "گەڕانەوە"},
    Entry{Str::WizardNext, "Next", "التالي", "دواتر"},
    Entry{Str::WizardCreate, "Create", "إنشاء", "دروستکردن"},
    Entry{Str::WizardCancel, "Cancel", "إلغاء", "پاشگەزبوونەوە"},
    Entry{Str::WizardMaking, "Making {0}…", "جارٍ إنشاء {0}…", "{0} دروست دەکرێت…"},
    Entry{Str::WizardDone, "{0} is ready", "{0} جاهز", "{0} ئامادەیە"},
    Entry{Str::WizardFailed, "Couldn't make the project: {0}", "تعذّر إنشاء المشروع: {0}", "نەتوانرا پڕۆژەکە دروست بکرێت: {0}"},
    Entry{Str::WizardNotMade, "The project wasn't made", "لم يُنشأ المشروع", "پڕۆژەکە دروست نەکرا"},
    Entry{Str::WizardOpenProject, "Open project", "فتح المشروع", "کردنەوەی پڕۆژە"},
    Entry{Str::WizardClose, "Close", "إغلاق", "داخستن"},
    Entry{Str::WizardNoTemplates, "The templates come with the runtime list, which couldn't be loaded: {0}",
          "تأتي القوالب مع قائمة بيئات التشغيل، وتعذّر تحميلها: {0}",
          "قاڵبەکان لەگەڵ لیستی ژینگەکانی کارپێکردن دێن، کە نەتوانرا باربکرێت: {0}"},
    Entry{Str::WizardLoading, "Loading the templates…", "جارٍ تحميل القوالب…", "قاڵبەکان بار دەکرێن…"},
    Entry{Str::SharedSites, "Shared sites", "المواقع المشتركة", "ماڵپەڕە هاوبەشەکان"},
    Entry{Str::SharedHint, "Every folder in {0} opens at {1}", "كل مجلد في {0} يُفتح على {1}",
          "هەموو بوخچەیەک لە {0} لە {1} دەکرێتەوە"},
    Entry{Str::SharedCantStart, "Shared sites can't start", "تعذّر تشغيل المواقع المشتركة",
          "ماڵپەڕە هاوبەشەکان ناتوانرێن بخرێنە کار"},
    Entry{Str::SharedOpenFolder, "Open folder", "فتح المجلد", "کردنەوەی بوخچە"},
    Entry{Str::SharedWwwFolder, "www folder", "مجلد " FLO_LRI "www" FLO_PDI, "بوخچەی " FLO_LRI "www" FLO_PDI},
    Entry{Str::SharedChange, "Change…", "تغيير…", "گۆڕین…"},
    Entry{Str::SharedOpen, "Open", "فتح", "کردنەوە"},
    Entry{Str::WebApacheChoice, "Apache (.htaccess works)", FLO_LRI "Apache" FLO_PDI " (يعمل " FLO_LRI ".htaccess" FLO_PDI ")",
          FLO_LRI "Apache" FLO_PDI " (" FLO_LRI ".htaccess" FLO_PDI " کار دەکات)"},
    Entry{Str::WebNginxChoice, "nginx (lighter, no .htaccess)",
          FLO_LRI "nginx" FLO_PDI " (أخف، بلا " FLO_LRI ".htaccess" FLO_PDI ")",
          FLO_LRI "nginx" FLO_PDI " (سووکترە، بێ " FLO_LRI ".htaccess" FLO_PDI ")"},
    Entry{Str::SharedDatabaseDetail, "{0} · user {1} · can create {2}… databases",
          "{0} · المستخدم {1} · يمكنه إنشاء قواعد بيانات {2}…", "{0} · بەکارهێنەر {1} · دەتوانێت داتابەیسی {2}… دروست بکات"},
    // Settings (D20). Arabic and Kurdish need a native read.
    Entry{Str::SettingsTitle, "FloServer Settings", "إعدادات " FLO_LRI "FloServer" FLO_PDI,
          "ڕێکخستنەکانی " FLO_LRI "FloServer" FLO_PDI},
    Entry{Str::SettingsRuntimes, "Runtimes", "بيئات التشغيل", "ژینگەکانی کارپێکردن"},
    Entry{Str::SettingsRuntimesHint, "Download, remove and choose the default for new projects.",
          "نزّل واحذف واختر الإصدار الافتراضي للمشاريع الجديدة.",
          "دایبگرە، بیسڕەوە و وەشانی بنەڕەت بۆ پڕۆژە نوێیەکان هەڵبژێرە."},
    Entry{Str::SettingsListFrom, "The list of versions is number {0}, good until {1}. Nothing is checked online until you ask.",
          "قائمة الإصدارات رقم {0}، صالحة حتى {1}. لا يُفحص شيء عبر الإنترنت حتى تطلب ذلك.",
          "لیستی وەشانەکان: ژمارە {0}، تا {1} دەست دەدات. هیچ شتێک لە ئینتەرنێت ناپشکنرێت تا داوای نەکەیت."},
    Entry{Str::SettingsNoList,
          "There's no list of versions on this computer yet, so only what's installed is shown. Check for updates to see what you can download.",
          "لا توجد قائمة إصدارات على هذا الحاسوب بعد، لذا يظهر المثبّت فقط. افحص التحديثات لترى ما يمكنك تنزيله.",
          "هێشتا لیستی وەشانەکان لەسەر ئەم کۆمپیوتەرە نییە، بۆیە تەنها ئەوەی دامەزراوە پیشان دەدرێت. نوێکارییەکان بپشکنە بۆ بینینی ئەوەی دەتوانیت دایبگریت."},
    Entry{Str::SettingsLoading, "Loading…", "جارٍ التحميل…", "بار دەکرێت…"},
    Entry{Str::SettingsCheckUpdates, "Check for updates", "فحص التحديثات", "پشکنینی نوێکارییەکان"},
    Entry{Str::SettingsChecking, "Checking…", "جارٍ الفحص…", "دەپشکنرێت…"},
    Entry{Str::SettingsCheckFailed, "Couldn't check for updates: {0}", "تعذّر فحص التحديثات: {0}",
          "نەتوانرا نوێکارییەکان بپشکنرێن: {0}"},
    Entry{Str::SettingsReadFailed, "Couldn't read the runtimes: {0}", "تعذّرت قراءة بيئات التشغيل: {0}",
          "نەتوانرا ژینگەکانی کارپێکردن بخوێندرێنەوە: {0}"},
    Entry{Str::SettingsDefaultFor, "default for new projects", "الافتراضي للمشاريع الجديدة", "بنەڕەت بۆ پڕۆژە نوێیەکان"},
    Entry{Str::SettingsDefaultChip, "Default", "افتراضي", "بنەڕەت"},
    Entry{Str::SettingsInstalledChip, "Installed", "مثبّت", "دامەزراوە"},
    Entry{Str::SettingsAlwaysUsed, "Always used", "مستخدم دائمًا", "هەمیشە بەکاردێت"},
    Entry{Str::SettingsWebServers, "Web servers", "خوادم الويب", "سێرڤەرەکانی وێب"},
    Entry{Str::SettingsOneProject, "1 project", "مشروع واحد", "1 پڕۆژە"},
    Entry{Str::SettingsProjects, "{0} projects", "المشاريع: {0}", "پڕۆژەکان: {0}"},
    Entry{Str::SettingsUsedBy, "Used by {0}", "يستخدمه {0}", "بەکاردێت لەلایەن {0}"},
    Entry{Str::SettingsRunningOnPort, "running on port {0}", "يعمل على المنفذ {0}", "لەسەر پۆرتی {0} کار دەکات"},
    Entry{Str::SettingsOnPort, "port {0}", "المنفذ {0}", "پۆرتی {0}"},
    Entry{Str::SettingsDownload, "Download", "تنزيل", "داگرتن"},
    Entry{Str::SettingsRemove, "Remove", "إزالة", "سڕینەوە"},
    Entry{Str::SettingsStarting, "Starting…", "جارٍ البدء…", "دەست پێ دەکات…"},
    Entry{Str::SettingsDownloadingOf, "Downloading… {0} of {1}", "جارٍ التنزيل… {0} من {1}", "دادەگیرێت… {0} لە {1}"},
    Entry{Str::SettingsDownloadingDots, "Downloading…", "جارٍ التنزيل…", "دادەگیرێت…"},
    Entry{Str::SettingsInstallingDots, "Installing…", "جارٍ التثبيت…", "دادەمەزرێت…"},
    Entry{Str::SettingsRemoving, "Removing…", "جارٍ الإزالة…", "دەسڕدرێتەوە…"},
    Entry{Str::SettingsRemoveFailed, "Couldn't remove {0}: {1}", "تعذّرت إزالة {0}: {1}", "نەتوانرا {0} بسڕدرێتەوە: {1}"},
    Entry{Str::SettingsDefaultFailed, "Couldn't change the default: {0}", "تعذّر تغيير الافتراضي: {0}",
          "نەتوانرا بنەڕەتەکە بگۆڕدرێت: {0}"},
    // Settings > PHP (D20). Arabic and Kurdish need a native read.
    Entry{Str::SettingsPhp, "PHP", FLO_LRI "PHP" FLO_PDI, FLO_LRI "PHP" FLO_PDI},
    Entry{Str::PhpHint,
          "The defaults for every project and shared sites on this version. A project can change them in its own panel.",
          "الإعدادات الافتراضية لكل المشاريع والمواقع المشتركة على هذا الإصدار. يمكن للمشروع تغييرها من لوحته.",
          "بنەڕەتەکان بۆ هەموو پڕۆژەکان و ماڵپەڕە هاوبەشەکان لەسەر ئەم وەشانە. پڕۆژە دەتوانێت لە پانێڵی خۆیدا بیانگۆڕێت."},
    Entry{Str::PhpVersion, "Version", "الإصدار", "وەشان"},
    Entry{Str::PhpNoneInstalled, "No PHP is installed yet. Download one in Runtimes first.",
          "لم يُثبَّت أي " FLO_LRI "PHP" FLO_PDI " بعد. نزّل إصدارًا من بيئات التشغيل أولًا.",
          "هێشتا هیچ " FLO_LRI "PHP" FLO_PDI "یەک دانەمەزراوە. سەرەتا یەکێک لە ژینگەکانی کارپێکردن دابگرە."},
    Entry{Str::PhpGoToRuntimes, "Go to Runtimes", "انتقل إلى بيئات التشغيل", "بڕۆ بۆ ژینگەکانی کارپێکردن"},
    Entry{Str::PhpExtensions, "Extensions", "الإضافات", "پێوەکراوەکان"},
    Entry{Str::PhpExtensionsOn, "· {0} of {1} on", "· {0} من {1} مفعّلة", "· {0} لە {1} چالاکە"},
    Entry{Str::PhpFindExtension, "Find an extension", "ابحث عن إضافة", "گەڕان بۆ پێوەکراوێک"},
    Entry{Str::PhpSlows, "slows PHP", "يبطّئ " FLO_LRI "PHP" FLO_PDI, FLO_LRI "PHP" FLO_PDI " خاو دەکات"},
    Entry{Str::PhpMissing, "not in this PHP", "غير موجودة في هذا " FLO_LRI "PHP" FLO_PDI,
          "لەم " FLO_LRI "PHP" FLO_PDI "یەدا نییە"},
    Entry{Str::PhpOpcacheNote, "opcache is always on (it's what makes PHP fast).",
          FLO_LRI "opcache" FLO_PDI " مفعّلة دائمًا (فهي ما يجعل " FLO_LRI "PHP" FLO_PDI " سريعًا).",
          FLO_LRI "opcache" FLO_PDI " هەمیشە چالاکە (ئەوە " FLO_LRI "PHP" FLO_PDI " خێرا دەکات)."},
    Entry{Str::PhpMoreCount, "{0} more:", "{0} أخرى:", "{0} ی تر:"},
    Entry{Str::PhpShowAll, "show all", "عرض الكل", "هەمووی پیشان بدە"},
    Entry{Str::PhpShowFewer, "show fewer", "عرض أقل", "کەمتر پیشان بدە"},
    Entry{Str::PhpFixedNote, "This PHP comes with its extensions built in, so they can't be changed here.",
          "يأتي هذا " FLO_LRI "PHP" FLO_PDI " بإضافاته مدمجة، لذا لا يمكن تغييرها هنا.",
          "ئەم " FLO_LRI "PHP" FLO_PDI "یە پێوەکراوەکانی تێدا دروستکراوە، بۆیە لێرە ناگۆڕدرێن."},
    Entry{Str::PhpNoMatch, "No extension matches “{0}”", "لا توجد إضافة تطابق «{0}»",
          "هیچ پێوەکراوێک لەگەڵ «{0}» ناگونجێت"},
    Entry{Str::PhpIni, "php.ini", FLO_LRI "php.ini" FLO_PDI, FLO_LRI "php.ini" FLO_PDI},
    Entry{Str::PhpIniHint, "· the settings people change most", "· الإعدادات الأكثر تغييرًا",
          "· ئەو ڕێکخستنانەی زۆرتر دەگۆڕدرێن"},
    Entry{Str::PhpEditFull, "Add your own php.ini lines…", "إضافة أسطرك إلى " FLO_LRI "php.ini" FLO_PDI "…",
          "زیادکردنی دێڕی خۆت بۆ " FLO_LRI "php.ini" FLO_PDI "…"},
    Entry{Str::PhpCustomIniUsed,
          "Your own lines come after these fields and win over them. They're kept for this PHP version and stay "
          "after updates.",
          "تأتي أسطرك بعد هذه الحقول وتتقدّم عليها، وتُحفظ لهذا الإصدار من " FLO_LRI "PHP" FLO_PDI
          " وتبقى بعد التحديثات.",
          "دێڕەکانی خۆت دوای ئەم خانانە دێن و بەسەریاندا زاڵ دەبن؛ بۆ ئەم وەشانەی " FLO_LRI "PHP" FLO_PDI
          " هەڵدەگیرێن و دوای نوێکردنەوەش دەمێننەوە."},
    Entry{Str::PhpReset, "Reset", "إعادة تعيين", "گەڕاندنەوە"},
    Entry{Str::PhpDefaultCue, "default", "الافتراضي", "بنەڕەت"},
    Entry{Str::PhpApplies, "Applies when saved: PHP for {0} restarts (under a second).",
          "يُطبَّق عند الحفظ: يُعاد تشغيل " FLO_LRI "PHP" FLO_PDI " لـ {0} (أقل من ثانية).",
          "دوای پاشەکەوتکردن جێبەجێ دەبێت: " FLO_LRI "PHP" FLO_PDI " بۆ {0} دەست پێ دەکاتەوە (کەمتر لە چرکەیەک)."},
    Entry{Str::PhpAndShared, "{0} and shared sites", "{0} والمواقع المشتركة", "{0} و ماڵپەڕە هاوبەشەکان"},
    Entry{Str::PhpSharedOnly, "shared sites", "المواقع المشتركة", "ماڵپەڕە هاوبەشەکان"},
    Entry{Str::PhpAppliesNobody, "Applies when saved. Nothing uses PHP {0} right now.",
          "يُطبَّق عند الحفظ. لا شيء يستخدم " FLO_LRI "PHP" FLO_PDI " {0} الآن.",
          "دوای پاشەکەوتکردن جێبەجێ دەبێت. ئێستا هیچ شتێک " FLO_LRI "PHP" FLO_PDI " {0} بەکار ناهێنێت."},
    Entry{Str::PhpDiscard, "Discard", "تجاهل", "وازهێنان"},
    Entry{Str::PhpSave, "Save", "حفظ", "پاشەکەوتکردن"},
    Entry{Str::PhpSaving, "Saving…", "جارٍ الحفظ…", "پاشەکەوت دەکرێت…"},
    Entry{Str::PhpSaved, "Saved.", "تم الحفظ.", "پاشەکەوت کرا."},
    Entry{Str::PhpDontSave, "Don't save", "لا تحفظ", "پاشەکەوت مەکە"},
    Entry{Str::PhpAskSave, "Save your changes to PHP {0}?",
          "هل تريد حفظ تغييراتك على " FLO_LRI "PHP" FLO_PDI " {0}؟",
          "گۆڕانکارییەکانت لە " FLO_LRI "PHP" FLO_PDI " {0} پاشەکەوت بکرێن؟"},
    Entry{Str::PhpReadFailed, "Couldn't read the settings of PHP {0}: {1}",
          "تعذّرت قراءة إعدادات " FLO_LRI "PHP" FLO_PDI " {0}: {1}",
          "نەتوانرا ڕێکخستنەکانی " FLO_LRI "PHP" FLO_PDI " {0} بخوێندرێنەوە: {1}"},
    Entry{Str::PhpSaveFailed, "Couldn't save: {0}", "تعذّر الحفظ: {0}", "نەتوانرا پاشەکەوت بکرێت: {0}"},
    Entry{Str::PhpOpenIniFailed, "Couldn't open the php.ini file: {0}",
          "تعذّر فتح ملف " FLO_LRI "php.ini" FLO_PDI ": {0}",
          "نەتوانرا پەڕگەی " FLO_LRI "php.ini" FLO_PDI " بکرێتەوە: {0}"},
    Entry{Str::PhpIniOpened, "Opened in Notepad. FloServer uses your edits a few seconds after you save the file.",
          "فُتح في المفكرة. يستخدم " FLO_LRI "FloServer" FLO_PDI " تعديلاتك بعد ثوانٍ من حفظ الملف.",
          "لە " FLO_LRI "Notepad" FLO_PDI " کرایەوە. " FLO_LRI "FloServer" FLO_PDI
          " چەند چرکەیەک دوای پاشەکەوتکردنی پەڕگەکە دەستکارییەکانت بەکاردەهێنێت."},
    Entry{Str::PhpShowFinal, "Show the final php.ini", "عرض " FLO_LRI "php.ini" FLO_PDI " النهائي",
          "پیشاندانی " FLO_LRI "php.ini" FLO_PDI "ی کۆتایی"},
    Entry{Str::PhpFinalOpened,
          "Opened in Notepad to read: the php.ini this PHP version uses. A project's own changes come after it.",
          "فُتح في المفكرة للقراءة: ملف " FLO_LRI "php.ini" FLO_PDI
          " الذي يستخدمه هذا الإصدار من " FLO_LRI "PHP" FLO_PDI ". تأتي تغييرات المشروع الخاصة بعده.",
          "لە " FLO_LRI "Notepad" FLO_PDI " بۆ خوێندنەوە کرایەوە: ئەو " FLO_LRI "php.ini" FLO_PDI
          "ـەی ئەم وەشانەی " FLO_LRI "PHP" FLO_PDI " بەکاری دەهێنێت. گۆڕانکارییەکانی خودی پرۆژە دوای ئەو دێن."},
    Entry{Str::PhpProblems, "Some PHP settings were skipped: {0}", "تم تخطي بعض إعدادات " FLO_LRI "PHP" FLO_PDI ": {0}",
          "هەندێک ڕێکخستنی " FLO_LRI "PHP" FLO_PDI " پشتگوێ خران: {0}"},
    Entry{Str::PanelPhpSettings, "PHP settings", "إعدادات " FLO_LRI "PHP" FLO_PDI, "ڕێکخستنەکانی " FLO_LRI "PHP" FLO_PDI},
    Entry{Str::ProjectPhpOpenSettings, "Open settings for PHP {0}", "فتح إعدادات " FLO_LRI "PHP" FLO_PDI " {0}",
          "کردنەوەی ڕێکخستنەکانی " FLO_LRI "PHP" FLO_PDI " {0}"},
    Entry{Str::ProjectPhpExtHint, "· the PHP {0} defaults, with this project's changes",
          "· افتراضيات " FLO_LRI "PHP" FLO_PDI " {0} مع تغييرات هذا المشروع",
          "· بنەڕەتەکانی " FLO_LRI "PHP" FLO_PDI " {0} لەگەڵ گۆڕانکارییەکانی ئەم پڕۆژەیە"},
    Entry{Str::ProjectPhpNoChanges, "No changes", "لا تغييرات", "هیچ گۆڕانکارییەک نییە"},
    Entry{Str::ProjectPhpChange, "Change…", "تغيير…", "گۆڕین…"},
    Entry{Str::ProjectPhpIniHint, "· empty = the PHP {0} default (shown grey)",
          "· فارغ = افتراضي " FLO_LRI "PHP" FLO_PDI " {0} (بالرمادي)",
          "· بەتاڵ = بنەڕەتی " FLO_LRI "PHP" FLO_PDI " {0} (بە خۆڵەمێشی)"},
    Entry{Str::ProjectPhpDefaultChoice, "Default ({0})", "الافتراضي ({0})", "بنەڕەت ({0})"},
    Entry{Str::ProjectPhpAnother, "Another setting", "إعداد آخر", "ڕێکخستنێکی تر"},
    Entry{Str::ProjectPhpAnotherCue, "name = value", FLO_LRI "name = value" FLO_PDI, FLO_LRI "name = value" FLO_PDI},
    Entry{Str::ProjectPhpSavedIn,
          "Saved in the project's {0}, so the project takes them with it (to Git, to another PC). Only this project's PHP restarts.",
          "تُحفظ في {0} الخاص بالمشروع، فتنتقل معه (إلى " FLO_LRI "Git" FLO_PDI "، أو إلى حاسوب آخر). يُعاد تشغيل " FLO_LRI "PHP" FLO_PDI " لهذا المشروع فقط.",
          "لە {0}ی پڕۆژەکەدا پاشەکەوت دەکرێن، بۆیە لەگەڵ پڕۆژەکە دەڕۆن (بۆ " FLO_LRI "Git" FLO_PDI "، بۆ کۆمپیوتەرێکی تر). تەنها " FLO_LRI "PHP" FLO_PDI "ی ئەم پڕۆژەیە دەست پێ دەکاتەوە."},
    Entry{Str::ProjectPhpReset, "Reset to defaults", "إعادة إلى الافتراضيات", "گەڕاندنەوە بۆ بنەڕەتەکان"},
    Entry{Str::ProjectPhpAskSave, "Save the PHP changes for {0}?", "هل تريد حفظ تغييرات " FLO_LRI "PHP" FLO_PDI " لـ {0}؟",
          "گۆڕانکارییەکانی " FLO_LRI "PHP" FLO_PDI " بۆ {0} پاشەکەوت بکرێن؟"},
    Entry{Str::ProjectPhpNoSeries, "No PHP is installed for this project yet.",
          "لم يُثبَّت " FLO_LRI "PHP" FLO_PDI " لهذا المشروع بعد.",
          "هێشتا هیچ " FLO_LRI "PHP" FLO_PDI "یەک بۆ ئەم پڕۆژەیە دانەمەزراوە."},
    Entry{Str::ProjectPhpBadLine, "Write another setting as name = value.",
          "اكتب الإعداد الآخر بالشكل " FLO_LRI "name = value" FLO_PDI ".",
          "ڕێکخستنی تر بەم شێوەیە بنووسە: " FLO_LRI "name = value" FLO_PDI "."},
    Entry{Str::PhpUnknownIni, "PHP {0} has no setting named {1}, so it would be ignored.",
          "لا يوجد في " FLO_LRI "PHP" FLO_PDI " {0} إعداد باسم {1}، لذا سيُتجاهَل.",
          FLO_LRI "PHP" FLO_PDI " {0} هیچ ڕێکخستنێکی بە ناوی {1} نییە، بۆیە پشتگوێ دەخرێت."},
    Entry{Str::PhpUnknownIniMaybe, "PHP {0} has no setting named {1}. Did you mean {2}?",
          "لا يوجد في " FLO_LRI "PHP" FLO_PDI " {0} إعداد باسم {1}. هل تقصد {2}؟",
          FLO_LRI "PHP" FLO_PDI " {0} هیچ ڕێکخستنێکی بە ناوی {1} نییە. مەبەستت {2} بوو؟"},
    Entry{Str::PanelTabGeneral, "General", "عام", "گشتی"},
    Entry{Str::PanelFolder, "Project folder", "مجلد المشروع", "بوخچەی پڕۆژە"},
    // Settings > Ports & HTTPS (D20). Arabic and Kurdish need a native read.
    Entry{Str::SettingsPorts, "Ports & HTTPS",
          "المنافذ و" FLO_LRI "HTTPS" FLO_PDI,
          "پۆرتەکان و " FLO_LRI "HTTPS" FLO_PDI},
    Entry{Str::PortsHint, "Where FloServer's web server listens, and HTTPS for your projects. Changes take effect at once.",
          "المنافذ التي يستمع عليها خادم الويب في " FLO_LRI "FloServer" FLO_PDI "، و" FLO_LRI "HTTPS" FLO_PDI " لمشاريعك. تُطبَّق التغييرات فورًا.",
          "ئەو پۆرتانەی سێرڤەری وێبی " FLO_LRI "FloServer" FLO_PDI " گوێیان لێ دەگرێت، و " FLO_LRI "HTTPS" FLO_PDI " بۆ پڕۆژەکانت. گۆڕانکارییەکان یەکسەر جێبەجێ دەبن."},
    Entry{Str::PortsWebTitle, "Web ports", "منافذ الويب", "پۆرتەکانی وێب"},
    Entry{Str::PortsAuto, "Automatic", "تلقائي", "خۆکار"},
    Entry{Str::PortsAutoNote, "80 and 443 when they're free, else 8080 and 8443.",
          FLO_LRI "80" FLO_PDI " و" FLO_LRI "443" FLO_PDI " عندما تكونان متاحتين، وإلا " FLO_LRI "8080" FLO_PDI " و" FLO_LRI "8443" FLO_PDI ".",
          FLO_LRI "80" FLO_PDI " و " FLO_LRI "443" FLO_PDI " کاتێک بەتاڵن، ئەگەرنا " FLO_LRI "8080" FLO_PDI " و " FLO_LRI "8443" FLO_PDI "."},
    Entry{Str::PortsStandard, "80 and 443",
          FLO_LRI "80" FLO_PDI " و" FLO_LRI "443" FLO_PDI,
          FLO_LRI "80" FLO_PDI " و " FLO_LRI "443" FLO_PDI},
    Entry{Str::PortsStandardNote, "Addresses without a port, such as {0}.",
          "عناوين بلا منفذ، مثل {0}.",
          "ناونیشان بەبێ پۆرت، وەک {0}."},
    Entry{Str::PortsAlternate, "8080 and 8443",
          FLO_LRI "8080" FLO_PDI " و" FLO_LRI "8443" FLO_PDI,
          FLO_LRI "8080" FLO_PDI " و " FLO_LRI "8443" FLO_PDI},
    Entry{Str::PortsAlternateNote, "Addresses with the port, such as {0}.",
          "عناوين مع المنفذ، مثل {0}.",
          "ناونیشان لەگەڵ پۆرت، وەک {0}."},
    Entry{Str::PortsCustom, "Other ports", "منافذ أخرى", "پۆرتی تر"},
    Entry{Str::PortsCustomNote, "Two port numbers of your own.",
          "رقما منفذين تختارهما بنفسك.",
          "دوو ژمارەی پۆرت کە خۆت هەڵیان دەبژێریت."},
    Entry{Str::PortsHttpLabel, "http port", "منفذ " FLO_LRI "http" FLO_PDI, "پۆرتی " FLO_LRI "http" FLO_PDI},
    Entry{Str::PortsHttpsLabel, "https port", "منفذ " FLO_LRI "https" FLO_PDI, "پۆرتی " FLO_LRI "https" FLO_PDI},
    Entry{Str::PortsUse, "Use these ports", "استخدم هذين المنفذين", "ئەم پۆرتانە بەکاربهێنە"},
    Entry{Str::PortsBadNumber, "Type port numbers from 1 to 65535.",
          "اكتب أرقام منافذ من " FLO_LRI "1" FLO_PDI " إلى " FLO_LRI "65535" FLO_PDI ".",
          "ژمارەی پۆرت لە " FLO_LRI "1" FLO_PDI " تا " FLO_LRI "65535" FLO_PDI " بنووسە."},
    Entry{Str::PortsSameNumber, "The http port and the https port must be different.",
          "يجب أن يختلف منفذ " FLO_LRI "http" FLO_PDI " عن منفذ " FLO_LRI "https" FLO_PDI ".",
          "پۆرتی " FLO_LRI "http" FLO_PDI " و پۆرتی " FLO_LRI "https" FLO_PDI " دەبێت جیاواز بن."},
    Entry{Str::PortsInUse, "In use now: http {0}, https {1}.",
          "المستخدم الآن: " FLO_LRI "http" FLO_PDI " {0}، " FLO_LRI "https" FLO_PDI " {1}.",
          "ئێستا بەکاردێت: " FLO_LRI "http" FLO_PDI " {0}، " FLO_LRI "https" FLO_PDI " {1}."},
    Entry{Str::PortsNotRunning, "Nothing is served now, so no port is in use.",
          "لا يُقدَّم شيء الآن، فلا يوجد منفذ مستخدم.",
          "ئێستا هیچ شتێک پێشکەش ناکرێت، بۆیە هیچ پۆرتێک بەکارنایەت."},
    Entry{Str::PortsTakenBy, "Port {0} is in use by {1}.",
          "المنفذ {0} يستخدمه {1}.",
          "پۆرتی {0} لەلایەن {1} بەکاردێت."},
    Entry{Str::PortsTakenByPid, "Port {0} is in use by another program (process {1}).",
          "المنفذ {0} يستخدمه برنامج آخر (العملية {1}).",
          "پۆرتی {0} لەلایەن بەرنامەیەکی ترەوە بەکاردێت (پرۆسەی {1})."},
    Entry{Str::PortsTaken, "Port {0} is in use by another program.",
          "المنفذ {0} يستخدمه برنامج آخر.",
          "پۆرتی {0} لەلایەن بەرنامەیەکی ترەوە بەکاردێت."},
    Entry{Str::PortsMovesLater, "FloServer uses other ports until those are free, then moves to them by itself.",
          "يستخدم " FLO_LRI "FloServer" FLO_PDI " منافذ أخرى حتى تصبح تلك متاحة، ثم ينتقل إليها تلقائيًا.",
          FLO_LRI "FloServer" FLO_PDI " پۆرتی تر بەکاردەهێنێت تا ئەوانە بەتاڵ دەبن، پاشان خۆی دەچێتە سەریان."},
    Entry{Str::PortsHttpsTitle, "HTTPS", FLO_LRI "HTTPS" FLO_PDI, FLO_LRI "HTTPS" FLO_PDI},
    Entry{Str::PortsHttpsProjects, "HTTPS for projects",
          FLO_LRI "HTTPS" FLO_PDI " للمشاريع",
          FLO_LRI "HTTPS" FLO_PDI " بۆ پڕۆژەکان"},
    Entry{Str::PortsHttpsProjectsNote, "Projects open at {0}, and their http addresses go there too.",
          "تُفتح المشاريع على {0}، وتنتقل عناوين " FLO_LRI "http" FLO_PDI " الخاصة بها إليه أيضًا.",
          "پڕۆژەکان لە {0} دەکرێنەوە، و ناونیشانە " FLO_LRI "http" FLO_PDI " ـەکانیشیان دەچنە ئەوێ."},
    Entry{Str::PortsHttpsShared, "Shared sites over HTTPS",
          "المواقع المشتركة عبر " FLO_LRI "HTTPS" FLO_PDI,
          "ماڵپەڕە هاوبەشەکان بە " FLO_LRI "HTTPS" FLO_PDI},
    Entry{Str::PortsHttpsSharedNote, "Only {0} gets HTTPS; {1} stays http.",
          "يحصل {0} وحده على " FLO_LRI "HTTPS" FLO_PDI "؛ ويبقى {1} على " FLO_LRI "http" FLO_PDI ".",
          "تەنها {0} " FLO_LRI "HTTPS" FLO_PDI " وەردەگرێت؛ {1} بە " FLO_LRI "http" FLO_PDI " دەمێنێتەوە."},
    Entry{Str::PortsNeedsTrust, "HTTPS starts once Windows trusts FloServer's certificate authority, below.",
          "يبدأ " FLO_LRI "HTTPS" FLO_PDI " بعد أن يثق " FLO_LRI "Windows" FLO_PDI " بجهة الشهادات في " FLO_LRI "FloServer" FLO_PDI " أدناه.",
          FLO_LRI "HTTPS" FLO_PDI " دەست پێ دەکات کاتێک " FLO_LRI "Windows" FLO_PDI " متمانە بە دەسەڵاتی بڕوانامەی " FLO_LRI "FloServer" FLO_PDI " بکات، لە خوارەوە."},
    Entry{Str::PortsCaTitle, "FloServer's certificate authority",
          "جهة الشهادات في " FLO_LRI "FloServer" FLO_PDI,
          "دەسەڵاتی بڕوانامەی " FLO_LRI "FloServer" FLO_PDI},
    Entry{Str::PortsCaTrusted, "Windows trusts it, so browsers accept your https addresses.",
          "يثق بها " FLO_LRI "Windows" FLO_PDI "، لذا تقبل المتصفحات عناوين " FLO_LRI "https" FLO_PDI " الخاصة بك.",
          FLO_LRI "Windows" FLO_PDI " متمانەی پێیەتی، بۆیە وێبگەڕەکان ناونیشانە " FLO_LRI "https" FLO_PDI " ـەکانت قبوڵ دەکەن."},
    Entry{Str::PortsCaNotTrusted, "Windows doesn't trust it yet, so projects open on http.",
          "لا يثق بها " FLO_LRI "Windows" FLO_PDI " بعد، لذا تُفتح المشاريع على " FLO_LRI "http" FLO_PDI ".",
          FLO_LRI "Windows" FLO_PDI " هێشتا متمانەی پێی نییە، بۆیە پڕۆژەکان بە " FLO_LRI "http" FLO_PDI " دەکرێنەوە."},
    Entry{Str::PortsCaNone, "There isn't one yet. Trust makes it and adds it to Windows.",
          "لا توجد بعد. «الثقة بها» تنشئها وتضيفها إلى " FLO_LRI "Windows" FLO_PDI ".",
          "هێشتا نییە. «متمانەپێکردن» دروستی دەکات و زیادی دەکات بۆ " FLO_LRI "Windows" FLO_PDI "."},
    Entry{Str::PortsCaDamaged, "It can't be read: {0}", "تعذّرت قراءتها: {0}", "ناخوێندرێتەوە: {0}"},
    Entry{Str::PortsCaSafe, "It only works for localhost addresses, so it can't be misused for real websites.",
          "تعمل فقط مع عناوين " FLO_LRI "localhost" FLO_PDI "، لذا لا يمكن إساءة استخدامها لمواقع حقيقية.",
          "تەنها بۆ ناونیشانەکانی " FLO_LRI "localhost" FLO_PDI " کار دەکات، بۆیە ناتوانرێت بۆ ماڵپەڕی ڕاستەقینە خراپ بەکاربهێنرێت."},
    Entry{Str::PortsTrust, "Trust", "الثقة بها", "متمانەپێکردن"},
    Entry{Str::PortsOpenCert, "Open the certificate", "فتح الشهادة", "کردنەوەی بڕوانامە"},
    Entry{Str::PortsTrustDone, "Added. Restart a browser that was open.",
          "تمت الإضافة. أعد تشغيل أي متصفح كان مفتوحًا.",
          "زیاد کرا. ئەو وێبگەڕەی کراوە بوو دووبارە بیکەرەوە."},
    Entry{Str::PortsTrustNo, "Not added: you said no to Windows' question.",
          "لم تتم الإضافة: رفضتَ سؤال " FLO_LRI "Windows" FLO_PDI ".",
          "زیاد نەکرا: وەڵامی پرسیارەکەی " FLO_LRI "Windows" FLO_PDI " ـت بە نەخێر دایەوە."},
    Entry{Str::PortsTrustFailed, "Not added: {0}", "لم تتم الإضافة: {0}", "زیاد نەکرا: {0}"},
    Entry{Str::PortsRemoveDone, "Removed from Windows. Projects open on http now.",
          "أُزيلت من " FLO_LRI "Windows" FLO_PDI ". تُفتح المشاريع على " FLO_LRI "http" FLO_PDI " الآن.",
          "لە " FLO_LRI "Windows" FLO_PDI " سڕایەوە. پڕۆژەکان ئێستا بە " FLO_LRI "http" FLO_PDI " دەکرێنەوە."},
    Entry{Str::PortsRemoveNo, "Not removed: you said no to Windows' question.",
          "لم تتم الإزالة: رفضتَ سؤال " FLO_LRI "Windows" FLO_PDI ".",
          "نەسڕایەوە: وەڵامی پرسیارەکەی " FLO_LRI "Windows" FLO_PDI " ـت بە نەخێر دایەوە."},
    Entry{Str::PortsRemoveFailed, "Not removed: {0}", "لم تتم الإزالة: {0}", "نەسڕایەوە: {0}"},
    Entry{Str::PortsChangeFailed, "Couldn't change it: {0}", "تعذّر التغيير: {0}", "نەتوانرا بگۆڕدرێت: {0}"},
    Entry{Str::PortsReadFailed, "Couldn't read the settings: {0}",
          "تعذّرت قراءة الإعدادات: {0}",
          "نەتوانرا ڕێکخستنەکان بخوێندرێنەوە: {0}"},
    Entry{Str::PortsOpenFailed, "Couldn't open the certificate: {0}",
          "تعذّر فتح الشهادة: {0}",
          "نەتوانرا بڕوانامەکە بکرێتەوە: {0}"},
    Entry{Str::PortsChanging, "Changing…", "جارٍ التغيير…", "دەگۆڕدرێت…"},
    Entry{Str::PortsCertUnreadable, "the certificate can't be read", "تعذّرت قراءة الشهادة", "بڕوانامەکە ناخوێندرێتەوە"},
    // Settings > Database (MariaDB). Arabic and Kurdish need a native read.
    Entry{Str::SettingsDatabase, "Database", "قاعدة البيانات", "بنکەی زانیاری"},
    Entry{Str::DbTitle, "Database (MariaDB)", "قاعدة البيانات (" FLO_LRI "MariaDB" FLO_PDI ")",
          "بنکەی زانیاری (" FLO_LRI "MariaDB" FLO_PDI ")"},
    Entry{Str::DbHint,
          "The MariaDB server for every project and shared sites on this version: its port, whether it sleeps, its "
          "my.ini and root's password.",
          "خادم " FLO_LRI "MariaDB" FLO_PDI " لكل المشاريع والمواقع المشتركة على هذا الإصدار: منفذه، وهل ينام، وملف "
          FLO_LRI "my.ini" FLO_PDI " وكلمة مرور " FLO_LRI "root" FLO_PDI ".",
          "ڕاژەکاری " FLO_LRI "MariaDB" FLO_PDI " بۆ هەموو پڕۆژەکان و ماڵپەڕە هاوبەشەکان لەسەر ئەم وەشانە: پۆرتەکەی، "
          "ئایا دەخەوێت، " FLO_LRI "my.ini" FLO_PDI " و وشەی نهێنیی " FLO_LRI "root" FLO_PDI "."},
    Entry{Str::DbNoneInstalled, "No MariaDB is installed yet. Download one in Runtimes first.",
          "لم يُثبَّت أي " FLO_LRI "MariaDB" FLO_PDI " بعد. نزّل إصدارًا من بيئات التشغيل أولًا.",
          "هێشتا هیچ " FLO_LRI "MariaDB" FLO_PDI "یەک دانەمەزراوە. سەرەتا یەکێک لە ژینگەکانی کارپێکردن دابگرە."},
    Entry{Str::DbServer, "Server", "الخادم", "ڕاژەکار"},
    Entry{Str::DbStart, "Start", "تشغيل", "دەستپێکردن"},
    Entry{Str::DbStop, "Stop", "إيقاف", "وەستاندن"},
    Entry{Str::DbRestart, "Restart", "إعادة التشغيل", "دەستپێکردنەوە"},
    Entry{Str::DbSleeping, "Sleeping on port {0}: it starts when a project connects.",
          "نائم على المنفذ {0}: يبدأ عندما يتصل به مشروع.",
          "لەسەر پۆرتی {0} خەوتووە: کاتێک پڕۆژەیەک پەیوەندی پێوە بکات دەست پێ دەکات."},
    Entry{Str::DbStopping, "Stopping…", "جارٍ الإيقاف…", "دەوەستێندرێت…"},
    Entry{Str::DbFailed, "Couldn't start: {0}", "تعذّر التشغيل: {0}", "نەتوانرا دەست پێ بکات: {0}"},
    Entry{Str::DbNotInUse,
          "Not ready yet: FloServer sets up its server a few seconds after it's installed.",
          "غير جاهز بعد: يجهّز " FLO_LRI "FloServer" FLO_PDI " خادمه بعد ثوانٍ من تثبيته.",
          "هێشتا ئامادە نییە: " FLO_LRI "FloServer" FLO_PDI " چەند چرکەیەک دوای دامەزراندنی ڕاژەکارەکەی ئامادە دەکات."},
    Entry{Str::DbPortTaken, "Port {0} is in use by another program, so it's on port {1} for now.",
          "المنفذ {0} يستخدمه برنامج آخر، لذا يعمل على المنفذ {1} حاليًا.",
          "پۆرتی {0} لەلایەن بەرنامەیەکی ترەوە بەکاردێت، بۆیە ئێستا لەسەر پۆرتی {1}ە."},
    Entry{Str::DbPort, "Port", "المنفذ", "پۆرت"},
    Entry{Str::DbPortCue, "automatic", "تلقائي", "خۆکار"},
    Entry{Str::DbPortBad, "The port is a number from 1024 to 65535.", "المنفذ رقم من 1024 إلى 65535.",
          "پۆرت ژمارەیەکە لە 1024 تا 65535."},
    Entry{Str::DbPortNote,
          "Projects' .env files keep the old port: copy the new lines from each project's Database tab.",
          "تحتفظ ملفات " FLO_LRI ".env" FLO_PDI " في المشاريع بالمنفذ القديم: انسخ الأسطر الجديدة من تبويب قاعدة البيانات في كل مشروع.",
          "پەڕگەکانی " FLO_LRI ".env" FLO_PDI "ی پڕۆژەکان پۆرتە کۆنەکە دەهێڵنەوە: دێڕە نوێیەکان لە تابی بنکەی زانیاریی هەر پڕۆژەیەک کۆپی بکە."},
    Entry{Str::DbKeepRunning, "Keep running", "إبقاء التشغيل", "بەردەوام کارکردن"},
    Entry{Str::DbKeepRunningNote,
          "Off: it sleeps when no project has used it for a while, and wakes on the next connection.",
          "عند الإيقاف: ينام إذا لم يستخدمه أي مشروع لفترة، ويستيقظ عند الاتصال التالي.",
          "کاتێک ناچالاکە: ئەگەر بۆ ماوەیەک هیچ پڕۆژەیەک بەکاری نەهێنا دەخەوێت، و بە پەیوەندیی داهاتوو بێدار دەبێتەوە."},
    Entry{Str::DbKeptBy, "Kept running by {0} (each project's own setting).",
          "يبقى قيد التشغيل بسبب {0} (إعداد كل مشروع).",
          "بەهۆی {0} بەردەوام کار دەکات (ڕێکخستنی خودی هەر پڕۆژەیەک)."},
    Entry{Str::DbUsedBy, "Used by {0}.", "يستخدمه {0}.", "بەکاردێت لەلایەن {0}."},
    Entry{Str::DbUsedByNobody, "No project uses it.", "لا يستخدمه أي مشروع.", "هیچ پڕۆژەیەک بەکاری ناهێنێت."},
    Entry{Str::DbShowFinal, "Show the final my.ini", "عرض " FLO_LRI "my.ini" FLO_PDI " النهائي",
          "پیشاندانی " FLO_LRI "my.ini" FLO_PDI "ی کۆتایی"},
    Entry{Str::DbEditFull, "Add your own my.ini lines…", "إضافة أسطرك إلى " FLO_LRI "my.ini" FLO_PDI "…",
          "زیادکردنی دێڕی خۆت بۆ " FLO_LRI "my.ini" FLO_PDI "…"},
    Entry{Str::DbUnknownOption, "custom.ini: MariaDB {0} has no option named {1}, so it won't start with it.",
          FLO_LRI "custom.ini" FLO_PDI ": لا يوجد في " FLO_LRI "MariaDB" FLO_PDI " {0} خيار باسم {1}، لذا لن يبدأ به.",
          FLO_LRI "custom.ini" FLO_PDI ": " FLO_LRI "MariaDB" FLO_PDI " {0} هیچ هەڵبژاردەیەکی بە ناوی {1} نییە، بۆیە بەوەوە دەست پێ ناکات."},
    Entry{Str::DbUnknownOptionMaybe, "custom.ini: MariaDB {0} has no option named {1}. Did you mean {2}?",
          FLO_LRI "custom.ini" FLO_PDI ": لا يوجد في " FLO_LRI "MariaDB" FLO_PDI " {0} خيار باسم {1}. هل تقصد {2}؟",
          FLO_LRI "custom.ini" FLO_PDI ": " FLO_LRI "MariaDB" FLO_PDI " {0} هیچ هەڵبژاردەیەکی بە ناوی {1} نییە. مەبەستت {2} بوو؟"},
    Entry{Str::DbCustomProblems, "Some lines of custom.ini are left out: {0}",
          "تم تجاهل بعض أسطر " FLO_LRI "custom.ini" FLO_PDI ": {0}",
          "هەندێک دێڕی " FLO_LRI "custom.ini" FLO_PDI " پشتگوێ خران: {0}"},
    Entry{Str::DbCustomIniUsed,
          "Your own lines come after these fields and win over them. The server uses them the next time it starts.",
          "تأتي أسطرك بعد هذه الحقول وتتقدّم عليها، ويستخدمها الخادم في المرة التالية التي يبدأ فيها.",
          "دێڕەکانی خۆت دوای ئەم خانانە دێن و بەسەریاندا زاڵ دەبن؛ ڕاژەکار جاری داهاتوو کە دەست پێ دەکات بەکاریان دەهێنێت."},
    Entry{Str::DbProblems, "Some database settings were skipped: {0}", "تم تخطي بعض إعدادات قاعدة البيانات: {0}",
          "هەندێک ڕێکخستنی بنکەی زانیاری پشتگوێ خران: {0}"},
    Entry{Str::DbRoot, "Root access", "صلاحيات " FLO_LRI "root" FLO_PDI, "دەستگەیشتنی " FLO_LRI "root" FLO_PDI},
    Entry{Str::DbRootPassword, "root's password", "كلمة مرور " FLO_LRI "root" FLO_PDI,
          "وشەی نهێنیی " FLO_LRI "root" FLO_PDI},
    Entry{Str::DbShow, "Show", "إظهار", "پیشاندان"},
    Entry{Str::DbHide, "Hide", "إخفاء", "شاردنەوە"},
    Entry{Str::DbCopy, "Copy", "نسخ", "کۆپی"},
    Entry{Str::DbNewPassword, "New password", "كلمة مرور جديدة", "وشەی نهێنیی نوێ"},
    Entry{Str::DbNewPasswordCue, "at least 8 characters", "8 أحرف على الأقل", "لانیکەم 8 پیت"},
    Entry{Str::DbSetPassword, "Set password", "تعيين كلمة المرور", "دانانی وشەی نهێنی"},
    Entry{Str::DbRootNote,
          "root can do anything in this MariaDB. Projects use their own logins; use root only in database tools.",
          "يستطيع " FLO_LRI "root" FLO_PDI " فعل أي شيء في " FLO_LRI "MariaDB" FLO_PDI
          " هذا. تستخدم المشاريع حساباتها الخاصة؛ استخدم " FLO_LRI "root" FLO_PDI " في أدوات قواعد البيانات فقط.",
          FLO_LRI "root" FLO_PDI " دەتوانێت هەموو شتێک لەم " FLO_LRI "MariaDB" FLO_PDI
          "ـە بکات. پڕۆژەکان چوونەژوورەوەی خۆیان بەکاردەهێنن؛ " FLO_LRI "root" FLO_PDI " تەنها لە ئامرازەکانی بنکەی زانیاریدا بەکاربهێنە."},
    Entry{Str::DbDatabases, "Own databases here: {0}", "قواعد بيانات خاصة هنا: {0}", "بنکەی زانیاریی تایبەت لێرە: {0}"},
    Entry{Str::DbNoDatabases, "No project has its own database here yet.", "لا يملك أي مشروع قاعدة بيانات خاصة هنا بعد.",
          "هێشتا هیچ پڕۆژەیەک بنکەی زانیاریی تایبەتی خۆی لێرە نییە."},
    Entry{Str::DbPasswordSet, "root's password was changed. Database tools that saved the old one need the new one.",
          "تم تغيير كلمة مرور " FLO_LRI "root" FLO_PDI ". أدوات قواعد البيانات التي حفظت القديمة تحتاج الجديدة.",
          "وشەی نهێنیی " FLO_LRI "root" FLO_PDI " گۆڕا. ئەو ئامرازانەی بنکەی زانیاری کە کۆنەکەیان پاشەکەوت کردووە پێویستیان بە نوێیەکەیە."},
    Entry{Str::DbPasswordCopied, "root's password is on the clipboard.",
          "كلمة مرور " FLO_LRI "root" FLO_PDI " في الحافظة.",
          "وشەی نهێنیی " FLO_LRI "root" FLO_PDI " لە کلیپبۆردە."},
    Entry{Str::DbPasswordFailed, "Couldn't change root's password: {0}",
          "تعذّر تغيير كلمة مرور " FLO_LRI "root" FLO_PDI ": {0}",
          "نەتوانرا وشەی نهێنیی " FLO_LRI "root" FLO_PDI " بگۆڕدرێت: {0}"},
    Entry{Str::DbRootReadFailed, "Couldn't read root's password: {0}",
          "تعذّرت قراءة كلمة مرور " FLO_LRI "root" FLO_PDI ": {0}",
          "نەتوانرا وشەی نهێنیی " FLO_LRI "root" FLO_PDI " بخوێندرێتەوە: {0}"},
    Entry{Str::DbPasswordShort, "The password needs at least 8 characters.", "تحتاج كلمة المرور إلى 8 أحرف على الأقل.",
          "وشەی نهێنی لانیکەم 8 پیتی پێویستە."},
    Entry{Str::DbPasswordLong, "The password can have at most 100 characters.", "يمكن أن تحتوي كلمة المرور على 100 حرف كحد أقصى.",
          "وشەی نهێنی دەتوانێت زۆرترین 100 پیتی هەبێت."},
    Entry{Str::DbPasswordChars, "Use English letters, digits and symbols, with no spaces.",
          "استخدم أحرفًا إنجليزية وأرقامًا ورموزًا، دون مسافات.",
          "پیتی ئینگلیزی و ژمارە و هێما بەکاربهێنە، بێ بۆشایی."},
    Entry{Str::DbApplies, "Applies when saved: MariaDB {0} restarts (a few seconds) for {1}.",
          "يُطبَّق عند الحفظ: يُعاد تشغيل " FLO_LRI "MariaDB" FLO_PDI " {0} (بضع ثوانٍ) لـ {1}.",
          "دوای پاشەکەوتکردن جێبەجێ دەبێت: " FLO_LRI "MariaDB" FLO_PDI " {0} بۆ {1} دەست پێ دەکاتەوە (چەند چرکەیەک)."},
    Entry{Str::DbAppliesSleeping, "Applies when saved. MariaDB {0} isn't running now: it uses them the next time it starts.",
          "يُطبَّق عند الحفظ. " FLO_LRI "MariaDB" FLO_PDI " {0} لا يعمل الآن: سيستخدمها في المرة التالية التي يبدأ فيها.",
          "دوای پاشەکەوتکردن جێبەجێ دەبێت. " FLO_LRI "MariaDB" FLO_PDI " {0} ئێستا کار ناکات: جاری داهاتوو کە دەست پێ دەکات بەکاریان دەهێنێت."},
    Entry{Str::DbAppliesNobody, "Applies when saved. Nothing uses MariaDB {0} right now.",
          "يُطبَّق عند الحفظ. لا شيء يستخدم " FLO_LRI "MariaDB" FLO_PDI " {0} الآن.",
          "دوای پاشەکەوتکردن جێبەجێ دەبێت. ئێستا هیچ شتێک " FLO_LRI "MariaDB" FLO_PDI " {0} بەکار ناهێنێت."},
    Entry{Str::DbAskSave, "Save your changes to MariaDB {0}?",
          "هل تريد حفظ تغييراتك على " FLO_LRI "MariaDB" FLO_PDI " {0}؟",
          "گۆڕانکارییەکانت لە " FLO_LRI "MariaDB" FLO_PDI " {0} پاشەکەوت بکرێن؟"},
    Entry{Str::DbReadFailed, "Couldn't read the settings of MariaDB {0}: {1}",
          "تعذّرت قراءة إعدادات " FLO_LRI "MariaDB" FLO_PDI " {0}: {1}",
          "نەتوانرا ڕێکخستنەکانی " FLO_LRI "MariaDB" FLO_PDI " {0} بخوێندرێنەوە: {1}"},
    Entry{Str::DbRestarting, "Saved. MariaDB {0} is restarting.",
          "تم الحفظ. يُعاد تشغيل " FLO_LRI "MariaDB" FLO_PDI " {0}.",
          "پاشەکەوت کرا. " FLO_LRI "MariaDB" FLO_PDI " {0} دەست پێ دەکاتەوە."},
    Entry{Str::DbServerFailed, "MariaDB {0}: {1}", FLO_LRI "MariaDB" FLO_PDI " {0}: {1}",
          FLO_LRI "MariaDB" FLO_PDI " {0}: {1}"},
    Entry{Str::DbOpenIniFailed, "Couldn't open the my.ini file: {0}",
          "تعذّر فتح ملف " FLO_LRI "my.ini" FLO_PDI ": {0}",
          "نەتوانرا پەڕگەی " FLO_LRI "my.ini" FLO_PDI " بکرێتەوە: {0}"},
    Entry{Str::DbIniOpened, "Opened in Notepad. Save the file, then Restart the server to use it.",
          "فُتح في المفكرة. احفظ الملف، ثم أعد تشغيل الخادم لاستخدامه.",
          "لە " FLO_LRI "Notepad" FLO_PDI " کرایەوە. پەڕگەکە پاشەکەوت بکە، پاشان ڕاژەکار دەستپێبکەرەوە بۆ بەکارهێنانی."},
    Entry{Str::DbFinalOpened, "Opened in Notepad to read: the my.ini this MariaDB uses.",
          "فُتح في المفكرة للقراءة: ملف " FLO_LRI "my.ini" FLO_PDI " الذي يستخدمه " FLO_LRI "MariaDB" FLO_PDI " هذا.",
          "لە " FLO_LRI "Notepad" FLO_PDI " بۆ خوێندنەوە کرایەوە: ئەو " FLO_LRI "my.ini" FLO_PDI "ـەی ئەم " FLO_LRI "MariaDB" FLO_PDI "ـە بەکاری دەهێنێت."},
    // phpMyAdmin (an optional tool).
    Entry{Str::SettingsTools, "Tools", "الأدوات", "ئامرازەکان"},
    Entry{Str::PmaOpen, "Open phpMyAdmin", "فتح " FLO_LRI "phpMyAdmin" FLO_PDI,
          FLO_LRI "phpMyAdmin" FLO_PDI " بکەرەوە"},
    Entry{Str::PmaOpenProject, "Open in phpMyAdmin", "فتح في " FLO_LRI "phpMyAdmin" FLO_PDI,
          "لە " FLO_LRI "phpMyAdmin" FLO_PDI " بیکەرەوە"},
    Entry{Str::PmaNotInstalled, "Download phpMyAdmin in Settings → Runtimes",
          "نزّل " FLO_LRI "phpMyAdmin" FLO_PDI " من الإعدادات ← بيئات التشغيل",
          FLO_LRI "phpMyAdmin" FLO_PDI " لە ڕێکخستنەکان ← ژینگەکانی کارپێکردن دابگرە"},
    Entry{Str::PmaOpening, "Opening phpMyAdmin…", "جارٍ فتح " FLO_LRI "phpMyAdmin" FLO_PDI "…",
          FLO_LRI "phpMyAdmin" FLO_PDI " دەکرێتەوە…"},
    Entry{Str::PmaStarting, "Starting MariaDB {0}…", "جارٍ تشغيل " FLO_LRI "MariaDB {0}" FLO_PDI "…",
          FLO_LRI "MariaDB {0}" FLO_PDI " دەست پێ دەکات…"},
    Entry{Str::PmaOpenFailed, "phpMyAdmin couldn't be opened: {0}", "تعذّر فتح " FLO_LRI "phpMyAdmin" FLO_PDI ": {0}",
          "نەتوانرا " FLO_LRI "phpMyAdmin" FLO_PDI " بکرێتەوە: {0}"},
    Entry{Str::PmaAddress, "Opens at {0}", "يُفتح على {0}", "لە {0} دەکرێتەوە"},
    Entry{Str::PmaNameTaken, "The project phpmyadmin has phpmyadmin.localhost, so phpMyAdmin can't use it",
          "المشروع " FLO_LRI "phpmyadmin" FLO_PDI " يستخدم " FLO_LRI "phpmyadmin.localhost" FLO_PDI
          "، لذلك لا يستطيع " FLO_LRI "phpMyAdmin" FLO_PDI " استخدامه",
          "پڕۆژەی " FLO_LRI "phpmyadmin" FLO_PDI " خاوەنی " FLO_LRI "phpmyadmin.localhost" FLO_PDI
          "ـە، بۆیە " FLO_LRI "phpMyAdmin" FLO_PDI " ناتوانێت بەکاری بهێنێت"},
    Entry{Str::PmaNeedsPhp, "Needs PHP: download a version above", "يحتاج إلى " FLO_LRI "PHP" FLO_PDI ": نزّل إصدارًا من الأعلى",
          "پێویستی بە " FLO_LRI "PHP" FLO_PDI " هەیە: وەشانێک لە سەرەوە دابگرە"},
    Entry{Str::SettingsApache, "Apache",
          FLO_LRI "Apache" FLO_PDI,
          FLO_LRI "Apache" FLO_PDI},
    Entry{Str::ApacheHint, "The shared Apache that serves projects using .htaccess, on this computer behind nginx. Turn its modules on or off and add your own httpd.conf lines; the rest is FloServer's.",
          "خادم " FLO_LRI "Apache" FLO_PDI " المشترك الذي يخدم المشاريع التي تستخدم " FLO_LRI ".htaccess" FLO_PDI "، على هذا الحاسوب خلف " FLO_LRI "nginx" FLO_PDI ". شغّل وحداته أو أوقفها وأضف أسطرك الخاصة إلى " FLO_LRI "httpd.conf" FLO_PDI "؛ والباقي يتولاه " FLO_LRI "FloServer" FLO_PDI ".",
          "ئەو " FLO_LRI "Apache" FLO_PDI " ـە هاوبەشەی کە خزمەتی ئەو پڕۆژانە دەکات کە " FLO_LRI ".htaccess" FLO_PDI " بەکاردەهێنن، لەسەر ئەم کۆمپیوتەرە لە پشت " FLO_LRI "nginx" FLO_PDI ". مۆدیولەکانی هەڵبکە یان بیانکوژێنەوە و دێڕەکانی خۆت بۆ " FLO_LRI "httpd.conf" FLO_PDI " زیاد بکە؛ ئەوانی تر هی " FLO_LRI "FloServer" FLO_PDI " ـن."},
    Entry{Str::ApacheVersion, "Version",
          "الإصدار",
          "وەشان"},
    Entry{Str::ApacheNewest, "Newest installed ({0})",
          "أحدث إصدار مثبّت ({0})",
          "نوێترینی دامەزراو ({0})"},
    Entry{Str::ApacheNoneInstalled, "Apache isn't installed. Download it in Runtimes; projects that use .htaccess need it.",
          FLO_LRI "Apache" FLO_PDI " غير مثبّت. نزّله من بيئات التشغيل؛ تحتاجه المشاريع التي تستخدم " FLO_LRI ".htaccess" FLO_PDI ".",
          FLO_LRI "Apache" FLO_PDI " دانەمەزراوە. لە ژینگەکانی کارپێکردن دایبگرە؛ ئەو پڕۆژانەی " FLO_LRI ".htaccess" FLO_PDI " بەکاردەهێنن پێویستیان پێیەتی."},
    Entry{Str::ApacheModules, "Modules",
          "الوحدات",
          "مۆدیولەکان"},
    Entry{Str::ApacheModulesOn, "{0} of {1} on",
          "{0} من {1} مفعّلة",
          "{0} لە {1} هەڵکراوە"},
    Entry{Str::ApacheFindModule, "Find a module",
          "ابحث عن وحدة",
          "گەڕان بۆ مۆدیولێک"},
    Entry{Str::ApacheModNeeded, "FloServer's",
          "يحتاجه " FLO_LRI "FloServer" FLO_PDI,
          FLO_LRI "FloServer" FLO_PDI " پێویستیەتی"},
    Entry{Str::ApacheModHttps, "nginx does HTTPS",
          FLO_LRI "nginx" FLO_PDI " يتولى " FLO_LRI "HTTPS" FLO_PDI,
          FLO_LRI "nginx" FLO_PDI " ئیشی " FLO_LRI "HTTPS" FLO_PDI " دەکات"},
    Entry{Str::ApacheModHttp2, "nginx does HTTP/2",
          FLO_LRI "nginx" FLO_PDI " يتولى " FLO_LRI "HTTP/2" FLO_PDI,
          FLO_LRI "nginx" FLO_PDI " ئیشی " FLO_LRI "HTTP/2" FLO_PDI " دەکات"},
    Entry{Str::ApacheModMpm, "FloServer's choice",
          "يختاره " FLO_LRI "FloServer" FLO_PDI,
          FLO_LRI "FloServer" FLO_PDI " هەڵیدەبژێرێت"},
    Entry{Str::ApacheModUnused, "not used here",
          "غير مستخدم هنا",
          "لێرە بەکارنایەت"},
    Entry{Str::ApacheModFor, "for {0}",
          "لأجل {0}",
          "بۆ {0}"},
    Entry{Str::ApacheModulesNote, "Ticked and grey: FloServer's own config needs them. Grey and empty: they don't apply behind nginx.",
          "المؤشَّرة الرمادية: يحتاجها إعداد " FLO_LRI "FloServer" FLO_PDI " نفسه. الرمادية الفارغة: لا تنطبق خلف " FLO_LRI "nginx" FLO_PDI ".",
          "نیشانکراو و خۆڵەمێشی: ڕێکخستنی خودی " FLO_LRI "FloServer" FLO_PDI " پێویستیانە. خۆڵەمێشی و بەتاڵ: لە پشت " FLO_LRI "nginx" FLO_PDI " بەکارنایەن."},
    Entry{Str::ApacheNoMatch, "No module matches “{0}”",
          "لا توجد وحدة تطابق «{0}»",
          "هیچ مۆدیولێک لەگەڵ «{0}» ناگونجێت"},
    Entry{Str::ApacheConfHint, "Your own lines go at the end of the config FloServer writes.",
          "تُضاف أسطرك في نهاية الإعداد الذي يكتبه " FLO_LRI "FloServer" FLO_PDI ".",
          "دێڕەکانی خۆت دەچنە کۆتایی ئەو ڕێکخستنەی " FLO_LRI "FloServer" FLO_PDI " دەینووسێت."},
    Entry{Str::ApacheShowFinal, "Show the final httpd.conf",
          "عرض " FLO_LRI "httpd.conf" FLO_PDI " النهائي",
          FLO_LRI "httpd.conf" FLO_PDI " ی کۆتایی پیشان بدە"},
    Entry{Str::ApacheEditConf, "Add your own httpd.conf lines…",
          "أضف أسطرك الخاصة إلى " FLO_LRI "httpd.conf" FLO_PDI "…",
          "دێڕەکانی خۆت بۆ " FLO_LRI "httpd.conf" FLO_PDI " زیاد بکە…"},
    Entry{Str::ApacheConfNote, "Lines that would change where Apache listens, its files or its modules, and <VirtualHost>, are left out; Apache checks the rest before using it.",
          "تُستبعد الأسطر التي قد تغيّر مكان استماع " FLO_LRI "Apache" FLO_PDI " أو ملفاته أو وحداته، وكذلك " FLO_LRI "<VirtualHost>" FLO_PDI "؛ ويفحص " FLO_LRI "Apache" FLO_PDI " الباقي قبل استخدامه.",
          "ئەو دێڕانەی شوێنی گوێگرتنی " FLO_LRI "Apache" FLO_PDI " یان پەڕگەکانی یان مۆدیولەکانی دەگۆڕن، هەروەها " FLO_LRI "<VirtualHost>" FLO_PDI "، لادەبرێن؛ " FLO_LRI "Apache" FLO_PDI " ئەوانی تر پێش بەکارهێنان دەپشکنێت."},
    Entry{Str::ApacheCustomProblems, "Some lines of custom.conf are left out: {0}",
          "تم تجاهل بعض أسطر " FLO_LRI "custom.conf" FLO_PDI ": {0}",
          "هەندێک دێڕی " FLO_LRI "custom.conf" FLO_PDI " پشتگوێ خران: {0}"},
    Entry{Str::ApacheRefused, "Apache refused these settings, so it keeps running with the last ones it accepted: {0}",
          "رفض " FLO_LRI "Apache" FLO_PDI " هذه الإعدادات، لذا يستمر بالعمل بآخر إعدادات قبلها: {0}",
          FLO_LRI "Apache" FLO_PDI " ئەم ڕێکخستنانەی ڕەتکردەوە، بۆیە بە دوایین ڕێکخستن کە پەسەندی کردبوو بەردەوام دەبێت: {0}"},
    Entry{Str::ApacheProblems, "Some Apache settings were skipped: {0}",
          "تم تخطي بعض إعدادات " FLO_LRI "Apache" FLO_PDI ": {0}",
          "هەندێک ڕێکخستنی " FLO_LRI "Apache" FLO_PDI " پشتگوێ خران: {0}"},
    Entry{Str::ApacheApplies, "Applies when saved: Apache restarts (about a second) for {0}.",
          "يُطبَّق عند الحفظ: يُعاد تشغيل " FLO_LRI "Apache" FLO_PDI " (نحو ثانية) لـ {0}.",
          "دوای پاشەکەوتکردن جێبەجێ دەبێت: " FLO_LRI "Apache" FLO_PDI " بۆ {0} دەست پێ دەکاتەوە (نزیکەی چرکەیەک)."},
    Entry{Str::ApacheAppliesSleeping, "Applies when saved. Apache isn't running now: it uses them the next time a project wakes it.",
          "يُطبَّق عند الحفظ. " FLO_LRI "Apache" FLO_PDI " لا يعمل الآن: سيستخدمها عندما يوقظه مشروع في المرة القادمة.",
          "دوای پاشەکەوتکردن جێبەجێ دەبێت. " FLO_LRI "Apache" FLO_PDI " ئێستا کار ناکات: جاری داهاتوو کە پڕۆژەیەک بەئاگای دەهێنێتەوە بەکاریان دەهێنێت."},
    Entry{Str::ApacheAppliesNobody, "Applies when saved. No project uses Apache right now.",
          "يُطبَّق عند الحفظ. لا يستخدم أي مشروع " FLO_LRI "Apache" FLO_PDI " الآن.",
          "دوای پاشەکەوتکردن جێبەجێ دەبێت. ئێستا هیچ پڕۆژەیەک " FLO_LRI "Apache" FLO_PDI " بەکار ناهێنێت."},
    Entry{Str::ApacheAskSave, "Save your changes to Apache?",
          "هل تريد حفظ تغييراتك على " FLO_LRI "Apache" FLO_PDI "؟",
          "گۆڕانکارییەکانت لە " FLO_LRI "Apache" FLO_PDI " پاشەکەوت بکرێن؟"},
    Entry{Str::ApacheReadFailed, "Couldn't read Apache's settings: {0}",
          "تعذّرت قراءة إعدادات " FLO_LRI "Apache" FLO_PDI ": {0}",
          "نەتوانرا ڕێکخستنەکانی " FLO_LRI "Apache" FLO_PDI " بخوێندرێنەوە: {0}"},
    Entry{Str::ApacheRestarting, "Saved. Apache is restarting with them.",
          "تم الحفظ. يُعاد تشغيل " FLO_LRI "Apache" FLO_PDI " بها.",
          "پاشەکەوت کرا. " FLO_LRI "Apache" FLO_PDI " پێیانەوە دەست پێ دەکاتەوە."},
    Entry{Str::ApacheOpenFailed, "Couldn't open the file: {0}",
          "تعذّر فتح الملف: {0}",
          "نەتوانرا پەڕگەکە بکرێتەوە: {0}"},
    Entry{Str::ApacheConfOpened, "Opened in Notepad. Save the file: Apache checks your lines and uses them within a few seconds.",
          "فُتح في المفكرة. احفظ الملف: يفحص " FLO_LRI "Apache" FLO_PDI " أسطرك ويستخدمها خلال ثوانٍ.",
          "لە " FLO_LRI "Notepad" FLO_PDI " کرایەوە. پەڕگەکە پاشەکەوت بکە: " FLO_LRI "Apache" FLO_PDI " دێڕەکانت دەپشکنێت و لە ماوەی چەند چرکەیەکدا بەکاریان دەهێنێت."},
    Entry{Str::ApacheFinalOpened, "Opened in Notepad to read: the httpd.conf Apache runs.",
          "فُتح في المفكرة للقراءة: ملف " FLO_LRI "httpd.conf" FLO_PDI " الذي يشغّله " FLO_LRI "Apache" FLO_PDI ".",
          "لە " FLO_LRI "Notepad" FLO_PDI " بۆ خوێندنەوە کرایەوە: ئەو " FLO_LRI "httpd.conf" FLO_PDI " ـەی " FLO_LRI "Apache" FLO_PDI " بەکاری دەهێنێت."},
    Entry{Str::SettingsFolders, "Folders", "المجلدات", "بوخچەکان"},
    Entry{Str::FoldersHint, "Where FloServer makes projects, serves shared sites from and keeps its own files.",
          "أين ينشئ " FLO_LRI "FloServer" FLO_PDI " المشاريع، ومن أين يخدم المواقع المشتركة، وأين يحفظ ملفاته.",
          "شوێنی دروستکردنی پڕۆژەکان، ماڵپەڕە هاوبەشەکان و پەڕگەکانی خودی " FLO_LRI "FloServer" FLO_PDI "."},
    Entry{Str::FoldersProjects, "Projects folder", "مجلد المشاريع", "بوخچەی پڕۆژەکان"},
    Entry{Str::FoldersProjectsNote, "New projects are made here. Projects you already have stay where they are.",
          "تُنشأ المشاريع الجديدة هنا. تبقى مشاريعك الحالية في أماكنها.",
          "پڕۆژە نوێیەکان لێرە دروست دەکرێن. پڕۆژەکانی ئێستات لە شوێنی خۆیان دەمێننەوە."},
    Entry{Str::FoldersUseDefault, "Use the default", "استخدام الافتراضي", "بنەڕەت بەکاربهێنە"},
    Entry{Str::FoldersPickProjects, "Choose where new projects are made", "اختر أين تُنشأ المشاريع الجديدة",
          "شوێنی دروستکردنی پڕۆژە نوێیەکان هەڵبژێرە"},
    Entry{Str::FoldersWww, "www folder for shared sites", "مجلد " FLO_LRI "www" FLO_PDI " للمواقع المشتركة",
          "بوخچەی " FLO_LRI "www" FLO_PDI " بۆ ماڵپەڕە هاوبەشەکان"},
    Entry{Str::FoldersWwwNote, "Every folder in it opens at {0}. It's the same setting as in Shared sites.",
          "كل مجلد فيه يُفتح على {0}. إنه الإعداد نفسه في المواقع المشتركة.",
          "هەر بوخچەیەک تێیدا لە {0} دەکرێتەوە. هەمان ڕێکخستنی ماڵپەڕە هاوبەشەکانە."},
    Entry{Str::FoldersData, "FloServer's data folder", "مجلد بيانات " FLO_LRI "FloServer" FLO_PDI,
          "بوخچەی داتای " FLO_LRI "FloServer" FLO_PDI},
    Entry{Str::FoldersDataNote, "Runtimes, logs, databases and settings. It can't be moved.",
          "بيئات التشغيل والسجلات وقواعد البيانات والإعدادات. لا يمكن نقله.",
          "ڕەنتایمەکان، لۆگەکان، داتابەیسەکان و ڕێکخستنەکان. ناگوازرێتەوە."},
    Entry{Str::FoldersUses, "Uses {0}", "يشغل {0}", "{0} بەکاردەهێنێت"},
    Entry{Str::FoldersUsesAtLeast, "Uses more than {0} (counting took too long to finish)",
          "يشغل أكثر من {0} (استغرق العدّ وقتًا أطول من اللازم)",
          "زیاتر لە {0} بەکاردەهێنێت (ژماردن زۆری خایاند)"},
    Entry{Str::FoldersCounting, "Working out how much space it uses…", "جارٍ حساب المساحة المستخدمة…",
          "حیسابکردنی ئەو شوێنەی بەکاری دەهێنێت…"},
    Entry{Str::FoldersCleanUp, "Clean up", "تنظيف", "پاککردنەوە"},
    Entry{Str::FoldersClearLogs, "Clear old logs", "مسح السجلات القديمة", "سڕینەوەی لۆگە کۆنەکان"},
    Entry{Str::FoldersClearLogsNote,
          "Empties the logs of the web servers, PHP, Node.js and Python, and each project's request log. Running servers keep writing to them.",
          "يُفرغ سجلات خوادم الويب و" FLO_LRI "PHP" FLO_PDI " و" FLO_LRI "Node.js" FLO_PDI " و" FLO_LRI "Python" FLO_PDI " وسجل طلبات كل مشروع. تواصل الخوادم العاملة الكتابة فيها.",
          "لۆگەکانی ڕاژەکارەکانی وێب، " FLO_LRI "PHP" FLO_PDI "، " FLO_LRI "Node.js" FLO_PDI " و " FLO_LRI "Python" FLO_PDI " و لۆگی داواکارییەکانی هەر پڕۆژەیەک بەتاڵ دەکاتەوە. ڕاژەکارە کارکەرەکان بەردەوام تێیان دەنووسن."},
    Entry{Str::FoldersLeftovers, "Remove leftover downloads", "إزالة التنزيلات المتبقية", "لابردنی داگرتنە ماوەکان"},
    Entry{Str::FoldersLeftoversNote,
          "Deletes downloads that were cut off, and installs and new projects that didn't finish.",
          "يحذف التنزيلات التي انقطعت، وعمليات التثبيت والمشاريع الجديدة التي لم تكتمل.",
          "ئەو داگرتنانەی پچڕان، و دامەزراندن و پڕۆژە نوێیانەی تەواو نەبوون دەسڕێتەوە."},
    Entry{Str::FoldersNeverNote,
          "Never removed: installed runtimes, your projects, databases, settings, certificates and passwords.",
          "لا يُحذف أبدًا: بيئات التشغيل المثبتة ومشاريعك وقواعد البيانات والإعدادات والشهادات وكلمات المرور.",
          "هەرگیز لاناچن: ڕەنتایمە دامەزراوەکان، پڕۆژەکانت، داتابەیسەکان، ڕێکخستنەکان، بڕوانامەکان و وشە نهێنییەکان."},
    Entry{Str::FoldersFrees, "Frees {0}", "يحرّر {0}", "{0} ئازاد دەکات"},
    Entry{Str::FoldersNothing, "Nothing to free", "لا شيء لتحريره", "هیچ شتێک نییە بۆ ئازادکردن"},
    Entry{Str::FoldersWaitInstall, "A download or new project is running: try again when it's done.",
          "يجري تنزيل أو إنشاء مشروع جديد: حاول مجددًا عند انتهائه.",
          "داگرتنێک یان پڕۆژەیەکی نوێ لە کاردایە: کە تەواو بوو دووبارە هەوڵ بدەوە."},
    Entry{Str::FoldersFreed, "Freed {0}.", "تم تحرير {0}.", "{0} ئازاد کرا."},
    Entry{Str::FoldersFreedSkipped, "Freed {0}. {1} in use were left as they are.",
          "تم تحرير {0}. تُركت {1} قيد الاستخدام كما هي.",
          "{0} ئازاد کرا. {1} کە لە بەکارهێناندان وەک خۆیان مانەوە."},
    Entry{Str::FoldersCleaning, "Cleaning up…", "جارٍ التنظيف…", "پاککردنەوە…"},
    Entry{Str::FoldersReadFailed, "Couldn't read the folders: {0}", "تعذّرت قراءة المجلدات: {0}",
          "نەتوانرا بوخچەکان بخوێندرێنەوە: {0}"},
    Entry{Str::FoldersChangeFailed, "Couldn't change the projects folder: {0}", "تعذّر تغيير مجلد المشاريع: {0}",
          "نەتوانرا بوخچەی پڕۆژەکان بگۆڕدرێت: {0}"},
    Entry{Str::FoldersProjectsChanged, "New projects will be made in {0}.", "ستُنشأ المشاريع الجديدة في {0}.",
          "پڕۆژە نوێیەکان لە {0} دروست دەکرێن."},
    Entry{Str::FoldersCleanFailed, "Couldn't clean up: {0}", "تعذّر التنظيف: {0}", "نەتوانرا پاک بکرێتەوە: {0}"},
    // Settings > General. Arabic and Kurdish need a native read.
    Entry{Str::SettingsGeneral, "General", "عام", "گشتی"},
    Entry{Str::GeneralHint, "Language, theme, starting with Windows, and what closing the window does.",
          "اللغة والمظهر والتشغيل مع " FLO_LRI "Windows" FLO_PDI " وما يحدث عند إغلاق النافذة.",
          "زمان، ڕووکار، دەستپێکردن لەگەڵ " FLO_LRI "Windows" FLO_PDI "، و ئەوەی داخستنی پەنجەرەکە دەیکات."},
    Entry{Str::GeneralLanguage, "Language", "اللغة", "زمان"},
    Entry{Str::GeneralSameAsWindows, "Same as Windows", "مثل " FLO_LRI "Windows" FLO_PDI, "وەک " FLO_LRI "Windows" FLO_PDI},
    Entry{Str::GeneralLanguageNote, "Changes at once in every FloServer window.",
          "يتغيّر فورًا في كل نوافذ " FLO_LRI "FloServer" FLO_PDI ".",
          "یەکسەر لە هەموو پەنجەرەکانی " FLO_LRI "FloServer" FLO_PDI " دەگۆڕێت."},
    Entry{Str::GeneralTheme, "Theme", "المظهر", "ڕووکار"},
    Entry{Str::GeneralLight, "Light", "فاتح", "ڕووناک"},
    Entry{Str::GeneralDark, "Dark", "داكن", "تاریک"},
    Entry{Str::GeneralStartup, "Starting with Windows", "التشغيل مع " FLO_LRI "Windows" FLO_PDI, "دەستپێکردن لەگەڵ " FLO_LRI "Windows" FLO_PDI},
    Entry{Str::GeneralStartWithWindows, "Start FloServer when I sign in to Windows",
          "تشغيل " FLO_LRI "FloServer" FLO_PDI " عند تسجيل الدخول إلى " FLO_LRI "Windows" FLO_PDI,
          FLO_LRI "FloServer" FLO_PDI " دەست پێ بکات کاتێک دەچمە ناو " FLO_LRI "Windows" FLO_PDI},
    Entry{Str::GeneralStartNote, "Its service starts too, so your projects are ready when you are.",
          "تبدأ خدمته أيضًا، فتكون مشاريعك جاهزة عندما تبدأ العمل.",
          "خزمەتگوزارییەکەشی دەست پێ دەکات، بۆیە پڕۆژەکانت ئامادە دەبن."},
    Entry{Str::GeneralStartOpen, "Open the window", "فتح النافذة", "پەنجەرەکە بکرێتەوە"},
    Entry{Str::GeneralStartTray, "Start in the tray", "البدء في منطقة الإعلام", "لە تەنیشت کاتژمێرەکە دەست پێ بکات"},
    Entry{Str::GeneralStartFailed, "Couldn't change starting with Windows: {0}",
          "تعذّر تغيير التشغيل مع " FLO_LRI "Windows" FLO_PDI ": {0}",
          "نەتوانرا دەستپێکردن لەگەڵ " FLO_LRI "Windows" FLO_PDI " بگۆڕدرێت: {0}"},
    Entry{Str::GeneralStartElsewhere, "Windows starts another copy now: {0}. Pick a choice below to start this one.",
          "يشغّل " FLO_LRI "Windows" FLO_PDI " نسخة أخرى الآن: {0}. اختر أحد الخيارات أدناه لتشغيل هذه النسخة.",
          FLO_LRI "Windows" FLO_PDI " ئێستا کۆپییەکی تر دەست پێ دەکات: {0}. یەکێک لە هەڵبژاردنەکانی خوارەوە هەڵبژێرە بۆ ئەم کۆپییە."},
    Entry{Str::GeneralClosing, "Closing the window", "إغلاق النافذة", "داخستنی پەنجەرەکە"},
    Entry{Str::GeneralCloseTray, "Keep FloServer running in the tray", "إبقاء " FLO_LRI "FloServer" FLO_PDI " يعمل في منطقة الإعلام",
          FLO_LRI "FloServer" FLO_PDI " لە تەنیشت کاتژمێرەکە کار بکات"},
    Entry{Str::GeneralCloseTrayNote, "Your projects stay on. Quit from the tray icon's menu.",
          "تبقى مشاريعك تعمل. للإنهاء استخدم قائمة أيقونة منطقة الإعلام.",
          "پڕۆژەکانت کار دەکەن. لە لیستی ئایکۆنەکەوە دایبخە."},
    Entry{Str::GeneralCloseQuit, "Quit FloServer", "إنهاء " FLO_LRI "FloServer" FLO_PDI, "داخستنی " FLO_LRI "FloServer" FLO_PDI},
    Entry{Str::GeneralCloseQuitNote, "Stops FloServer and every project, as Quit does.",
          "يوقف " FLO_LRI "FloServer" FLO_PDI " وكل المشاريع، كما يفعل الإنهاء.",
          FLO_LRI "FloServer" FLO_PDI " و هەموو پڕۆژەکان دەوەستێنێت، وەک داخستن."},
    Entry{Str::GeneralAbout, "About FloServer", "حول " FLO_LRI "FloServer" FLO_PDI, "دەربارەی " FLO_LRI "FloServer" FLO_PDI},
    Entry{Str::GeneralVersion, "Version {0}", "الإصدار {0}", "وەشانی {0}"},
    Entry{Str::GeneralCheckUpdates, "Check for FloServer updates", "البحث عن تحديثات " FLO_LRI "FloServer" FLO_PDI,
          "پشکنینی نوێکارییەکانی " FLO_LRI "FloServer" FLO_PDI},
    Entry{Str::GeneralUpdatesNote, "Opens {0} in your browser.", "يفتح {0} في المتصفح.", "{0} لە وێبگەڕەکەت دەکاتەوە."},
    Entry{Str::GeneralLicences, "Licences", "التراخيص", "مۆڵەتەکان"},
    Entry{Str::GeneralLicencesOpened, "The licences are open in Notepad.", "فُتحت التراخيص في المفكرة.",
          "مۆڵەتەکان لە " FLO_LRI "Notepad" FLO_PDI " کرانەوە."},
    Entry{Str::GeneralLicencesFailed, "Couldn't open the licences: {0}", "تعذّر فتح التراخيص: {0}",
          "نەتوانرا مۆڵەتەکان بکرێنەوە: {0}"},
    Entry{Str::GeneralSaveFailed, "Couldn't save the setting: {0}", "تعذّر حفظ الإعداد: {0}",
          "نەتوانرا ڕێکخستنەکە پاشەکەوت بکرێت: {0}"},
    Entry{Str::LicencesTitle, "FloServer {0}: licences", "FloServer {0}: التراخيص", "FloServer {0}: مۆڵەتەکان"},
    Entry{Str::LicencesOwn, "FloServer is made by HostKurd (https://floserver.org).",
          "FloServer من صنع HostKurd (https://floserver.org).",
          "FloServer لەلایەن HostKurd دروست کراوە (https://floserver.org)."},
    Entry{Str::LicencesLibraries, "Libraries built into FloServer:", "المكتبات المضمّنة في FloServer:",
          "ئەو کتێبخانانەی لە FloServer دان:"},
    Entry{Str::LicencesTestOnly, "tests only, not shipped", "للاختبارات فقط، لا يُشحن", "تەنها بۆ تاقیکردنەوە، نانێردرێت"},
    Entry{Str::LicencesRuntimes, "Runtimes installed on this computer:", "بيئات التشغيل المثبّتة على هذا الكمبيوتر:",
          "ژینگەکانی کارپێکردنی دامەزراو لەم کۆمپیوتەرە:"},
    Entry{Str::LicencesNoRuntimes, "None yet.", "لا شيء بعد.", "هێشتا هیچ."},
    Entry{Str::LicencesNotRead, "Not read yet: open Licences again in a moment.",
          "لم تُقرأ بعد: افتح التراخيص مرة أخرى بعد لحظة.",
          "هێشتا نەخوێندراونەتەوە: دوای کەمێک مۆڵەتەکان دووبارە بکەرەوە."},
}};

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

std::string_view lang_code(Lang lang) {
    switch (lang) {
        case Lang::Ar:
            return "ar";
        case Lang::Ku:
            return "ku";
        case Lang::En:
            break;
    }
    return "en";
}

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

// Byte by byte, not as literals: a lone embedding in a literal looks like a
// Trojan Source trick to the compiler checks.
constexpr char kLri[] = {'\xE2', '\x80', '\xAA'};  // U+202A LRE
constexpr char kRle[] = {'\xE2', '\x80', '\xAB'};  // U+202B RLE
constexpr char kPdi[] = {'\xE2', '\x80', '\xAC'};  // U+202C PDF

// Whether the first letter of `s` (UTF-8) is written right to left
// (Hebrew, Arabic and their presentation forms).
bool first_strong_is_rtl(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        std::size_t len = 1;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0U) == 0xC0U && i + 1 < s.size()) {
            cp = ((b & 0x1FU) << 6U) | (static_cast<unsigned char>(s[i + 1]) & 0x3FU);
            len = 2;
        } else if ((b & 0xF0U) == 0xE0U && i + 2 < s.size()) {
            cp = ((b & 0x0FU) << 12U) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 6U) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3FU);
            len = 3;
        } else {
            return false;  // outside the BMP or broken: taken as left to right
        }
        i += len;
        if ((cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) {
            return true;
        }
        const bool latin = (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
        if (latin || (cp >= 0x00C0 && cp < 0x2000)) {
            return false;
        }
    }
    return false;
}

std::string isolated(const char (&open)[3], std::string_view s) {
    std::string out(std::begin(open), std::end(open));
    out += s;
    out.append(std::begin(kPdi), std::end(kPdi));
    return out;
}

std::string isolate_ltr(std::string_view s) { return isolated(kLri, s); }
std::string isolate_auto(std::string_view s) { return isolated(first_strong_is_rtl(s) ? kRle : kLri, s); }

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
