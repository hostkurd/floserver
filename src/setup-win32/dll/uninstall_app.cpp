// The uninstall screens (D25, approved with their mockups 6 Nov), in
// floserver-setup.dll. Windows' Settings -> Apps -> FloServer -> Uninstall
// and Settings -> General -> Remove FloServer… both open them through
// FloServer.exe, which does the clean-up itself (it owns the service's link):
//
//   Choose:    what else to delete, then Remove (FloServer.exe is told);
//   Removing:  FloServer.exe says each step as it starts (the certificate,
//              stopping FloServer, deleting what was ticked);
//   then FloServer.exe's copy in %TEMP% opens the same page at its last
//   step, Removing the program, runs Windows' uninstall with Windows
//   Installer's screens off and its progress on the bar, and shows Done.
//
// English only (D21, D25).

#include "common.h"
#include "floserver/setup_api.h"
#include "setup_look.h"
#include "win_util.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <memory>
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

constexpr wchar_t kTitle[] = L"Uninstall FloServer";
constexpr wchar_t kPanelNote[] = L"Thanks for using FloServer.";

enum : int {
    kRemove = 200,
    kCancel,
    kClose,
    kOpenData,
    kChoiceFirst = 220,  // + the box's index
};

constexpr int kRemoveSteps = 4;

struct Choice {
    unsigned bit = 0;
    std::wstring label;
    std::wstring note;
    bool checked = false;
};

enum class Page : std::uint8_t { Choose, Removing, Done, Message };

class UninstallWindow final : public look::WizardWindow {
public:
    UninstallWindow(const FloUninstallHost* host, std::wstring version)
        : WizardWindow(setup::dll_instance(), kTitle, kPanelNote, L"Version " + version + L" · HostKurd\nfloserver.org") {
        if (host != nullptr) {
            host_ = *host;
            in_app_ = true;
        }
    }
    ~UninstallWindow() override {
        run_->detach();
        if (worker_.joinable()) {
            worker_.join();
        }
    }
    UninstallWindow(const UninstallWindow&) = delete;
    UninstallWindow& operator=(const UninstallWindow&) = delete;
    UninstallWindow(UninstallWindow&&) = delete;
    UninstallWindow& operator=(UninstallWindow&&) = delete;

    // --- FloServer.exe's window, through the exports ---
    void set_choices(const FloUninstallChoice* items, std::uint32_t count) {
        std::vector<Choice> next;
        for (std::uint32_t i = 0; i < count; ++i) {
            const FloUninstallChoice& c = items[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): the caller's array
            Choice choice{c.bit, c.label != nullptr ? c.label : L"", c.note != nullptr ? c.note : L"", c.checked != 0};
            // A box the user already changed keeps its tick.
            for (const Choice& old : choices_) {
                if (old.bit == choice.bit) {
                    choice.checked = old.checked;
                }
            }
            next.push_back(std::move(choice));
        }
        const bool same_layout = next.size() == choices_.size();
        choices_ = std::move(next);
        if (page_ != Page::Choose) {
            return;
        }
        if (same_layout && !note_texts_.empty()) {
            for (std::size_t i = 0; i < choices_.size() && i < note_texts_.size(); ++i) {
                set_text(note_texts_[i], choices_[i].note);
            }
        } else {
            rebuild();
        }
    }
    void set_step(int step) {
        if (page_ != Page::Removing) {
            page_ = Page::Removing;
            step_ = step;
            rebuild();
            return;
        }
        step_ = step;
        rebuild();
    }
    void show_message(std::wstring heading, std::wstring text, bool done) {
        page_ = Page::Message;
        message_heading_ = std::move(heading);
        message_text_ = std::move(text);
        message_done_ = done;
        rebuild();
    }
    void close_quietly() {
        quiet_close_ = true;
        if (hwnd() != nullptr) {
            DestroyWindow(hwnd());
        }
    }

    // --- FloServer.exe's copy in %TEMP%: the last step ---
    void remove_program(const FloRemoveProgramArgs& args) {
        product_ = args.product != nullptr ? args.product : L"";
        deleted_ = args.deleted != nullptr ? args.deleted : L"";
        kept_ = args.kept != nullptr ? args.kept : L"";
        problems_ = args.problems != nullptr ? args.problems : L"";
        data_dir_ = args.data_dir != nullptr ? args.data_dir : L"";
        page_ = Page::Removing;
        step_ = 3;
    }
    void start_removing() {
        {
            const std::lock_guard lock(run_->mutex);
            run_->window = hwnd();
        }
        worker_ = std::thread([run = run_, product = product_] {
            const UINT result = setup::msi_remove(product, *run);
            run->post(setup::kDoneMessage, result);
        });
    }
    [[nodiscard]] int result() const { return result_; }

private:
    void build_page() override {
        switch (page_) {
            case Page::Choose:
                build_choose();
                break;
            case Page::Removing:
                build_removing();
                break;
            case Page::Done:
                build_done();
                break;
            case Page::Message:
                build_message();
                break;
        }
    }

    void heading(std::wstring_view title, std::wstring_view sub) {
        add_text({look::kContentX, 82, look::kContentR, 120}, title, look::TextKind::Heading);
        add_text({look::kContentX, 122, look::kContentR, 166}, sub, look::TextKind::Sub, look::kWhite, look::kMuted);
    }

    void build_choose() {
        start_page({L"Choose", L"Remove", L"Done"}, 0);
        note_texts_.clear();
        heading(L"Remove FloServer", L"FloServer stops your projects, then removes the program from this computer.");
        add_card({look::kContentX, 160, look::kContentR, 220}, look::kGreyCard, look::kGreyCard);
        add_mark(look::kContentX + 14, 170, look::MarkKind::Info);
        add_text({look::kContentX + 40, 168, look::kContentR - 14, 214},
                 L"Always removed: the programs, the Start menu shortcut, flo on your Path, Start with Windows and "
                 L"FloServer's certificate (Windows asks you once).",
                 look::TextKind::Note, look::kGreyCard, look::kText2);
        add_text({look::kContentX, 234, look::kContentR, 254}, L"Also delete", look::TextKind::NoteBold);
        const int top = 256;
        const int row = 64;
        add_card({look::kContentX, top, look::kContentR, top + (row * static_cast<int>(choices_.size())) + 4}, look::kWhite,
                 look::kBorder);
        for (std::size_t i = 0; i < choices_.size(); ++i) {
            const int y = top + 10 + (row * static_cast<int>(i));
            add_check(kChoiceFirst + static_cast<int>(i), {look::kContentX + 16, y, look::kContentR - 16, y + 24},
                      choices_[i].label, choices_[i].checked);
            note_texts_.push_back(add_text({look::kContentX + 42, y + 24, look::kContentR - 16, y + 58}, choices_[i].note,
                                           look::TextKind::Note, look::kWhite, look::kMuted));
        }
        int x = look::kContentR - 96;
        add_button(kCancel, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Cancel",
                   look::ButtonKind::Secondary);
        x -= 10 + 96;
        add_button(kRemove, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Remove",
                   look::ButtonKind::Danger);
        // Nothing is deleted on Enter alone: Cancel has the focus first.
        set_cancel_button(kCancel);
    }

    [[nodiscard]] look::MarkKind mark_for(int index) const {
        if (index < step_) {
            return look::MarkKind::Done;
        }
        return index == step_ ? look::MarkKind::Now : look::MarkKind::Next;
    }

    void build_removing() {
        start_page({L"Choose", L"Remove", L"Done"}, 1);
        heading(L"Removing FloServer", L"This takes a moment. Please keep this window open.");
        add_progress({look::kContentX, 184, look::kContentR, 190});
        set_progress((static_cast<double>(step_) + msi_fraction_) / kRemoveSteps);
        const std::array<std::wstring_view, kRemoveSteps> names{
            L"Removing FloServer's certificate", L"Stopping your projects and FloServer", L"Deleting what you chose",
            L"Removing the program"};
        int y = 216;
        for (int i = 0; i < kRemoveSteps; ++i) {
            add_mark(look::kContentX, y, mark_for(i));
            add_text({look::kContentX + 32, y, look::kContentR, y + 22}, names.at(static_cast<std::size_t>(i)),
                     i == step_ ? look::TextKind::BodyBold : look::TextKind::Body, look::kWhite,
                     i > step_ ? RGB(0x61, 0x61, 0x61) : look::kText);
            y += 36;
        }
        const int x = look::kContentR - 96;
        add_button(kCancel, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Cancel",
                   look::ButtonKind::Secondary, false);
    }

    void build_done() {
        start_page({L"Choose", L"Remove", L"Done"}, 3);
        add_mark(look::kContentX, 104, look::MarkKind::BigCheck);
        add_text({look::kContentX, 176, look::kContentR, 214}, L"FloServer has been removed", look::TextKind::Heading);
        add_text({look::kContentX, 216, look::kContentR, 256}, deleted_, look::TextKind::Sub, look::kWhite, look::kMuted);
        int y = 268;
        if (!kept_.empty()) {
            const int lines = 1 + static_cast<int>(std::count(kept_.begin(), kept_.end(), L'\n'));
            const int height = 34 + (lines * 34) + (data_dir_.empty() ? 0 : 28);
            add_card({look::kContentX, y, look::kContentR, y + height}, look::kWhite, look::kBorder);
            add_text({look::kContentX + 16, y + 12, look::kContentR - 16, y + 32}, L"Kept, as you chose",
                     look::TextKind::NoteBold);
            add_text({look::kContentX + 16, y + 34, look::kContentR - 16, y + 34 + (lines * 34)}, kept_, look::TextKind::Note,
                     look::kWhite, look::kText2);
            if (!data_dir_.empty()) {
                add_button(kOpenData, {look::kContentX + 12, y + height - 30, look::kContentX + 180, y + height - 8},
                           L"Open the data folder", look::ButtonKind::Link);
            }
            y += height + 12;
        }
        if (!problems_.empty()) {
            add_text({look::kContentX, y, look::kContentR, look::kFooterY - 8}, problems_, look::TextKind::Note, look::kWhite,
                     look::kDanger);
        }
        const int x = look::kContentR - 96;
        add_button(kClose, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Close",
                   look::ButtonKind::Primary);
        set_default_button(kClose);
        set_cancel_button(kClose);
    }

    void build_message() {
        start_page({}, 0);
        add_mark(look::kContentX, 112, message_done_ ? look::MarkKind::BigCheck : look::MarkKind::Warning);
        add_text({look::kContentX, 186, look::kContentR, 224}, message_heading_, look::TextKind::Heading);
        add_text({look::kContentX, 230, look::kContentR, look::kFooterY - 8}, message_text_, look::TextKind::Sub,
                 look::kWhite, look::kText2);
        const int x = look::kContentR - 96;
        add_button(kClose, {x, look::kButtonY, x + 96, look::kButtonY + look::kButtonH}, L"Close",
                   look::ButtonKind::Primary);
        set_default_button(kClose);
        set_cancel_button(kClose);
    }

    void on_command(int id) override {
        if (id >= kChoiceFirst && id < kChoiceFirst + static_cast<int>(choices_.size())) {
            choices_[static_cast<std::size_t>(id - kChoiceFirst)].checked = checked(id);
            return;
        }
        switch (id) {
            case kRemove: {
                unsigned ticks = 0;
                for (const Choice& c : choices_) {
                    if (c.checked) {
                        ticks |= c.bit;
                    }
                }
                page_ = Page::Removing;
                step_ = 0;
                rebuild();
                if (host_.remove != nullptr) {
                    host_.remove(host_.context, ticks);
                }
                break;
            }
            case kCancel:
            case kClose:
                DestroyWindow(hwnd());
                break;
            case kOpenData:
                ShellExecuteW(hwnd(), L"open", data_dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            default:
                break;
        }
    }

    bool on_close_request() override {
        // While FloServer is being removed, the window stays: it says when it's done.
        return page_ != Page::Removing;
    }

    void on_app_message(UINT message, WPARAM wparam, LPARAM /*lparam*/) override {
        if (message == setup::kProgressMessage && page_ == Page::Removing) {
            msi_fraction_ = static_cast<double>(wparam) / 1000;
            set_progress((static_cast<double>(step_) + msi_fraction_) / kRemoveSteps);
        } else if (message == setup::kDoneMessage) {
            if (worker_.joinable()) {
                worker_.join();
            }
            result_ = static_cast<int>(wparam);
            const auto r = static_cast<UINT>(wparam);
            if (r == ERROR_SUCCESS || r == ERROR_SUCCESS_REBOOT_REQUIRED) {
                forget_licence();
                if (r == ERROR_SUCCESS_REBOOT_REQUIRED) {
                    problems_ += (problems_.empty() ? L"" : L"\n") + std::wstring(L"Restart Windows to finish.");
                }
                page_ = Page::Done;
                rebuild();
            } else {
                std::wstring error;
                {
                    const std::lock_guard lock(run_->mutex);
                    error = run_->error;
                }
                if (error.empty()) {
                    error = setup::error_words(r);
                }
                show_message(L"FloServer's program wasn't removed",
                             error + L"\n\nWhat you chose to delete is gone; the program itself is still installed. "
                                     L"Try again from Windows' Settings → Apps.",
                             false);
            }
        }
    }

    // The licence the setup noted as accepted, and FloServer's registry key
    // if nothing else is left in it.
    static void forget_licence() {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, setup::kRegistryKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key) ==
            ERROR_SUCCESS) {
            RegDeleteValueW(key, setup::kAcceptedLicenceValue);
            RegCloseKey(key);
        }
        RegDeleteKeyW(HKEY_CURRENT_USER, setup::kRegistryKey);  // only when it has no subkeys; values go with it
    }

    void on_destroyed() override {
        if (in_app_) {
            if (!quiet_close_ && host_.closed != nullptr) {
                host_.closed(host_.context);
            }
            return;  // FloServer's own message loop goes on
        }
        PostQuitMessage(result_);
    }

    FloUninstallHost host_{};
    bool in_app_ = false;
    bool quiet_close_ = false;
    Page page_ = Page::Choose;
    std::vector<Choice> choices_;
    std::vector<HWND> note_texts_;
    int step_ = 0;
    double msi_fraction_ = 0;
    std::wstring message_heading_;
    std::wstring message_text_;
    bool message_done_ = false;
    std::wstring product_;
    std::wstring deleted_;
    std::wstring kept_;
    std::wstring problems_;
    std::wstring data_dir_;
    int result_ = 0;
    std::shared_ptr<setup::MsiRun> run_ = std::make_shared<setup::MsiRun>();
    std::thread worker_;
};

UninstallWindow* window_of(FloUninstall* h) { return reinterpret_cast<UninstallWindow*>(h); }  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the handle is the window

// GDI+ for as long as the DLL is used in this process (FloServer.exe has
// its own session too; GdiplusStartup counts).
bool gdiplus_ready() {
    static const bool ok = [] {
        ULONG_PTR token = 0;
        const Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ok;
}

}  // namespace

FLOSERVER_SETUP_API FloUninstall* __stdcall floserver_uninstall_open(const FloUninstallHost* host, int show) {
    if (host == nullptr || host->size < sizeof(FloUninstallHost) || !gdiplus_ready()) {
        return nullptr;
    }
    auto window = std::make_unique<UninstallWindow>(host, host->version != nullptr ? host->version : L"");
    if (!window->create(show)) {
        return nullptr;
    }
    return reinterpret_cast<FloUninstall*>(window.release());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): an opaque handle
}

FLOSERVER_SETUP_API void __stdcall floserver_uninstall_choices(FloUninstall* h, const FloUninstallChoice* choices,
                                                               std::uint32_t count) {
    if (h != nullptr && (choices != nullptr || count == 0)) {
        window_of(h)->set_choices(choices, count);
    }
}

FLOSERVER_SETUP_API void __stdcall floserver_uninstall_step(FloUninstall* h, int step) {
    if (h != nullptr) {
        window_of(h)->set_step(step);
    }
}

FLOSERVER_SETUP_API void __stdcall floserver_uninstall_message(FloUninstall* h, const wchar_t* heading, const wchar_t* text,
                                                               int done) {
    if (h != nullptr) {
        window_of(h)->show_message(heading != nullptr ? heading : L"", text != nullptr ? text : L"", done != 0);
    }
}

FLOSERVER_SETUP_API int __stdcall floserver_uninstall_place(FloUninstall* h, POINT* top_left) {
    RECT r{};
    if (h == nullptr || top_left == nullptr || window_of(h)->hwnd() == nullptr ||
        GetWindowRect(window_of(h)->hwnd(), &r) == FALSE) {
        return 0;
    }
    *top_left = {r.left, r.top};
    return 1;
}

FLOSERVER_SETUP_API int __stdcall floserver_uninstall_dialog_message(FloUninstall* h, MSG* msg) {
    return h != nullptr && msg != nullptr && window_of(h)->dialog_message(*msg) ? 1 : 0;
}

FLOSERVER_SETUP_API void __stdcall floserver_uninstall_forward(FloUninstall* h) {
    if (h != nullptr && window_of(h)->hwnd() != nullptr) {
        window_of(h)->bring_forward();
    }
}

FLOSERVER_SETUP_API void __stdcall floserver_uninstall_close(FloUninstall* h) {
    if (h == nullptr) {
        return;
    }
    const std::unique_ptr<UninstallWindow> window(window_of(h));
    window->close_quietly();
}

FLOSERVER_SETUP_API int __stdcall floserver_remove_program(const FloRemoveProgramArgs* args) {
    if (args == nullptr || args->size < sizeof(FloRemoveProgramArgs) || args->product == nullptr ||
        !gdiplus_ready()) {
        return static_cast<int>(ERROR_INVALID_PARAMETER);
    }
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    UninstallWindow window(nullptr, args->version != nullptr ? args->version : L"");
    window.remove_program(*args);
    if (!window.create(SW_SHOWNORMAL, &args->top_left)) {
        return static_cast<int>(ERROR_INSTALL_FAILURE);
    }
    window.start_removing();
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (window.dialog_message(m)) {
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return window.result();
}
