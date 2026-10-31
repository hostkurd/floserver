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
constexpr std::array kTable{
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
