#pragma once

// D37: the Publisher's Clean up window. It lists the files on R2 that
// neither the live list nor the next list uses (cleanup.h decides which,
// and which are ticked), and deletes the ticked ones with the R2 key in
// Credential Manager: one at a time, each row saying how it went, then
// purges them from Cloudflare's cache when the purge token is saved and
// writes a record to Documents\FloServer Publisher\cleanup <date> <time>.txt.
// Before deleting it reads the live list, the bucket and the next list
// again, so a file a list needs now is skipped.

#include "cleanup.h"

#include <windows.h>

#include <commctrl.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace floserver::publisher {

struct CleanupCheck;  // what a check found
struct CleanupStep;   // one row deleted, skipped or failed
struct CleanupRun;    // what Delete selected did

class CleanupWindow {
public:
    CleanupWindow() = default;
    CleanupWindow(const CleanupWindow&) = delete;
    CleanupWindow& operator=(const CleanupWindow&) = delete;
    CleanupWindow(CleanupWindow&&) = delete;
    CleanupWindow& operator=(CleanupWindow&&) = delete;
    ~CleanupWindow();

    // Opens the window (or brings it forward) and checks, unless it's busy.
    void open(HWND owner);
    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    // Deleting now: the main window doesn't publish meanwhile.
    [[nodiscard]] bool deleting() const { return deleting_; }
    // The main window's files (the next list): asked at each check, and
    // again when Delete selected is pressed.
    std::function<std::vector<std::string>()> next_paths;

private:
    // What happened to a row in this window's last run.
    enum class Outcome : std::uint8_t { None, Working, Deleted, Skipped, Failed };

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void make_fonts();
    void create_controls();
    void layout();
    void check();
    void stop_worker();
    void checked(CleanupCheck& found);
    void fill();
    void refresh_row(std::size_t row);
    void delete_selected();
    void step(const CleanupStep& s);
    void ran(const CleanupRun& run);
    void set_buttons();
    void tick_all(bool ticked);
    void ticks_changed();
    void update_summary();
    [[nodiscard]] bool tickable(std::size_t row) const;
    LRESULT custom_draw(NMLVCUSTOMDRAW* cd) const;
    [[nodiscard]] int px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }

    HWND hwnd_ = nullptr;
    HWND heading_ = nullptr, hint_ = nullptr, check_again_ = nullptr, check_all_ = nullptr, uncheck_all_ = nullptr;
    HWND summary_ = nullptr, list_ = nullptr, status_ = nullptr, warning_ = nullptr, delete_ = nullptr,
         close_ = nullptr;
    HFONT font_ = nullptr, heading_font_ = nullptr, small_font_ = nullptr;
    HBRUSH bg_ = nullptr;
    UINT dpi_ = 96;
    UINT dpi_before_ = 96;  // for the columns at a DPI change

    std::vector<manifest_tool::CleanupRow> rows_;
    std::vector<std::string> plan_notes_;  // each row's note as planned (tie_sources may change it)
    std::vector<Outcome> outcome_;
    std::vector<std::wstring> outcome_text_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    bool checking_ = false;
    bool deleting_ = false;
    bool filling_ = false;  // ticks set by the window, not the user
    bool ran_ = false;      // the rows show a run's outcome
    std::uint64_t freed_ = 0;
    std::size_t deleted_ = 0;
    std::size_t run_total_ = 0;  // rows in the run going on
    std::size_t run_done_ = 0;   // ...finished so far
};

}  // namespace floserver::publisher
