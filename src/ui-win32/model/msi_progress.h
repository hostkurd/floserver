#pragma once

// Windows Installer's progress, read from its messages. Plain C++ so it's
// tested on every platform; FloServer.exe (updates, uninstall) and the
// setup program (D25) feed it from their message handlers.

namespace floserver::ui {

// Windows Installer's progress messages (INSTALLMESSAGE_PROGRESS, fields 1
// to 4), turned into how far the bar goes, as MSDN's "Parsing Windows
// Installer Messages" reads them: a reset gives the total, action data
// moves by its step, a report moves by its ticks, "add" makes the total larger.
class MsiProgress {
public:
    void on_record(int field1, int field2, int field3, int field4);
    // INSTALLMESSAGE_ACTIONDATA: one step, when the action said its data moves the bar.
    void on_action_data();
    // 0 to 1; 0 before the first reset.
    [[nodiscard]] double fraction() const;
    // True while Windows Installer only writes its script (nothing is done
    // yet): actions said meanwhile aren't the work itself.
    [[nodiscard]] bool writing_script() const { return script_; }

private:
    long long total_ = 0;
    long long done_ = 0;
    bool backward_ = false;
    bool script_ = false;  // writing the script, not doing it: the bar waits
    bool explicit_ = false;  // action data moves the bar
    long long step_ = 0;
};

}  // namespace floserver::ui
