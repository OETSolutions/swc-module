#pragma once

#include <stdint.h>

#include "HAL/IHAL.h"

/*
 * Maintenance mode (FR-32..FR-38): the only state in which the radio exists.
 *
 * **The radio is never initialised in normal operation**, and that is the whole
 * point of this class. FR-32 makes WiFi and BLE maintenance-only, so a device
 * serving button presses has no radio, no TCP stack and no provisioning
 * endpoint listening. Leaving maintenance must be as reliable as entering it,
 * because a device stuck in maintenance cannot serve a steering wheel.
 *
 * **THIS CLASS DOES NOT TOUCH THE RADIO.** It is pure state: which trigger opened
 * the window, whether it is open, and when it must close. The device-only work
 * (NimBLE, `wifi_provisioning`, the web server, both OTA paths) is driven by the
 * caller in response to `Active()` transitions and lives in the
 * maintenance-only translation units. Keeping the decision here is what makes
 * the timeout -- the part that decides whether a user is stranded -- host-testable.
 *
 * Note the file extension: this is C++, not C. The plan's file list says
 * `MaintenanceMode.c`, but its own tests construct a `class MaintenanceMode` and
 * call methods on it, which cannot compile as C. Same defect as Task 14's
 * `EspHal.c`.
 */

// Why the window opened. Distinct values because the caller's shutdown path
// differs: a USB command should get an acknowledgement, an AUX1 hold gets a
// buzzer, and booting with no config is the one case that must explain itself on
// the LED.
enum class MaintenanceTrigger {
    kNone = 0,
    kUsbCommand,
    kConfigFlag,
    kAux1Hold,
    kNoConfigAtBoot,   // FR-25's pass-through device offers provisioning at boot
};

class MaintenanceMode {
public:
    // `timeout_ms` defaults to spec 8.2's 5 minutes. A caller with a config may
    // pass `settings.maintenance_timeout_ms` instead; the default exists so a
    // bare device still has a bounded window rather than an unbounded one.
    explicit MaintenanceMode(IHAL *hal, uint32_t timeout_ms = 300000);

    // Opens the window. Always succeeds -- refusing a maintenance request would
    // leave a user with no way to provision a device that needs it -- and always
    // resets the activity clock, so a re-entry after a timeout gets a fresh
    // window rather than inheriting the previous elapsed time.
    bool Enter(MaintenanceTrigger t, uint64_t now_ms);

    // Closes the window immediately. Idempotent.
    void Exit();

    bool Active() const { return active_; }

    // Bumps the activity clock, so a user actively working in the web UI or
    // typing a PoP is not kicked out mid-task.
    void NoteActivity(uint64_t now_ms);

    // Advances the timeout. Closes the window once `timeout_ms` has elapsed
    // since the last activity.
    void Update(uint64_t now_ms);

    // True once the window should have closed; the caller uses it to know a
    // transition just happened and that the radio must be torn down.
    bool ShouldTimeout(uint64_t now_ms) const;

    // Whether maintenance takes the device away from normal operation. It does
    // NOT: the orchestrator keeps ticking, so a press still works while the
    // radio is up. FR-38's reason for a bounded window is that a device unable
    // to serve input is unacceptable -- so this must stay false.
    bool Exclusive() const { return false; }

    MaintenanceTrigger Trigger() const { return trigger_; }
    uint64_t LastActivity() const { return last_activity_; }

private:
    IHAL              *hal_;
    uint32_t           timeout_ms_;
    bool               active_ = false;
    MaintenanceTrigger trigger_ = MaintenanceTrigger::kNone;
    uint64_t           last_activity_ = 0;
};
