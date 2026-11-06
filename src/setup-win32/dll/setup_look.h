#pragma once

// The look of FloServer's setup and uninstall screens (D25, approved with
// its mockups on 6 Nov): an 880 x 580 window with the blue FloServer panel on
// the left (its own title row, the feather, the name and a line under it),
// a step bar at the top right, the page under it and the buttons in a
// footer. Windows' own title bar is replaced by the window's own row (the
// window can still be moved, minimised and closed as usual, Alt+Space too).
//
// Part of floserver-setup.dll, which holds every setup and uninstall screen
// and their pictures, so FloServer.exe stays small: it needs only Win32,
// GDI+ and win_util.h, never FloServer's core. English only, light only (D21).
//
// A page is built from real controls where someone reads or presses
// something (texts, buttons, check boxes: screen readers and the keyboard
// know them) and painted parts around them (the panel, step bar, cards,
// marks, progress bar). Coordinates are in device-independent pixels inside
// the 880 x 580 window; build_page() runs again after a DPI change.

#include "win_util.h"  // src/ui-win32: header-only helpers

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::setup::look {

namespace win = floserver::ui::win;

inline constexpr int kWidth = 880;
inline constexpr int kHeight = 580;
inline constexpr int kPanelW = 288;
inline constexpr int kTitleH = 36;
inline constexpr int kContentX = kPanelW + 36;
inline constexpr int kContentR = kWidth - 36;
inline constexpr int kContentW = kContentR - kContentX;
inline constexpr int kFooterY = kHeight - 68;
inline constexpr int kButtonY = kFooterY + 16;
inline constexpr int kButtonH = 36;

// Colours (as the mockups have them).
inline constexpr COLORREF kWhite = RGB(0xFF, 0xFF, 0xFF);
inline constexpr COLORREF kText = RGB(0x1B, 0x1B, 0x1B);
inline constexpr COLORREF kText2 = RGB(0x3B, 0x3B, 0x3B);
inline constexpr COLORREF kMuted = RGB(0x5F, 0x5F, 0x5F);
inline constexpr COLORREF kAccent = RGB(0x0F, 0x6C, 0xBD);
inline constexpr COLORREF kAccentSoft = RGB(0xDC, 0xEB, 0xFA);
inline constexpr COLORREF kTintCard = RGB(0xF5, 0xF9, 0xFD);
inline constexpr COLORREF kGreyCard = RGB(0xF7, 0xF7, 0xF7);
inline constexpr COLORREF kBorder = RGB(0xE1, 0xE1, 0xE1);
inline constexpr COLORREF kDanger = RGB(0xC4, 0x2B, 0x1C);
inline constexpr COLORREF kDangerSoft = RGB(0xFD, 0xE7, 0xE9);

enum class ButtonKind : std::uint8_t { Primary, Secondary, Danger, Link };
enum class TextKind : std::uint8_t { Heading, Sub, Body, BodyBold, Note, NoteBold, Mono };
enum class MarkKind : std::uint8_t {
    Done,      // a step done: a tick in a pale blue circle
    Now,       // the step under way: a blue ring
    Next,      // a step still to come: a grey dot
    Info,      // an "i" in a circle, before a note
    BigCheck,  // the Done screens' large tick
    Warning,   // something went wrong: "!" in a pale red circle
};

class WizardWindow {
public:
    // `title`: the window's (taskbar, Alt+Tab, its title row); `panel_note`:
    // the line under "FloServer" in the panel; `version`: shown at the
    // bottom of the panel.
    WizardWindow(HINSTANCE instance, std::wstring title, std::wstring panel_note, std::wstring version);
    virtual ~WizardWindow();
    WizardWindow(const WizardWindow&) = delete;
    WizardWindow& operator=(const WizardWindow&) = delete;
    WizardWindow(WizardWindow&&) = delete;
    WizardWindow& operator=(WizardWindow&&) = delete;

    // Opens the window: at `at` (its top-left, screen pixels) when given,
    // else in the middle of the screen. False if it couldn't be made.
    bool create(int show, const POINT* at = nullptr);
    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    // Tab, Enter (the default button) and Esc (the cancel button) for this window.
    [[nodiscard]] bool dialog_message(MSG& msg) const;
    // Brings it to the front (another copy of it was started, say).
    void bring_forward() const;

protected:
    // --- building a page (from build_page()) ---
    // Clears the last page and sets the step bar (none when `steps` is empty).
    void start_page(std::vector<std::wstring> steps, int current);
    HWND add_text(RECT r, std::wstring_view text, TextKind kind, COLORREF background = kWhite,
                  COLORREF color = kText);
    HWND add_button(int id, RECT r, std::wstring_view text, ButtonKind kind, bool enabled = true);
    HWND add_check(int id, RECT r, std::wstring_view text, bool checked, bool enabled = true);
    // Any other control (a rich edit, say): it goes with the page.
    HWND adopt(HWND control, COLORREF background = kWhite);
    // A painted rounded box behind texts (give them `fill` as their background).
    void add_card(RECT r, COLORREF fill, COLORREF border);
    // A mark (20 x 20, or 56 x 56 for BigCheck) with its top-left at x, y; its index.
    int add_mark(int x, int y, MarkKind kind);
    void set_mark(int index, MarkKind kind);
    // A thin progress bar across `r`; set_progress() moves it (0 to 1).
    void add_progress(RECT r);
    void set_progress(double fraction);
    // The button Enter presses and the one Esc presses (0: none).
    void set_default_button(int id) { default_id_ = id; }
    void set_cancel_button(int id) { cancel_id_ = id; }

    void set_text(HWND control, std::wstring_view text) const;
    [[nodiscard]] bool checked(int id) const;
    void enable(int id, bool on) const;
    void focus(int id) const;
    [[nodiscard]] HWND control(int id) const;
    [[nodiscard]] int px(int dip) const;
    [[nodiscard]] RECT px(RECT dip) const;
    [[nodiscard]] HINSTANCE instance() const { return instance_; }
    [[nodiscard]] HFONT font(TextKind kind) const;
    // Builds the current page again (it changed).
    void rebuild();

    // --- what a window of this look does ---
    virtual void build_page() = 0;
    virtual void on_command(int id) = 0;
    // ✕, Alt+F4 or Esc with no cancel button: true lets the window close.
    virtual bool on_close_request() { return true; }
    // WM_APP and up, from the window's own threads.
    virtual void on_app_message(UINT /*message*/, WPARAM /*wparam*/, LPARAM /*lparam*/) {}
    // The window is gone (the default ends the message loop).
    virtual void on_destroyed() { PostQuitMessage(0); }

private:
    struct Mark {
        int x = 0;
        int y = 0;
        MarkKind kind = MarkKind::Next;
    };
    struct Card {
        RECT r{};
        COLORREF fill = kWhite;
        COLORREF border = kBorder;
    };
    struct Fonts {
        win::UniqueFont heading;
        win::UniqueFont sub;
        win::UniqueFont body;
        win::UniqueFont body_bold;
        win::UniqueFont note;
        win::UniqueFont note_bold;
        win::UniqueFont mono;
        win::UniqueFont button;
        win::UniqueFont button_bold;
        win::UniqueFont panel_title;
        win::UniqueFont panel_text;
        win::UniqueFont panel_small;
        win::UniqueFont step;
        win::UniqueFont step_bold;
        win::UniqueFont title;
    };
    enum : int { kMinimizeId = 9001, kCloseId = 9002 };

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK button_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                        DWORD_PTR data);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void make_fonts();
    void clear_controls();
    void paint(HDC dc) const;
    void draw_button(const DRAWITEMSTRUCT& item) const;
    void caption_buttons();
    [[nodiscard]] HBRUSH brush_for(COLORREF colour);
    bool request_close();  // true: it closed

    HINSTANCE instance_;
    std::wstring title_;
    std::wstring panel_note_;
    std::wstring version_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    Fonts fonts_;
    std::vector<std::wstring> steps_;
    int current_step_ = 0;
    std::vector<HWND> controls_;  // the page's (not the caption buttons)
    std::map<HWND, COLORREF> backgrounds_;
    std::map<HWND, COLORREF> colours_;
    std::map<HWND, ButtonKind> kinds_;
    std::map<COLORREF, win::UniqueBrush> brushes_;
    std::vector<Card> cards_;
    std::vector<Mark> marks_;
    RECT progress_{};
    bool has_progress_ = false;
    double fraction_ = 0;
    HWND hot_ = nullptr;  // the button under the mouse
    int default_id_ = 0;
    int cancel_id_ = 0;
};

}  // namespace floserver::setup::look
