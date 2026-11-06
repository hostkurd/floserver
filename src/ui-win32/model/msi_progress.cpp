#include "model/msi_progress.h"

#include <algorithm>

namespace floserver::ui {

void MsiProgress::on_record(int field1, int field2, int field3, int field4) {
    switch (field1) {
        case 0:  // reset: the total, which way, and whether it's only the script being written
            total_ = std::max(0, field2);
            backward_ = field3 == 1;
            done_ = backward_ ? total_ : 0;
            script_ = field4 == 1;
            explicit_ = false;
            break;
        case 1:  // action info: action data moves the bar by field2 when field3 says so
            step_ = std::max(0, field2);
            explicit_ = field3 == 1;
            break;
        case 2:  // a report: field2 ticks done
            if (total_ > 0) {
                done_ += backward_ ? -static_cast<long long>(field2) : static_cast<long long>(field2);
            }
            break;
        case 3:  // more to do: the total grows
            total_ += std::max(0, field2);
            break;
        default:
            break;
    }
}

void MsiProgress::on_action_data() {
    if (explicit_ && total_ > 0) {
        done_ += backward_ ? -step_ : step_;
    }
}

double MsiProgress::fraction() const {
    if (total_ <= 0 || script_) {
        return 0;
    }
    return std::clamp(static_cast<double>(done_) / static_cast<double>(total_), 0.0, 1.0);
}

}  // namespace floserver::ui
