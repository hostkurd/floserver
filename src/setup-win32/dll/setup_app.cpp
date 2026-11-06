// The setup screens (D25, approved with its mockups 6 Nov), in
// floserver-setup.dll: FloServer-Setup-<version>.exe writes this DLL and
// FloServer's MSI into a folder under %TEMP% and calls floserver_setup_run().
// It shows its own screens and runs the MSI with Windows Installer's own
// screens off, its progress on our bar. The MSI is the one D21 ships (per
// user, no admin): what it installs and removes doesn't change.
//
//   new install:     Licence -> Installing -> Done
//   older installed: Update (or Licence first, when it changed) -> Installing -> Done
//   same build:      Install again -> Installing -> Done
//   newer installed: says so, nothing else
//
// English only (D21). Quiet installs with no window (for CI and companies),
// accepting the licence as msiexec /qn does.

#include "common.h"
#include "floserver/setup_api.h"
#include "model/setup_model.h"
#include "setup_look.h"
#include "win_util.h"

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <msi.h>
#include <msiquery.h>
#include <richedit.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The GDI+ headers use min and max unqualified; NOMINMAX takes the macros away.
using std::max;  // NOLINT(misc-unused-using-decls): used by the headers below on some SDKs
using std::min;  // NOLINT(misc-unused-using-decls)
#include <objidl.h>
#include <gdiplus.h>

namespace {

namespace setup = floserver::setup;
namespace look = floserver::setup::look;
using floserver::ui::win::narrow;
using floserver::ui::win::widen;

constexpr wchar_t kUpgradeCode[] = L"{3AC2E747-DDE1-465C-87CC-277C97C81D0F}";
constexpr wchar_t kTitle[] = L"FloServer Setup";
constexpr wchar_t kPanelNote[] = L"PHP, Node.js, Python and MariaDB on your PC, each project at its own https://name.localhost.";

// The window's controls.
enum : int {
    kAccept = 100,
    kPrint,
    kInstall,
    kCancel,
    kBack,
    kReadLicence,
    kFinish,
    kOpenNow,
    kClose,
    kNext,
};

std::string sha256_hex(std::span<const std::uint8_t> data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    std::array<std::uint8_t, 32> digest{};
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    ok = ok && BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) == 0;  // NOLINT(cppcoreguidelines-pro-type-const-cast): only read
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) {
        return {};
    }
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : digest) {
        out += kHex[b >> 4U];
        out += kHex[b & 0xFU];
    }
    return out;
}

std::wstring accepted_licence() {
    std::array<wchar_t, 128> buf{};
    DWORD size = static_cast<DWORD>(sizeof buf);
    if (RegGetValueW(HKEY_CURRENT_USER, setup::kRegistryKey, setup::kAcceptedLicenceValue, RRF_RT_REG_SZ, nullptr, buf.data(), &size) !=
        ERROR_SUCCESS) {
        return {};
    }
    return buf.data();
}

void remember_licence(const std::string& sha256) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, setup::kRegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    const std::wstring value = widen(sha256);
    RegSetValueExW(key, setup::kAcceptedLicenceValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the registry takes bytes
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

struct MsiHandle {
    MSIHANDLE h = 0;
    MsiHandle() = default;
    MsiHandle(const MsiHandle&) = delete;
    MsiHandle& operator=(const MsiHandle&) = delete;
    MsiHandle(MsiHandle&&) = delete;
    MsiHandle& operator=(MsiHandle&&) = delete;
    ~MsiHandle() {
        if (h != 0) {
            MsiCloseHandle(h);
        }
    }
};

// The MSI's product code and version, and how much it installs.
struct Package {
    setup::Product product;
    std::uint64_t installed_bytes = 0;
};

std::optional<Package> read_package(const std::wstring& msi) {
    MsiHandle db;
    if (MsiOpenDatabaseW(msi.c_str(), MSIDBOPEN_READONLY, &db.h) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    auto property = [&db](const wchar_t* name) -> std::string {
        MsiHandle view;
        const std::wstring query = std::wstring(L"SELECT `Value` FROM `Property` WHERE `Property`='") + name + L"'";
        if (MsiDatabaseOpenViewW(db.h, query.c_str(), &view.h) != ERROR_SUCCESS ||
            MsiViewExecute(view.h, 0) != ERROR_SUCCESS) {
            return {};
        }
        MsiHandle record;
        if (MsiViewFetch(view.h, &record.h) != ERROR_SUCCESS) {
            return {};
        }
        std::array<wchar_t, 256> buf{};
        DWORD size = static_cast<DWORD>(buf.size());
        if (MsiRecordGetStringW(record.h, 1, buf.data(), &size) != ERROR_SUCCESS) {
            return {};
        }
        return narrow(buf.data());
    };
    Package out;
    out.product.code = property(L"ProductCode");
    out.product.version = property(L"ProductVersion");
    if (out.product.code.empty() || out.product.version.empty()) {
        return std::nullopt;
    }
    MsiHandle view;
    if (MsiDatabaseOpenViewW(db.h, L"SELECT `FileSize` FROM `File`", &view.h) == ERROR_SUCCESS &&
        MsiViewExecute(view.h, 0) == ERROR_SUCCESS) {
        while (true) {
            MsiHandle record;
            if (MsiViewFetch(view.h, &record.h) != ERROR_SUCCESS) {
                break;
            }
            const int size = MsiRecordGetInteger(record.h, 1);
            if (size > 0) {
                out.installed_bytes += static_cast<std::uint64_t>(size);
            }
        }
    }
    return out;
}

// The FloServer installed now (any version), found by the upgrade code.
std::optional<setup::Product> installed_product() {
    std::array<wchar_t, 39> code{};
    if (MsiEnumRelatedProductsW(kUpgradeCode, 0, 0, code.data()) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::array<wchar_t, 64> version{};
    DWORD size = static_cast<DWORD>(version.size());
    if (MsiGetProductInfoW(code.data(), INSTALLPROPERTY_VERSIONSTRING, version.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return setup::Product{narrow(code.data()), narrow(version.data())};
}

// --- the licence box ---

struct StreamSource {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
};

DWORD CALLBACK stream_in(DWORD_PTR cookie, LPBYTE buffer, LONG wanted, LONG* got) {
    auto* src = floserver::ui::win::int_to_ptr<StreamSource*>(cookie);
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(wanted), src->data.size() - src->pos);
    std::memcpy(buffer, src->data.data() + src->pos, n);
    src->pos += n;
    *got = static_cast<LONG>(n);
    return 0;
}

// Windows' Print dialog, then the licence as the rich edit lays it out, with
// a margin of an inch.
void print_licence(HWND owner, HWND rich) {
    PRINTDLGW pd{};
    pd.lStructSize = sizeof pd;
    pd.hwndOwner = owner;
    pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
    if (PrintDlgW(&pd) == FALSE || pd.hDC == nullptr) {
        return;
    }
    const HDC dc = pd.hDC;
    DOCINFOW doc{};
    doc.cbSize = sizeof doc;
    doc.lpszDocName = L"FloServer End-User Licence Agreement";
    if (StartDocW(dc, &doc) > 0) {
        const int dpi_x = GetDeviceCaps(dc, LOGPIXELSX);
        const int dpi_y = GetDeviceCaps(dc, LOGPIXELSY);
        const int width = MulDiv(GetDeviceCaps(dc, PHYSICALWIDTH), 1440, dpi_x);
        const int height = MulDiv(GetDeviceCaps(dc, PHYSICALHEIGHT), 1440, dpi_y);
        const int off_x = MulDiv(GetDeviceCaps(dc, PHYSICALOFFSETX), 1440, dpi_x);
        const int off_y = MulDiv(GetDeviceCaps(dc, PHYSICALOFFSETY), 1440, dpi_y);
        FORMATRANGE range{};
        range.hdc = dc;
        range.hdcTarget = dc;
        range.rcPage = {0, 0, width, height};
        range.rc = {1440 - off_x, 1440 - off_y, width - 1440 - off_x, height - 1440 - off_y};
        GETTEXTLENGTHEX length{GTL_DEFAULT, 1200};
        const auto total = static_cast<LONG>(
            SendMessageW(rich, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the message's struct
        range.chrg = {0, -1};
        while (range.chrg.cpMin < total) {
            if (StartPage(dc) <= 0) {
                break;
            }
            const auto next = static_cast<LONG>(
                SendMessageW(rich, EM_FORMATRANGE, TRUE, reinterpret_cast<LPARAM>(&range)));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the message's struct
            EndPage(dc);
            if (next <= range.chrg.cpMin) {
                break;
            }
            range.chrg.cpMin = next;
        }
        SendMessageW(rich, EM_FORMATRANGE, FALSE, 0);
        EndDoc(dc);
    }
    DeleteDC(dc);
    if (pd.hDevMode != nullptr) {
        GlobalFree(pd.hDevMode);
    }
    if (pd.hDevNames != nullptr) {
        GlobalFree(pd.hDevNames);
    }
}

// Opens the installed FloServer.exe, not elevated, as the user started it.
void open_floserver() {
    const std::wstring folder = setup::install_folder();
    if (folder.empty()) {
        return;
    }
    const std::wstring exe = folder + L"\\FloServer.exe";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + exe + L"\"";
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &si, &pi) !=
        FALSE) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// --- the window ---

enum class Page : std::uint8_t { Licence, Update, Installing, Done, Problem };

struct Facts {
    setup::SetupMode mode = setup::SetupMode::Install;
    std::optional<setup::Product> installed;
    setup::Product ours;
    std::uint64_t installed_bytes = 0;
    std::span<const std::uint8_t> licence;
    std::string licence_sha256;
    bool licence_changed = false;  // an update whose licence isn't the one accepted last time
    std::wstring msi;
    std::wstring log;
};

class SetupWindow final : public look::WizardWindow {
public:
    SetupWindow(HINSTANCE instance, Facts facts)
        : WizardWindow(instance, FLOSERVER_SETUP_WINDOW_CLASS, kTitle, kPanelNote,
                       L"Version " + widen(facts.ours.version.empty() ? "?" : facts.ours.version) +
                           L" · HostKurd\nfloserver.org"),
          facts_(std::move(facts)) {}

    ~SetupWindow() override {
        shared_->detach();
        if (worker_.joinable()) {
            shared_->cancel = true;
            worker_.join();
        }
    }
    SetupWindow(const SetupWindow&) = delete;
    SetupWindow& operator=(const SetupWindow&) = delete;
    SetupWindow(SetupWindow&&) = delete;
    SetupWindow& operator=(SetupWindow&&) = delete;

    // `code`: what the setup ends with (Windows Installer's codes).
    void show_problem(std::wstring heading, std::wstring text, int code) {
        exit_code_ = code;
        page_ = Page::Problem;
        problem_heading_ = std::move(heading);
        problem_text_ = std::move(text);
        rebuild();
    }

    void start_at(Page page) { page_ = page; }

    [[nodiscard]] int exit_code() const { return exit_code_; }

private:
    [[nodiscard]] bool updating() const { return facts_.mode != setup::SetupMode::Install; }
    [[nodiscard]] std::vector<std::wstring> steps(bool licence) const {
        if (!updating()) {
            return {L"Licence", L"Install", L"Done"};
        }
        return {licence ? L"Licence" : L"Check", L"Update", L"Done"};
    }

    void heading(std::wstring_view title, std::wstring_view sub) {
        add_text({look::kContentX, 82, look::kContentR, 120}, title, look::TextKind::Heading);
        if (!sub.empty()) {
            add_text({look::kContentX, 122, look::kContentR, 166}, sub, look::TextKind::Sub, look::kWhite, look::kMuted);
        }
    }

    void build_page() override {
        switch (page_) {
            case Page::Licence:
                build_licence();
                break;
            case Page::Update:
                build_update();
                break;
            case Page::Installing:
                build_installing();
                break;
            case Page::Done:
                build_done();
                break;
            case Page::Problem:
                build_problem();
                break;
        }
    }

    void build_licence() {
        start_page(steps(true), 0);
        std::wstring sub = L"Please read the terms before you install FloServer.";
        if (updating()) {
            sub = facts_.licence_changed ? L"The licence has changed since you last accepted it. Please read it before you update."
                                         : L"The terms you accepted when you installed FloServer.";
        }
        heading(L"Licence agreement", sub);
        add_card({look::kContentX, 168, look::kContentR, 390}, look::kWhite, RGB(0xD6, 0xD6, 0xD6));
        if (LoadLibraryW(L"Msftedit.dll") != nullptr) {
            const RECT r = px(RECT{look::kContentX + 14, 178, look::kContentR - 6, 382});
            HWND rich = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                                            ES_AUTOVSCROLL,
                                        r.left, r.top, r.right - r.left, r.bottom - r.top, hwnd(), nullptr, instance(),
                                        nullptr);
            adopt(rich);
            rich_ = rich;
            if (rich != nullptr) {
                SetWindowTextW(rich, L"FloServer End-User Licence Agreement");  // its name for screen readers until the text is in
                SendMessageW(rich, EM_SETBKGNDCOLOR, 0, look::kWhite);
                StreamSource src{facts_.licence, 0};
                EDITSTREAM es{};
                es.dwCookie = reinterpret_cast<DWORD_PTR>(&src);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the stream's context
                es.pfnCallback = &stream_in;
                SendMessageW(rich, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the message's struct
                SendMessageW(rich, EM_SETSEL, 0, 0);
            }
        }
        add_check(kAccept, {look::kContentX, 402, look::kContentR, 428}, L"I accept the terms of the licence agreement",
                  accepted_);
        add_mark(look::kContentX, 440, look::MarkKind::Info);
        std::wstring note = L"For you only, no administrator rights";
        if (facts_.installed_bytes > 0) {
            note += L" · " + widen(setup::format_size(facts_.installed_bytes));
        }
        note += L" · " + setup::install_folder() + L". Runtimes are downloaded when FloServer first opens.";
        add_text({look::kContentX + 24, 438, look::kContentR, 500}, note, look::TextKind::Note, look::kWhite, look::kMuted);

        add_button(kPrint, {look::kContentX, look::kButtonY, look::kContentX + 96, look::kButtonY + look::kButtonH},
                   L"Print", look::ButtonKind::Secondary, rich_ != nullptr);
        int x = look::kContentR - 96;
        add_button(kCancel, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Cancel",
                   look::ButtonKind::Secondary);
        x -= 10 + 96;
        add_button(kInstall, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH},
                   updating() ? L"Update" : L"Install", look::ButtonKind::Primary, accepted_);
        if (from_update_) {
            x -= 10 + 96;
            add_button(kBack, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Back",
                       look::ButtonKind::Secondary);
        }
        set_default_button(kInstall);
        set_cancel_button(kCancel);
    }

    void build_update() {
        start_page(steps(false), 0);
        const std::wstring old_version = facts_.installed ? widen(facts_.installed->version) : L"";
        const std::wstring new_version = widen(facts_.ours.version);
        const bool again = facts_.mode == setup::SetupMode::Reinstall;
        if (again) {
            heading(L"Install FloServer again",
                    L"FloServer " + new_version + L" is already installed. This installs the same version again.");
        } else {
            heading(L"Update FloServer",
                    L"FloServer " + old_version + L" is installed. This updates it to " + new_version + L".");
        }
        add_card({look::kContentX, 172, look::kContentR, 316}, look::kWhite, look::kBorder);
        add_text({look::kContentX + 18, 186, look::kContentR - 18, 208}, L"What happens", look::TextKind::BodyBold);
        const std::array<std::wstring_view, 3> lines{
            L"Running projects stop for a moment, then start again.",
            L"Your projects, databases, runtimes and settings stay as they are.",
            L"FloServer opens again when it's done, if you leave that ticked.",
        };
        int y = 216;
        for (const std::wstring_view line : lines) {
            add_mark(look::kContentX + 16, y, look::MarkKind::Done);
            add_text({look::kContentX + 46, y + 1, look::kContentR - 18, y + 22}, line, look::TextKind::Body, look::kWhite,
                     look::kText2);
            y += 30;
        }
        add_mark(look::kContentX, 336, look::MarkKind::Info);
        add_text({look::kContentX + 24, 334, look::kContentR, 354}, L"The licence hasn't changed since you accepted it.",
                 look::TextKind::Note, look::kWhite, look::kMuted);
        add_button(kReadLicence, {look::kContentX + 22, 356, look::kContentX + 160, 380}, L"Read the licence",
                   look::ButtonKind::Link);
        int x = look::kContentR - 96;
        add_button(kCancel, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Cancel",
                   look::ButtonKind::Secondary);
        x -= 10 + 120;
        add_button(kInstall, {x, look::kButtonY, x + 120, look::kButtonY + look::kButtonH},
                   again ? L"Install again" : L"Update", look::ButtonKind::Primary);
        set_default_button(kInstall);
        set_cancel_button(kCancel);
    }

    void build_installing() {
        start_page(steps(page_from_licence_), 1);
        heading(updating() ? L"Updating FloServer" : L"Installing FloServer", L"This takes a few seconds.");
        status_ = add_text({look::kContentX, 176, look::kContentR - 60, 196}, status_text(), look::TextKind::Note,
                           look::kWhite, look::kText);
        percent_ = add_text({look::kContentR - 60, 176, look::kContentR, 196}, percent_text(), look::TextKind::Note,
                            look::kWhite, look::kMuted);
        SetWindowLongPtrW(percent_, GWL_STYLE, GetWindowLongPtrW(percent_, GWL_STYLE) | SS_RIGHT);
        add_progress({look::kContentX, 202, look::kContentR, 208});
        set_progress(static_cast<double>(permille_) / 1000);
        const std::array<std::wstring_view, setup::kInstallSteps> names{
            L"Checked the installer", L"Copying the programs", L"Adding flo to your Path",
            L"Adding FloServer to the Start menu"};
        int y = 234;
        for (std::size_t i = 0; i < names.size(); ++i) {
            marks_.at(i) = add_mark(look::kContentX, y, mark_for(static_cast<int>(i)));
            step_texts_.at(i) = add_text({look::kContentX + 32, y, look::kContentR, y + 22}, names.at(i),
                                         static_cast<int>(i) == step_ ? look::TextKind::BodyBold : look::TextKind::Body,
                                         look::kWhite, static_cast<int>(i) > step_ ? RGB(0x61, 0x61, 0x61) : look::kText);
            y += 36;
        }
        int x = look::kContentR - 96;
        add_button(kCancel, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH},
                   shared_->cancel ? L"Cancelling…" : L"Cancel", look::ButtonKind::Secondary, !shared_->cancel);
        x -= 10 + 96;
        add_button(kNext, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Next", look::ButtonKind::Primary,
                   false);
        set_cancel_button(kCancel);
    }

    void build_done() {
        start_page(steps(page_from_licence_), 3);
        add_mark(look::kContentX, 112, look::MarkKind::BigCheck);
        add_text({look::kContentX, 186, look::kContentR, 224}, updating() ? L"FloServer is up to date" : L"FloServer is ready",
                 look::TextKind::Heading);
        std::wstring sub = updating()
                               ? L"FloServer " + widen(facts_.ours.version) + L" is installed. Your projects, databases and settings are as they were."
                               : L"When it opens, choose your language and the runtimes to download: PHP, MariaDB, Node.js and more.";
        if (restart_) {
            sub += L" Restart Windows to finish.";
        }
        add_text({look::kContentX, 228, look::kContentR - 40, 272}, sub, look::TextKind::Sub, look::kWhite, look::kMuted);
        add_card({look::kContentX, 290, look::kContentR - 40, 342}, look::kTintCard, look::kAccentSoft);
        add_mark(look::kContentX + 14, 306, look::MarkKind::Info);
        add_text({look::kContentX + 40, 304, look::kContentR - 54, 330},
                 L"In a new terminal, type flo to manage your projects from the command line.", look::TextKind::Body,
                 look::kTintCard, look::kText);
        add_check(kOpenNow, {look::kContentX, 362, look::kContentR, 388}, L"Open FloServer now", open_now_);
        const int x = look::kContentR - 96;
        add_button(kFinish, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Finish",
                   look::ButtonKind::Primary);
        set_default_button(kFinish);
    }

    void build_problem() {
        start_page({}, 0);
        add_mark(look::kContentX, 112, look::MarkKind::Warning);
        add_text({look::kContentX, 186, look::kContentR, 224}, problem_heading_, look::TextKind::Heading);
        add_text({look::kContentX, 230, look::kContentR, 420}, problem_text_, look::TextKind::Sub, look::kWhite,
                 look::kText2);
        const int x = look::kContentR - 96;
        add_button(kClose, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Close",
                   look::ButtonKind::Primary);
        set_default_button(kClose);
        set_cancel_button(kClose);
    }

    [[nodiscard]] look::MarkKind mark_for(int index) const {
        if (index < step_) {
            return look::MarkKind::Done;
        }
        return index == step_ ? look::MarkKind::Now : look::MarkKind::Next;
    }
    [[nodiscard]] std::wstring status_text() const {
        if (shared_->cancel) {
            return L"Cancelling: putting everything back as it was…";
        }
        switch (step_) {
            case 0:
                return L"Checking the installer…";
            case 1:
                return L"Copying the programs…";
            case 2:
                return L"Adding flo to your Path…";
            default:
                return L"Adding FloServer to the Start menu…";
        }
    }
    [[nodiscard]] std::wstring percent_text() const { return std::to_wstring(permille_ / 10) + L"%"; }

    void on_command(int id) override {
        switch (id) {
            case kAccept:
                accepted_ = checked(kAccept);
                enable(kInstall, accepted_);
                break;
            case kPrint:
                if (rich_ != nullptr) {
                    print_licence(hwnd(), rich_);
                }
                break;
            case kReadLicence:
                from_update_ = true;
                accepted_ = false;
                page_ = Page::Licence;
                rebuild();
                break;
            case kBack:
                from_update_ = false;
                page_ = Page::Update;
                rebuild();
                break;
            case kInstall:
                if (page_ == Page::Licence && !checked(kAccept)) {
                    return;
                }
                page_from_licence_ = page_ == Page::Licence;
                start_install();
                break;
            case kCancel:
                if (page_ == Page::Installing) {
                    cancel_install();
                } else {
                    exit_code_ = static_cast<int>(ERROR_INSTALL_USEREXIT);
                    DestroyWindow(hwnd());
                }
                break;
            case kOpenNow:
                open_now_ = checked(kOpenNow);
                break;
            case kFinish:
                if (checked(kOpenNow)) {
                    open_floserver();
                }
                DestroyWindow(hwnd());
                break;
            case kClose:
                DestroyWindow(hwnd());
                break;
            default:
                break;
        }
    }

    bool on_close_request() override {
        if (page_ == Page::Installing) {
            cancel_install();  // closes once Windows Installer has put things back
            return false;
        }
        if (page_ == Page::Licence || page_ == Page::Update) {
            exit_code_ = static_cast<int>(ERROR_INSTALL_USEREXIT);  // closed before installing
        }
        return true;
    }

    void cancel_install() {
        if (shared_->cancel) {
            return;
        }
        shared_->cancel = true;
        rebuild();
    }

    void start_install() {
        page_ = Page::Installing;
        step_ = 0;
        permille_ = 0;
        {
            const std::lock_guard lock(shared_->mutex);
            shared_->window = hwnd();
        }
        rebuild();
        worker_ = std::thread([shared = shared_, msi = facts_.msi, mode = facts_.mode, log = facts_.log] {
            const UINT result = setup::msi_install(msi, setup::msi_properties(mode), log, *shared);
            shared->post(setup::kDoneMessage, result);
        });
    }

    void on_app_message(UINT message, WPARAM wparam, LPARAM /*lparam*/) override {
        if (page_ != Page::Installing) {
            return;
        }
        if (message == setup::kProgressMessage) {
            permille_ = static_cast<int>(wparam);
            set_progress(static_cast<double>(permille_) / 1000);
            set_text(percent_, percent_text());
        } else if (message == setup::kStepMessage) {
            step_ = static_cast<int>(wparam);
            for (int i = 0; i < setup::kInstallSteps; ++i) {
                set_mark(marks_.at(static_cast<std::size_t>(i)), mark_for(i));
            }
            rebuild();  // the names' weights and colours
        } else if (message == setup::kDoneMessage) {
            if (worker_.joinable()) {
                worker_.join();
            }
            finish(static_cast<UINT>(wparam));
        }
    }

    void finish(UINT result) {
        exit_code_ = static_cast<int>(result);
        if (result == ERROR_SUCCESS || result == ERROR_SUCCESS_REBOOT_REQUIRED) {
            restart_ = result == ERROR_SUCCESS_REBOOT_REQUIRED;
            remember_licence(facts_.licence_sha256);
            step_ = setup::kInstallSteps;
            page_ = Page::Done;
            rebuild();
            return;
        }
        if (result == ERROR_INSTALL_USEREXIT) {
            show_problem(L"Setup was cancelled",
                         updating() ? L"Nothing was changed: the FloServer you had is as it was." : L"Nothing was installed.",
                         static_cast<int>(result));
            return;
        }
        std::wstring error;
        {
            const std::lock_guard lock(shared_->mutex);
            error = shared_->error;
        }
        if (error.empty()) {
            error = setup::error_words(result);
        }
        show_problem(updating() ? L"FloServer wasn't updated" : L"FloServer wasn't installed",
                     error + L"\n\nWindows put back everything it had changed. If it happens again, run "
                             L"the setup with --log setup.log and send us the file.",
                     static_cast<int>(result));
    }

    void on_destroyed() override { PostQuitMessage(exit_code_); }

    Facts facts_;
    Page page_ = Page::Licence;
    bool accepted_ = false;
    bool from_update_ = false;
    bool page_from_licence_ = true;
    bool open_now_ = true;
    bool restart_ = false;
    int exit_code_ = 0;
    int step_ = 0;
    int permille_ = 0;
    HWND rich_ = nullptr;
    HWND status_ = nullptr;
    HWND percent_ = nullptr;
    std::array<int, setup::kInstallSteps> marks_{};
    std::array<HWND, setup::kInstallSteps> step_texts_{};
    std::wstring problem_heading_;
    std::wstring problem_text_;
    std::shared_ptr<setup::MsiRun> shared_ = std::make_shared<setup::MsiRun>();
    std::thread worker_;
};


// The setup's licence: the EULA (installer/License.rtf), in the DLL.
constexpr wchar_t kLicenceResource[] = L"LICENCE_RTF";

}  // namespace

FLOSERVER_SETUP_API int __stdcall floserver_setup_run(const FloSetupRunArgs* args) {
    if (args == nullptr || args->size < sizeof(FloSetupRunArgs) || args->msi == nullptr) {
        return static_cast<int>(ERROR_INVALID_PARAMETER);
    }
    Facts facts;
    facts.msi = args->msi;
    facts.log = args->log != nullptr ? args->log : L"";
    facts.licence = setup::dll_resource(kLicenceResource);
    facts.licence_sha256 = sha256_hex(facts.licence);
    std::wstring problem_heading;
    std::wstring problem_text;
    int problem_code = static_cast<int>(ERROR_INSTALL_PACKAGE_INVALID);
    if (facts.licence.empty()) {
        problem_heading = L"This setup is incomplete";
        problem_text = L"Its licence is missing. Download FloServer again from floserver.org.";
    } else if (const auto package = read_package(facts.msi); !package) {
        problem_heading = L"This setup is damaged";
        problem_text = L"The FloServer installer inside it can't be read. Download FloServer again from floserver.org.";
    } else {
        facts.ours = package->product;
        facts.installed_bytes = package->installed_bytes;
        facts.installed = installed_product();
        facts.mode = setup::decide_setup_mode(facts.installed, facts.ours);
        const std::string accepted = narrow(accepted_licence());
        facts.licence_changed = !accepted.empty() && accepted != facts.licence_sha256;
        if (facts.mode == setup::SetupMode::Newer) {
            problem_code = static_cast<int>(ERROR_PRODUCT_VERSION);
            problem_heading = L"A newer FloServer is installed";
            problem_text = L"FloServer " + widen(facts.installed->version) +
                           L" is already installed, which is newer than this setup (" + widen(facts.ours.version) +
                           L"). To go back to " + widen(facts.ours.version) +
                           L", remove FloServer first in Windows' Settings \u2192 Apps.";
        }
    }

    if (args->quiet != 0) {
        if (!problem_heading.empty()) {
            return problem_code;
        }
        setup::MsiRun run;
        const UINT result = setup::msi_install(facts.msi, setup::msi_properties(facts.mode), facts.log, run);
        if (result == ERROR_SUCCESS || result == ERROR_SUCCESS_REBOOT_REQUIRED) {
            remember_licence(facts.licence_sha256);
        }
        return static_cast<int>(result);
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    ULONG_PTR gdiplus = 0;
    const Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&gdiplus, &input, nullptr) != Gdiplus::Ok) {
        return static_cast<int>(ERROR_INSTALL_FAILURE);
    }
    int code = 0;
    {
        const bool licence_first = setup::licence_needed(facts.mode, narrow(accepted_licence()), facts.licence_sha256);
        const setup::SetupMode mode = facts.mode;
        SetupWindow window(setup::dll_instance(), std::move(facts));
        if (!problem_heading.empty()) {
            window.show_problem(problem_heading, problem_text, problem_code);
        } else {
            window.start_at(mode == setup::SetupMode::Install || licence_first ? Page::Licence : Page::Update);
        }
        if (window.create(args->show)) {
            MSG m{};
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                if (window.dialog_message(m)) {
                    continue;
                }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            code = window.exit_code();
        } else {
            code = static_cast<int>(ERROR_INSTALL_FAILURE);
        }
    }
    Gdiplus::GdiplusShutdown(gdiplus);
    return code;
}
