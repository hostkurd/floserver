#pragma once

// The window's words in English, Arabic and Kurdish (Sorani) (D13).
// Plain C++ with no Windows code, so it's built and tested on every platform.
//
// Text is UTF-8. Patterns hold {0}, {1}... for values; fill() wraps each value
// in a left-to-right embedding (U+202A ... U+202C) when the language is written
// right to left, because the values are code words: names, addresses,
// versions, ports, commands. Numbers use Western digits in every language.

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace floserver::ui {

enum class Lang : std::uint8_t { En, Ar, Ku };

// "en", "ar", "ku" (also "ckb", Sorani's own code; case and a region such as
// "ar-IQ" are ignored). Nothing for anything else.
[[nodiscard]] std::optional<Lang> parse_lang(std::string_view code);

// FLOSERVER_LANG's value (nullptr when unset): English unless it names one of ours.
[[nodiscard]] Lang lang_from_setting(const char* value);

[[nodiscard]] bool is_rtl(Lang lang);

// "en", "ar" or "ku": what the window tells the service it shows.
[[nodiscard]] std::string_view lang_code(Lang lang);

enum class Str : std::uint16_t {  // NOLINT(performance-enum-size): with every section it outgrows 256
    Projects,
    SearchProjects,
    NewProject,
    ComingSoon,
    Settings,
    StateOn,
    StateAsleep,
    StateOff,
    StateNeedsDownload,
    StateProblem,
    PhpCantStart,
    ServerCantStart,
    Needs,             // {0}: what's missing, "php 8.2, mariadb 11.4"
    Connecting,
    StartingService,
    ServiceFailed,     // {0}: the reason, from the service
    ServiceStopped,    // clickable: starts it again
    ConnectionLost,
    NoProjects,        // {0}: the New project button's words
    NoMatches,         // {0}: the search text
    FooterCounts,      // {0} running, {1} asleep
    FooterNginx,       // {0} http port, {1} https port
    FooterNginxOff,
    FooterMemory,      // {0}: "23.4 MB"
    ChangeFailed,      // {0}: project, {1}: the service's reason
    TrayOpen,
    TrayQuit,
    Stopping,
    // The project panel (step 5b).
    StateWaking,
    OpenInBrowser,
    OpenFolder,
    OpenTerminal,
    OpenInStudio,
    ClosePanel,
    Versions,
    FieldPhp,
    FieldDatabase,
    FieldWeb,
    FieldNode,
    FieldPython,
    ChoiceNone,
    ChoiceNewest,
    NotInstalled,      // {0}: "8.1"
    SavedIn,           // {0}: "floserver.json"
    PhpOffNote,        // under the pickers: PHP off, the folder has .php files
    DatabaseTitle,     // {0}: the database name
    DatabaseDetail,    // {0}: "127.0.0.1:3306", {1}: the user
    CopyEnv,
    EnvCopied,
    KeepRunning,
    KeepRunningOff,    // {0}: minutes
    KeepRunningOn,
    Log,
    LogEmpty,
    OpenFailed,        // {0}: what couldn't be opened
    // New project (step 5c starts with adding an existing folder).
    AddExistingFolder,
    AddFailed,         // {0}: the folder, {1}: the service's reason
    // Downloads from the window (step 5c).
    DownloadOne,       // {0}: "PHP 8.2"
    DownloadMany,      // {0}: how many
    Downloading,       // {0}: "PHP 8.5.12", {1}: "42%" or empty
    Installing,        // {0}: "PHP 8.5.12"
    DownloadFailed,    // {0}: "PHP 8.2", {1}: the service's reason
    // The New project wizard (D9).
    WizardStepTemplate,
    WizardStepName,
    WizardStepVersions,
    WizardWhat,
    WizardNameTitle,
    WizardVersionsTitle,
    WizardName,
    WizardFolder,
    WizardBrowse,
    WizardAddress,     // {0}: "https://shop.localhost"
    WizardMadeIn,      // {0}: the full folder path
    WizardNameRule,
    WizardDownloads,
    WizardBack,
    WizardNext,
    WizardCreate,
    WizardCancel,
    WizardMaking,      // {0}: the project's name
    WizardDone,        // {0}: the project's name
    WizardFailed,      // {0}: the service's reason
    WizardNotMade,
    WizardOpenProject,
    WizardClose,
    WizardNoTemplates, // {0}: why
    WizardLoading,
    // Shared sites (D19): the row under the list and its settings page.
    SharedSites,
    SharedHint,        // {0}: the www folder, {1}: "localhost:8080/<folder>/"
    SharedCantStart,
    SharedOpenFolder,
    SharedWwwFolder,
    SharedChange,
    SharedOpen,
    WebApacheChoice,
    WebNginxChoice,
    SharedDatabaseDetail,  // {0}: "127.0.0.1:3306", {1}: the user, {2}: the prefix "www_"
    // Settings (D20): its own window; Runtimes is its first section.
    SettingsTitle,
    SettingsRuntimes,
    SettingsRuntimesHint,
    SettingsListFrom,      // {0}: the list's number, {1}: the date it's good until
    SettingsNoList,
    SettingsLoading,
    SettingsCheckUpdates,
    SettingsChecking,
    SettingsCheckFailed,   // {0}: the service's reason
    SettingsReadFailed,    // {0}: the service's reason
    SettingsDefaultFor,
    SettingsDefaultChip,
    SettingsInstalledChip,
    SettingsAlwaysUsed,
    SettingsWebServers,
    SettingsOneProject,
    SettingsProjects,      // {0}: how many
    SettingsUsedBy,        // {0}: the projects, and Shared sites
    SettingsRunningOnPort, // {0}: the port
    SettingsOnPort,        // {0}: the port
    SettingsDownload,
    SettingsRemove,
    SettingsStarting,
    SettingsDownloadingOf, // {0}: "19.0 MB", {1}: "31.0 MB"
    SettingsDownloadingDots,
    SettingsInstallingDots,
    SettingsRemoving,
    SettingsRemoveFailed,  // {0}: "PHP 8.4.16", {1}: the service's reason
    SettingsDefaultFailed, // {0}: the service's reason
    // Settings > PHP (D20) and the project panel's PHP part.
    SettingsPhp,
    PhpHint,
    PhpVersion,
    PhpNoneInstalled,
    PhpGoToRuntimes,
    PhpExtensions,
    PhpExtensionsOn,       // {0} on, {1} in all
    PhpFindExtension,
    PhpSlows,
    PhpMissing,
    PhpOpcacheNote,
    PhpMoreCount,          // {0}: how many more
    PhpShowAll,
    PhpShowFewer,
    PhpFixedNote,
    PhpNoMatch,            // {0}: the search text
    PhpIni,
    PhpIniHint,
    PhpEditFull,
    PhpCustomIniUsed,
    PhpReset,
    PhpDefaultCue,
    PhpApplies,            // {0}: who: "3 projects and shared sites"
    PhpAndShared,          // {0}: "3 projects"
    PhpSharedOnly,
    PhpAppliesNobody,      // {0}: "8.5"
    PhpDiscard,
    PhpSave,
    PhpSaving,
    PhpSaved,
    PhpDontSave,
    PhpAskSave,            // {0}: "8.5"
    PhpReadFailed,         // {0}: "8.5", {1}: the service's reason
    PhpSaveFailed,         // {0}: the service's reason
    PhpOpenIniFailed,      // {0}: the reason
    PhpIniOpened,
    PhpShowFinal,
    PhpFinalOpened,
    PhpProblems,           // {0}: what the service skipped
    PanelPhpSettings,
    ProjectPhpOpenSettings,  // {0}: "8.5"
    ProjectPhpExtHint,       // {0}: "8.5"
    ProjectPhpNoChanges,
    ProjectPhpChange,
    ProjectPhpIniHint,       // {0}: "8.5"
    ProjectPhpDefaultChoice, // {0}: the series' value
    ProjectPhpAnother,
    ProjectPhpAnotherCue,
    ProjectPhpSavedIn,       // {0}: "floserver.json"
    ProjectPhpReset,
    ProjectPhpAskSave,       // {0}: the project
    ProjectPhpNoSeries,
    ProjectPhpBadLine,
    PhpUnknownIni,           // {0}: "8.5", {1}: the name
    PhpUnknownIniMaybe,      // {0}: "8.5", {1}: the name, {2}: the known name it's closest to
    // The project panel's tabs.
    PanelTabGeneral,
    PanelFolder,
    // Settings > Ports & HTTPS (D20).
    SettingsPorts,
    PortsHint,
    PortsWebTitle,
    PortsAuto,
    PortsAutoNote,
    PortsStandard,
    PortsStandardNote,  // {0}: an address
    PortsAlternate,
    PortsAlternateNote,  // {0}: an address
    PortsCustom,
    PortsCustomNote,
    PortsHttpLabel,
    PortsHttpsLabel,
    PortsUse,
    PortsBadNumber,
    PortsSameNumber,
    PortsInUse,  // {0}: http port, {1}: https port
    PortsNotRunning,
    PortsTakenBy,  // {0}: port, {1}: program
    PortsTakenByPid,  // {0}: port, {1}: process id
    PortsTaken,  // {0}: port
    PortsMovesLater,
    PortsHttpsTitle,
    PortsHttpsProjects,
    PortsHttpsProjectsNote,  // {0}: "https://<name>.localhost"
    PortsHttpsShared,
    PortsHttpsSharedNote,  // {0}: "https://localhost/<folder>/", {1}: "<folder>.localhost"
    PortsNeedsTrust,
    PortsCaTitle,
    PortsCaTrusted,
    PortsCaNotTrusted,
    PortsCaNone,
    PortsCaDamaged,  // {0}: the reason (English)
    PortsCaSafe,
    PortsTrust,
    PortsOpenCert,
    PortsTrustDone,
    PortsTrustNo,
    PortsTrustFailed,  // {0}: the reason
    PortsRemoveDone,
    PortsRemoveNo,
    PortsRemoveFailed,  // {0}: the reason
    PortsChangeFailed,  // {0}: the service's reason
    PortsReadFailed,  // {0}: the service's reason
    PortsOpenFailed,  // {0}: the reason
    PortsChanging,
    PortsCertUnreadable,  // a reason, after "Not added: "
    // Settings > Database (MariaDB).
    SettingsDatabase,
    DbTitle,
    DbHint,
    DbNoneInstalled,
    DbServer,
    DbStart,
    DbStop,
    DbRestart,
    DbSleeping,              // {0}: the port
    DbStopping,
    DbFailed,                // {0}: the service's reason
    DbNotInUse,
    DbPortTaken,             // {0}: the port chosen, {1}: the one it's on
    DbPort,
    DbPortCue,
    DbPortBad,
    DbPortNote,
    DbKeepRunning,
    DbKeepRunningNote,
    DbKeptBy,                // {0}: the projects
    DbUsedBy,                // {0}: the projects, and shared sites
    DbUsedByNobody,
    DbShowFinal,
    DbEditFull,
    DbUnknownOption,         // {0}: "11.4", {1}: the name
    DbUnknownOptionMaybe,    // {0}: "11.4", {1}: the name, {2}: the known name it's closest to
    DbCustomProblems,        // {0}: the lines left out
    DbCustomIniUsed,
    DbProblems,              // {0}: what the service skipped
    DbRoot,
    DbRootPassword,
    DbShow,
    DbHide,
    DbCopy,
    DbNewPassword,
    DbNewPasswordCue,
    DbSetPassword,
    DbRootNote,
    DbDatabases,             // {0}: the projects
    DbNoDatabases,
    DbPasswordSet,
    DbPasswordCopied,
    DbPasswordFailed,        // {0}: the service's reason
    DbRootReadFailed,        // {0}: the service's reason
    DbPasswordShort,
    DbPasswordLong,
    DbPasswordChars,
    DbApplies,               // {0}: "11.4", {1}: who: "3 projects and shared sites"
    DbAppliesSleeping,       // {0}: "11.4"
    DbAppliesNobody,         // {0}: "11.4"
    DbAskSave,               // {0}: "11.4"
    DbReadFailed,            // {0}: "11.4", {1}: the service's reason
    DbRestarting,            // {0}: "11.4"
    DbServerFailed,          // {0}: "11.4", {1}: the service's reason
    DbOpenIniFailed,         // {0}: the reason
    DbIniOpened,
    DbFinalOpened,
    // phpMyAdmin (an optional tool).
    SettingsTools,
    PmaOpen,
    PmaOpenProject,
    PmaNotInstalled,
    PmaOpening,
    PmaStarting,             // {0}: the MariaDB series, "11.8"
    PmaOpenFailed,          // {0}: the service's reason
    PmaAddress,              // {0}: "https://phpmyadmin.localhost"
    PmaNameTaken,
    PmaNeedsPhp,
    // Settings > Apache.
    SettingsApache,
    ApacheHint,
    ApacheVersion,
    ApacheNewest,            // {0}: the version
    ApacheNoneInstalled,
    ApacheModules,
    ApacheModulesOn,         // {0}: on, {1}: all
    ApacheFindModule,
    ApacheModNeeded,
    ApacheModHttps,
    ApacheModHttp2,
    ApacheModMpm,
    ApacheModUnused,
    ApacheModFor,            // {0}: the modules that need it
    ApacheModulesNote,
    ApacheNoMatch,           // {0}: what was typed
    ApacheConfHint,
    ApacheShowFinal,
    ApacheEditConf,
    ApacheConfNote,
    ApacheCustomProblems,    // {0}: the lines left out
    ApacheRefused,           // {0}: what Apache said
    ApacheProblems,          // {0}: what the service skipped
    ApacheApplies,           // {0}: who: "blog, shop and shared sites"
    ApacheAppliesSleeping,
    ApacheAppliesNobody,
    ApacheAskSave,
    ApacheReadFailed,        // {0}: the service's reason
    ApacheRestarting,
    ApacheOpenFailed,        // {0}: the reason
    ApacheConfOpened,
    ApacheFinalOpened,
    // Settings > Folders.
    SettingsFolders,
    FoldersHint,
    FoldersProjects,
    FoldersProjectsNote,
    FoldersUseDefault,
    FoldersPickProjects,
    FoldersWww,
    FoldersWwwNote,          // {0}: "localhost/<folder>/"
    FoldersData,
    FoldersDataNote,
    FoldersUses,             // {0}: "1.4 GB"
    FoldersUsesAtLeast,      // {0}: "1.4 GB"
    FoldersCounting,
    FoldersCleanUp,
    FoldersClearLogs,
    FoldersClearLogsNote,
    FoldersLeftovers,
    FoldersLeftoversNote,
    FoldersNeverNote,
    FoldersFrees,            // {0}: "12 MB"
    FoldersNothing,
    FoldersWaitInstall,
    FoldersFreed,            // {0}: "12 MB"
    FoldersFreedSkipped,     // {0}: "12 MB", {1}: how many files were in use
    FoldersCleaning,
    FoldersReadFailed,       // {0}: the service's reason
    FoldersChangeFailed,     // {0}: the service's reason
    FoldersProjectsChanged,  // {0}: the folder
    FoldersCleanFailed,      // {0}: the service's reason
    // Settings > General.
    SettingsGeneral,
    GeneralHint,
    GeneralLanguage,
    GeneralSameAsWindows,
    GeneralLanguageNote,
    GeneralTheme,
    GeneralLight,
    GeneralDark,
    GeneralStartup,
    GeneralStartWithWindows,
    GeneralStartNote,
    GeneralStartOpen,
    GeneralStartTray,
    GeneralStartFailed,      // {0}: the reason
    GeneralStartElsewhere,   // {0}: the program the Run entry starts
    GeneralClosing,
    GeneralCloseTray,
    GeneralCloseTrayNote,
    GeneralCloseQuit,
    GeneralCloseQuitNote,
    GeneralAbout,
    GeneralVersion,          // {0}: "0.9.0"
    GeneralCheckUpdates,
    GeneralUpdatesNote,      // {0}: "floserver.org"
    GeneralLicences,
    GeneralLicencesOpened,
    GeneralLicencesFailed,   // {0}: the reason
    GeneralSaveFailed,       // {0}: the reason
    // The licences file (Notepad): no direction marks.
    LicencesTitle,           // {0}: FloServer's version
    LicencesOwn,
    LicencesLibraries,
    LicencesTestOnly,
    LicencesRuntimes,
    LicencesNoRuntimes,
    LicencesNotRead,
    Count_,
};

[[nodiscard]] std::string_view text(Lang lang, Str key);

// U+202A LEFT-TO-RIGHT EMBEDDING, the text, U+202C POP DIRECTIONAL
// FORMATTING. Embeddings, not the newer isolates (U+2066...), because
// Windows' DrawText draws isolates as boxes (seen on a real PC, 29 Oct).
[[nodiscard]] std::string isolate_ltr(std::string_view s);
// The same with the direction of the first letter (U+202B RIGHT-TO-LEFT
// EMBEDDING for Arabic): for text the user typed, in either direction.
[[nodiscard]] std::string isolate_auto(std::string_view s);

// Puts values into a pattern: "{0}" is the first value, "{1}" the second.
// Values are isolated as left-to-right in right-to-left languages.
[[nodiscard]] std::string fill(Lang lang, Str key, std::initializer_list<std::string_view> values);
[[nodiscard]] std::string fill_pattern(std::string_view pattern, bool isolate,
                                       std::initializer_list<std::string_view> values);

}  // namespace floserver::ui
