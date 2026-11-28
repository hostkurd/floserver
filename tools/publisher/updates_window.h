#pragma once

// D34: the Publisher's Updates window. "Check for updates" asks each
// runtime's official site what it offers now (updates.h) and lists, per
// series, what's newer than the next list: an Update (ticked), a New series
// (your choice) or Up to date, and why a site couldn't be checked.
// "Download only" gets the ticked ones from their official sites into a
// folder laid out as the CDN, with the upload steps, and adds them to the
// next list (D34.3, manual upload). "Get selected" does the same and
// uploads them to R2 with the key in Credential Manager (D34.4).

#include "updates.h"

#include <windows.h>

#include <commctrl.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace floserver::publisher {

struct FetchDone;  // what a Download only run did

class UpdatesWindow {
public:
    UpdatesWindow() = default;
    UpdatesWindow(const UpdatesWindow&) = delete;
    UpdatesWindow& operator=(const UpdatesWindow&) = delete;
    UpdatesWindow(UpdatesWindow&&) = delete;
    UpdatesWindow& operator=(UpdatesWindow&&) = delete;
    ~UpdatesWindow();

    // Opens the window (or brings it forward) and checks `listed`: runtime ->
    // the versions in the next list.
    void open(HWND owner, std::map<std::string, std::vector<std::string>> listed, manifest_tool::ListedBuilds builds = {});
    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    // Downloading or uploading now (Clean up doesn't delete meanwhile).
    [[nodiscard]] bool working() const { return downloading_; }
    // Why it mustn't download or upload now (Clean up is deleting), or empty.
    std::function<std::wstring()> blocked;
    // Called with the CDN paths downloaded; returns what to tell the user
    // about the next list (or why they couldn't be added).
    // `on_r2` says they're uploaded; otherwise they're still to upload by hand.
    std::function<std::wstring(const std::vector<std::string>& cdn_paths, bool on_r2)> on_fetched;

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void create_controls();
    void layout();
    void check();
    void stop_worker();
    void fill();
    void download(bool upload);
    void fetched(FetchDone& done);
    void set_buttons();
    void tick_all(bool ticked);
    void show_progress();
    LRESULT custom_draw(NMLVCUSTOMDRAW* cd) const;
    [[nodiscard]] int px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }

    HWND hwnd_ = nullptr;
    HWND heading_ = nullptr, hint_ = nullptr, list_ = nullptr, status_ = nullptr, summary_ = nullptr;
    HWND progress_ = nullptr, check_all_ = nullptr, uncheck_all_ = nullptr;
    HWND keep_answers_ = nullptr, check_again_ = nullptr, download_ = nullptr, get_ = nullptr, close_ = nullptr,
         key_ = nullptr;
    HFONT font_ = nullptr, heading_font_ = nullptr;
    HBRUSH bg_ = nullptr;
    UINT dpi_ = 96;

    std::map<std::string, std::vector<std::string>> listed_;
    manifest_tool::ListedBuilds builds_;  // our builds' numbers in the list (D36.3)
    std::vector<manifest_tool::UpdateRow> rows_;
    std::string saved_to_;  // where the sites' answers were kept, when asked
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    bool busy_ = false;         // checking
    std::size_t asked_ = 0;     // ...sites asked so far, of
    std::size_t to_ask_ = 0;    // ...this many
    bool bar_shown_ = false;    // the progress bar (the window itself may be hidden)
    bool downloading_ = false;  // Download only or Get selected
    bool uploading_ = false;    // ...Get selected
    std::vector<std::wstring> outcome_;  // per row: what Download only did ("" before)
    std::vector<bool> outcome_ok_;
    bool filling_ = false;  // fill() sets the ticks: its notifications aren't the user's
    UINT dpi_before_ = 96;  // for the columns at a DPI change
};

}  // namespace floserver::publisher
