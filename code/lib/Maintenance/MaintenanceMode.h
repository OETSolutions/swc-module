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

/*
 * Why the window opened, for a caller that wants to say so.
 *
 * **All four triggers now have sources (N-13 closed 2026-09-25).** The USB
 * command, the 3 s AUX1 hold, spec 8.2's "config flag on next boot"
 * (`settings.maintenance_on_boot`, consumed by `SystemOrchestrator::Boot`), and
 * `kNoConfigAtBoot` (a power-on with no config, keyed on `IHAL::reset_reason`).
 * The earlier note that `kNoConfigAtBoot` was "redundant" because "a device with
 * no config already reaches pass-through (FR-25)" was WRONG: pass-through means
 * the device keeps SERVING presses, which is not the same as OFFERING the
 * provisioning window a first-time user needs — and it predated `reset_reason`
 * existing at all.
 *
 * The distinction now has consequences, which is what it lacked before: the AUX1
 * hold plays `PROGRAM_ENTER` (the no-app user's only channel), and the
 * `maintenance` frame carries `trigger` so the app can say "Opened by: …" —
 * before this, a user holding AUX1 and a user tapping a button saw identical
 * feedback and nothing could tell them apart.
 */
enum class MaintenanceTrigger {
    kNone = 0,
    kUsbCommand,
    kConfigFlag,
    kAux1Hold,
    kNoConfigAtBoot,   // FR-25's pass-through device offers provisioning at boot
};

/*
 * The trigger's wire name for the `maintenance` frame.
 *
 * One home for the spelling, shared by the emitter and its test, so a renamed
 * enumerator cannot silently change the word the app is branching on. `kNone` maps
 * to "none" rather than an empty string: the app distinguishes "the window opened
 * for no recorded reason" from "the window is closed" by `active`, not by this
 * field, and an empty value would read as a missing field.
 */
const char *MaintenanceTriggerName(MaintenanceTrigger t);

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

    /*
     * Bumps the activity clock, so a user actively working in the web UI or
     * typing a PoP is not kicked out mid-task.
     *
     * **The window is measured from the last activity, not from `Enter`** (N-35,
     * resolved with the radio): the maintenance HTTP server counts the requests it
     * serves and `main.cpp`'s poll loop wrappers this call on every change, so the
     * close is the "5 minutes of inactivity" spec 8.2 and FR-38 describe. The
     * wrapper is `SystemOrchestratorNoteMaintenanceActivity`; the source is
     * `MaintenanceRadioRequestCount`.
     *
     * This file stays free of that wiring on purpose -- the HTTP server is
     * device-only and this class is host-tested, so naming it here would cost this
     * state machine its tests.
     */
    void NoteActivity(uint64_t now_ms);

    /*
     * Adopt a new timeout WITHOUT touching the live window.
     *
     * A config arriving over the link carries its own `maintenance_timeout_ms`,
     * and the obvious `maintenance_ = MaintenanceMode(hal, new_timeout)` is a
     * bug: the freshly constructed object is INACTIVE, so applying a config while
     * the provisioning web page is open would silently tear the window down
     * mid-provision. The radio is brought up and taken down by the caller on
     * `Active()` TRANSITIONS, so that state change is the one a user would have
     * to explain as "the setup page vanished while I was typing a password".
     *
     * Applies to the window in progress too: the new config's timeout is what the
     * device is running, and leaving the old one in force until re-entry would
     * make two answers to "how long is the window".
     */
    void SetTimeout(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }

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
