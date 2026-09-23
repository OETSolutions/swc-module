#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "HAL/EspHal.h"
#include "Link/UsbLink.h"
#include "Maintenance/MaintenanceRadio.h"
#include "System/SystemOrchestrator.h"

static const char *TAG = "swc-boot";

// The poll cadence. 10 ms is what the gesture tests assume: LONG must fire
// within one tick of its 750 ms threshold (spec 11's FR-10 test), and the
// double-press boundary is asserted at 499/500 ms, so a coarser poll would make
// those thresholds unreachable.
#define SWC_POLL_MS 10

// Board identity is asserted BEFORE anything else runs, because every budget and
// every pin in this firmware assumes it. A wrong board fails loudly here instead
// of as a mysterious analog fault later.
static bool VerifyBoard(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

    ESP_LOGI(TAG, "chip=%s cores=%d rev=v%d.%d", CONFIG_IDF_TARGET, chip.cores,
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "flash=%" PRIu32 " bytes idf=%s", flash_size, esp_get_idf_version());

    if (flash_size != 4 * 1024 * 1024) {
        ESP_LOGE(TAG, "flash is %" PRIu32 " bytes, build assumes %d", flash_size,
                 4 * 1024 * 1024);
        return false;
    }
    if (chip.cores != 2) {
        ESP_LOGE(TAG, "expected 2 cores for ESP32-S3, got %d", chip.cores);
        return false;
    }
    return true;
}

// IDF's startup (freertos/app_startup.c) is C and calls `app_main` by that exact
// symbol, so it must have C linkage. Without this guard the C++ definition
// mangles to `_Z8app_mainv` and the link fails with
// "undefined reference to `app_main`".
extern "C" void app_main(void)
{
#ifdef SWC_BENCH_PANIC_IMAGE
    /*
     * A deliberately BROKEN image, for spec §10.4's level-4 rollback test: "flash
     * a deliberately-faulting image (compiles, panics at startup), assert the
     * bootloader rolls back and the device comes up on the old image. **This must
     * be tested with a genuinely broken image**, not a mocked failure, or it
     * proves nothing."
     *
     * The panic is FIRST, before any HAL or orchestrator work, so the image can
     * never reach `esp_ota_mark_app_valid_cancel_rollback` -- which is the whole
     * point: an image that panics after confirming itself would test nothing. It
     * is guarded by a compile-time define that no shipping env sets, so the
     * product image cannot contain this code path; `tools/bench_rollback.py`
     * builds it with `-D SWC_BENCH_PANIC_IMAGE`.
     *
     * A NULL dereference rather than `abort()`: `abort()` is a straight
     * exception/panic that CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT also reboots, but
     * a real crash is the more faithful "faulting image", and it also proves the
     * panic HANDLER (not just an explicit call) leads to the rollback path.
     */
    volatile int *broken = (volatile int *)0;
    *broken = 1;
#endif
    if (!VerifyBoard()) {
        // A board that does not match the build must not drive the KEY line at
        // all. Halt rather than loop: a wrong board is a bench condition.
        abort();
    }

    IHAL *hal = EspHalInit();
    if (hal == NULL) {
        // The DAC or ADC did not come up, so the output cannot be reached or its
        // state cannot be read. Do NOT proceed to serve presses (spec 6.8: never
        // drive a guessed code). Reboot after a delay so a transient bus fault
        // recovers, which is what the maintainer will expect at the bench.
        ESP_LOGE(TAG, "HAL init failed; not serving output. Rebooting in 5s.");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    if (EspHalCalibrationIsDegraded()) {
        // Reported, not silent (spec 3.2). This console log names the cause at
        // init; the orchestrator ALSO plays BOOT_DEGRADED for this class of
        // condition, and the flag below is how it learns -- the buzzer is the
        // signal a user at the bench hears with no host attached. It is passed
        // rather than read inside the orchestrator because the orchestrator's
        // translation unit is host-compiled and cannot name EspHal.
        ESP_LOGW(TAG, "ADC calibration degraded: linear approximation in use");
    }

    // Create establishes the safe idle output before returning (FR-13), which is
    // why the link is started only after this call and never before it.
    SystemOrchestrator *sys = SystemOrchestratorCreate(hal, EspHalCalibrationIsDegraded());
    if (sys == NULL) {
        ESP_LOGE(TAG, "orchestrator alloc failed; cannot reach safe idle. Rebooting.");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    if (!SystemOrchestratorOutputVerified(sys)) {
        ESP_LOGE(TAG, "output NOT verified -- safe idle unestablished or a DAC write failed");
        // Deliberately NOT marked valid: see the mark-valid call below.
    }

    // FR-37: mark valid only once the device has PROVEN it can do its job --
    // the safe idle is established AND the output is actually reachable. Marking
    // this at the top of app_main would confirm an image that boots but cannot
    // drive the DAC, stranding the user with a bricked-but-"valid" device and no
    // rollback.
    //
    // **The condition must be able to be FALSE, and that is the whole point.**
    // It used to read `SystemOrchestratorSafeIdle()`, which returns
    // `safe_idle_established_` -- a flag assigned `true` once, at the end of an
    // `EstablishSafeIdle()` that returns void and cannot fail. The gate was a
    // compile-time constant `true` on the device path, so spec §9.8's "an image
    // that boots but cannot drive the DAC is not healthy" was exactly the case it
    // could not detect: a dead I2C bus still cancelled the pending rollback.
    // `SystemOrchestratorOutputVerified` folds in the HAL's latched
    // `dac_faulted`, which a failed `i2c_master_transmit` sets and nothing
    // clears -- that is the signal the gate needed to be able to say NO.
    //
    // `esp_ota_mark_app_valid_cancel_rollback` is a no-op when the running image
    // was not started from a pending-verify state (the normal case after a
    // successful boot), so calling it unconditionally on the good path is correct.
    if (SystemOrchestratorOutputVerified(sys)) {
        const esp_err_t mark = esp_ota_mark_app_valid_cancel_rollback();
        if (mark != ESP_OK) {
            // Not fatal: the device works, it just will not be treated as
            // confirmed if this boot came from an OTA. Say so rather than hide it.
            ESP_LOGW(TAG, "could not mark the image valid: %s", esp_err_to_name(mark));
        } else {
            ESP_LOGI(TAG, "image confirmed valid; rollback cancelled");
        }
    }

    // The USB link belongs here and not earlier: everything above has already
    // made the output safe, so the device serves presses with no app, no host and
    // no radio attached. That is what makes FR-42 structural rather than a
    // promise -- there is no ordering in which the link is required to boot.
    UsbLinkStart(hal, sys);

    /*
     * FR-32/FR-34: the maintenance radio is driven by the WINDOW's transitions,
     * here in the poll loop rather than inside `MaintenanceMode`.
     *
     * `MaintenanceMode` is pure state and is host-tested; the moment it named
     * NimBLE it would lose those tests. Spec 8.2 says the radio work "is driven by
     * the caller in response to `Active()` transitions", and this loop is that
     * caller. `last_maintenance` is the applied state, so the bring-up and the
     * teardown each run exactly once per transition rather than on every tick.
     *
     * **A start that FAILED is latched, not retried every tick.** A failed
     * bring-up means the radio could not come up (no heap, no MAC, a driver
     * error); retrying at 100 Hz would fill the log and burn heap while telling
     * the user nothing new. The failure count is published to the app instead, so
     * a user sees "the window is open and the radio is not up" rather than a mode
     * that silently does nothing.
     */
    bool last_maintenance = false;
    uint32_t last_requests = 0;
    uint32_t published_failures = 0;
    MaintenanceInfo last_info{};
    bool have_published = false;

    for (;;) {
        SystemOrchestratorTick(sys, hal->now_ms(hal->ctx));

        const bool maintenance_now = SystemOrchestratorIsMaintenanceActive(sys);
        if (maintenance_now != last_maintenance) {
            last_maintenance = maintenance_now;
            if (maintenance_now) {
                MaintenanceInfo info{};
                if (MaintenanceRadioStart(&info)) {
                    ESP_LOGI(TAG, "maintenance window open; radio up, page at %s", info.page_url);
                } else {
                    // Reported, not hidden: the window is still open (the
                    // orchestrator's state is its own) and the app is told the
                    // radio did not come up.
                    ESP_LOGE(TAG, "maintenance window open but the radio failed to start");
                }
            } else {
                // FR-32: the stacks are freed on the way out, so nothing radio-
                // shaped is resident while the device serves the wheel.
                MaintenanceRadioStop();
                ESP_LOGI(TAG, "maintenance window closed; radio torn down");
            }
        }

        // Keep the page's `config_state` current with the config the device is
        // RUNNING, so a commit during the window is reflected rather than frozen
        // at the boot value.
        if (maintenance_now) {
            MaintenanceRadioSetConfigState(SystemOrchestratorConfigStateWord(sys));
        }

        /*
         * FR-38's activity clock. A request served by the maintenance HTTP server
         * IS activity -- a user reading the status page or typing a WiFi password
         * -- so the window is bumped whenever the request count has MOVED since the
         * last tick. Without this the window would be a fixed deadline from entry
         * and would reap a user mid-provision, which is the gap N-35 recorded while
         * there was no request source to bump it from.
         *
         * A count compared against the last value, not a flag: the server's task
         * runs concurrently with this loop, so several requests can land between
         * two ticks and a flag would report only the first.
         */
        const uint32_t requests = MaintenanceRadioRequestCount();
        if (maintenance_now && requests != last_requests) {
            SystemOrchestratorNoteMaintenanceActivity(sys, hal->now_ms(hal->ctx));
        }
        last_requests = requests;

        // Publish the window's facts to the app (spec 8.3 option 1). Every tick,
        // so a connection that arrives mid-window is handled by the router's own
        // change detection; the values are only re-sent when something moved.
        MaintenanceInfo info{};
        info.active = maintenance_now;
        if (maintenance_now) MaintenanceRadioDescribe(&info);
        const uint32_t failures = MaintenanceRadioFailures();
        if (!have_published || info.active != last_info.active ||
            strcmp(info.pop, last_info.pop) != 0 || strcmp(info.token, last_info.token) != 0 ||
            failures != published_failures) {
            UsbLinkPublishMaintenance(info, failures);
            last_info = info;
            published_failures = failures;
            have_published = true;
        }

        // Drains one deferred router frame and whatever the TX buffer still
        // holds. A large config reply is chunked, so this must be called often
        // enough that a reply finishes in a reasonable time -- 10 ms per chunk
        // puts a maximum config out in well under a second.
        UsbLinkService();
        vTaskDelay(pdMS_TO_TICKS(SWC_POLL_MS));
    }
}
