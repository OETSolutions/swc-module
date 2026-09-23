#pragma once

#include "Maintenance/MaintenanceInfo.h"

/*
 * The maintenance RADIO (spec 8.1/8.2/8.3/8.4, FR-32/FR-34), which is the one
 * piece of the maintenance path that touches hardware and the network stack.
 *
 * **Why this is a separate translation unit rather than a method on
 * `MaintenanceMode`.** `MaintenanceMode` is pure state and is HOST-TESTED; the
 * moment it includes `esp_wifi.h` it joins the two files the host build already
 * excludes (`EspHal.cpp`, `UsbLink.cpp`) and the state machine loses its tests.
 * Spec 8.2 says the radio work is "driven by the caller in response to
 * `Active()` transitions", and this header is that caller's interface. It is the
 * THIRD file the host build excludes, added to `build_src_filter` for the same
 * reason as the other two.
 *
 * **FR-32 is why the entry points are start/stop and not begin-forever.** The
 * radio must not exist in normal operation -- it is the largest power, RAM and
 * attack-surface item in the design -- so `MaintenanceRadioStop()` de-initialises
 * the stacks and frees them, and the orchestrator's poll loop calls it the moment
 * `MaintenanceActive()` goes false. A leaked radio is a device that still has
 * NimBLE and a WiFi driver resident while it is driving a car's steering wheel.
 *
 * **This is DEVICE-ONLY and its coverage is `test/test_hw`.** NimBLE, the WiFi
 * driver, `wifi_provisioning`, the SRP6a handshake and the HTTP server cannot run
 * on the host, and nothing here pretends otherwise. What IS host-testable stays in
 * `MaintenanceMode`, `BleProvisioning` (the PoP derivation, the Sec0 gate),
 * `WebPage` (the token derivation, the asset lookup) and `MaintenanceInfo`
 * (the value the app is told), all of which are tested.
 */

/*
 * Bring up the maintenance radio: NimBLE + the `wifi_provisioning` BLE transport
 * (the Espressif provisioning app's path, FR-34), the WiFi driver in AP+STA, and
 * the HTTP server serving `WebPage`'s asset plus the `api` endpoints of spec 8.4.
 *
 * Idempotent: a second call while the radio is up is the same as the first.
 *
 * Returns false if any piece failed, having torn down whatever did come up -- a
 * half-started radio is exactly the state FR-32 forbids, and reporting success
 * while NimBLE is down would make the app display a PoP for a session that cannot
 * receive it.
 *
 * `out` receives the window's public facts (PoP, token, URL, BLE name) on
 * success, for the router to publish to the app (spec 8.3 option 1). It is filled
 * only on success, so a caller that ignores the return value cannot show a user a
 * secret from a radio that did not come up.
 */
bool MaintenanceRadioStart(MaintenanceInfo *out);

/*
 * Tear everything down and FREE it (spec 8.2's wording). Safe when the radio is
 * already down, so the poll loop can call it on every transition to inactive
 * without tracking whether it ran.
 */
void MaintenanceRadioStop(void);

// True while the radio is up. Diagnostic: the poll loop uses it to notice a start
// that failed rather than retrying blindly on every tick.
bool MaintenanceRadioActive(void);

/*
 * Copy the open window's public facts into `out` (spec 8.3 option 1). Fills only
 * the four display fields -- `active` is the caller's to set, because the WINDOW
 * is the orchestrator's state and the radio is a consequence of it.
 *
 * A no-op on a closed radio, which is what keeps a stale secret off the wire: if
 * the radio never came up, the fields stay empty and the app shows an open window
 * with no provisioning details rather than the last session's.
 */
void MaintenanceRadioDescribe(MaintenanceInfo *out);

/*
 * A monotonically increasing count of HTTP requests this module has served.
 *
 * **This is FR-38's activity source.** The window must close after
 * `maintenance_timeout_ms` of INACTIVITY, not on a fixed deadline from entry, so
 * a user typing a WiFi password is not reaped mid-task. The events that count as
 * activity are HTTP requests, and they are produced HERE -- but the window itself
 * is `MaintenanceMode`'s, inside the host-compiled orchestrator, which this
 * device-only module cannot call. So the count is published and the poll loop
 * compares it against the last value it saw, calling
 * `NoteMaintenanceActivity` on a change.
 *
 * A counter rather than a "was there a request" flag, because the poll loop runs
 * at 100 Hz and several requests can arrive between two ticks: a flag would be
 * cleared by the first read and the later ones would go unnoticed, whereas a
 * changed count is still a change.
 *
 * Read on the poll task, written by the HTTP server's own task. A `uint32_t`
 * load/store is atomic on this target, and the exact value does not matter --
 * only whether it moved -- so no lock is needed and a torn read is impossible.
 */
uint32_t MaintenanceRadioRequestCount(void);

/*
 * Whether THIS window's radio came up: 0 on success, 1 when the last start
 * failed. The poll loop reports it so a window with no radio behind it is
 * distinguishable from a working one, instead of a mode that silently does
 * nothing.
 *
 * **It describes the CURRENT window, not a running total.** `Stop` clears it, so
 * a window whose start failed does not poison the next one: the app branches on
 * this FIRST when writing the maintenance card, so a sticky count would make
 * every later window claim its radio was down while the same frame carried a
 * working page URL.
 *
 * Read on the poll task; written only by `MaintenanceRadioStart`/`Stop`, which the
 * poll task also calls, so no atomicity is needed.
 */
uint32_t MaintenanceRadioFailures(void);

/*
 * The `config_state` word the page's `/api/status` reports, injected because this
 * module cannot reach the orchestrator (the orchestrator is host-compiled and
 * this file is not).
 *
 * Refreshed every poll tick, so the page reflects the config the device is
 * RUNNING rather than the one it booted with -- the same "the field describes the
 * config in force" rule the USB `status` frame follows.
 */
void MaintenanceRadioSetConfigState(const char *word);
