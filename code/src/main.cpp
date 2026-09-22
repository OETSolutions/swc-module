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

    for (;;) {
        SystemOrchestratorTick(sys, hal->now_ms(hal->ctx));
        // Drains one deferred router frame and whatever the TX buffer still
        // holds. A large config reply is chunked, so this must be called often
        // enough that a reply finishes in a reasonable time -- 10 ms per chunk
        // puts a maximum config out in well under a second.
        UsbLinkService();
        vTaskDelay(pdMS_TO_TICKS(SWC_POLL_MS));
    }
}
