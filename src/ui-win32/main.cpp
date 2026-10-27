// FloServer.exe: the Windows window (D1, D2). Plain Win32 with common
// controls v6 and per-monitor DPI (both from the manifest). This first
// version opens an empty window so packaging and the size budget can start.

#include "floserver/core/build_info.h"

#include <windows.h>

#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"FloServerMainWindow";

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev*/, PWSTR /*cmd*/, int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        return 1;
    }

    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(960, static_cast<int>(dpi), 96);
    const int height = MulDiv(640, static_cast<int>(dpi), 96);
    const std::string version = floserver::core::kVersion;
    const std::wstring title = L"FloServer " + std::wstring(version.begin(), version.end());

    HWND hwnd = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr,
                                instance, nullptr);
    if (hwnd == nullptr) {
        return 1;
    }
    ShowWindow(hwnd, show);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
