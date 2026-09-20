#include "Maintenance/MaintenanceMode.h"

MaintenanceMode::MaintenanceMode(IHAL *hal, uint32_t timeout_ms)
    : hal_(hal), timeout_ms_(timeout_ms) {}

bool MaintenanceMode::Enter(MaintenanceTrigger t, uint64_t now_ms) {
    (void)hal_;   // the HAL is held for the caller's benefit; the logic is pure.
    active_ = true;
    trigger_ = t;
    // Always a FRESH window. Reusing the previous window's start would make a
    // re-entry expire immediately, so the user who just asked for maintenance
    // would be thrown straight back out.
    last_activity_ = now_ms;
    return true;
}

void MaintenanceMode::Exit() {
    active_ = false;
    trigger_ = MaintenanceTrigger::kNone;
    // last_activity_ is deliberately NOT cleared: ShouldTimeout on a closed
    // window is meaningless, and a stale value is more diagnosable than zero.
}

void MaintenanceMode::NoteActivity(uint64_t now_ms) {
    if (!active_) return;
    last_activity_ = now_ms;
}

bool MaintenanceMode::ShouldTimeout(uint64_t now_ms) const {
    if (!active_) return false;
    // Unsigned subtraction, so a caller that passes a `now` earlier than the last
    // activity (a clock rewind, or a caller bug) yields a huge value and would
    // look like a timeout. Guard it: an earlier `now` means no elapsed time has
    // been observed, which is "not yet timed out", not "timed out long ago".
    if (now_ms < last_activity_) return false;
    return (now_ms - last_activity_) >= timeout_ms_;
}

void MaintenanceMode::Update(uint64_t now_ms) {
    if (ShouldTimeout(now_ms)) {
        // Exit() rather than clearing the flag here, so there is exactly one
        // place that closes a window and a future field cannot be forgotten in
        // the timeout path.
        Exit();
    }
}
