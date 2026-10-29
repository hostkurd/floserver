#pragma once

// Small Win32 helpers for the window: UTF-8 <-> UTF-16 at the Win32 boundary
// and owners for handles, so nothing is freed by hand.

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace floserver::ui::win {

inline std::wstring widen(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string narrow(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Win32 messages carry pointers and handles in WPARAM / LPARAM.
template <typename P, typename I>
P int_to_ptr(I value) {
    return reinterpret_cast<P>(value);  // NOLINT(performance-no-int-to-ptr): the Win32 message convention
}

struct GdiDeleter {
    void operator()(HGDIOBJ h) const noexcept { DeleteObject(h); }
};
struct IconDeleter {
    void operator()(HICON h) const noexcept { DestroyIcon(h); }
};
struct HandleDeleter {
    void operator()(HANDLE h) const noexcept { CloseHandle(h); }
};
struct DcDeleter {
    void operator()(HDC h) const noexcept { DeleteDC(h); }
};
struct MenuDeleter {
    void operator()(HMENU h) const noexcept { DestroyMenu(h); }
};

template <typename H>
using UniqueGdi = std::unique_ptr<std::remove_pointer_t<H>, GdiDeleter>;
using UniqueFont = UniqueGdi<HFONT>;
using UniqueBrush = UniqueGdi<HBRUSH>;
using UniqueBitmap = UniqueGdi<HBITMAP>;
using UniqueIcon = std::unique_ptr<std::remove_pointer_t<HICON>, IconDeleter>;
using UniqueHandle = std::unique_ptr<void, HandleDeleter>;
using UniqueDc = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;
using UniqueMenu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;

// Selects an object into a DC and puts the old one back when it goes.
class Selected {
public:
    Selected(HDC dc, HGDIOBJ obj) : dc_(dc), old_(SelectObject(dc, obj)) {}
    ~Selected() { SelectObject(dc_, old_); }
    Selected(const Selected&) = delete;
    Selected& operator=(const Selected&) = delete;
    Selected(Selected&&) = delete;
    Selected& operator=(Selected&&) = delete;

private:
    HDC dc_;
    HGDIOBJ old_;
};

}  // namespace floserver::ui::win
