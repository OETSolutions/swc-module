#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Spec 3.2's value types, declared here because this header is the frozen C
 * contract every later task consumes. An earlier revision omitted all three --
 * they were declared in the Shared contract but not here, so LadderDecode.h had
 * to carry a local `using MilliVolt = uint16_t;` to compile. One declaration,
 * in the frozen header, is the fix.
 */
typedef uint16_t MilliVolt;    /* 0-2900 at the pin, after calibration (spec 3.2) */
typedef uint16_t AdcRaw;       /* 0-4095, 12-bit at 12 dB atten (spec 3.2) */
typedef uint32_t TimestampMs;  /* monotonic ms since boot (spec 3.2) */

typedef enum {
    ADC_CH_SWC1 = 0, ADC_CH_SWC2, ADC_CH_TEMP,
    ADC_CH_AUX1, ADC_CH_AUX2, ADC_CH_AUX3,
    /* KEY-line sense, KEY voltage / 2 (spec 2.2). Analog inputs driving the
     * servo trim loop, so they are ADC channels and NOT GpioPins. */
    ADC_CH_KEY_SENSE1, ADC_CH_KEY_SENSE2,
    ADC_CH_COUNT
} AdcChannel;

typedef enum {
    /* Per channel: one signal DAC output, one V_ADJ output. Verified against
     * the netlist -- U4.VOUTA -> ch1 signal, VOUTB -> /V_ADJ1, VOUTC -> ch2
     * signal, VOUTD -> /V_ADJ2. There is no spare channel. */
    DAC_CH_KEY1 = 0, DAC_CH_ADJ1, DAC_CH_KEY2, DAC_CH_ADJ2,
    DAC_CH_COUNT
} DacChannel;

/* The MCP4728 has no high-impedance state; these are its power-down modes. */
typedef enum {
    DAC_POWER_NORMAL = 0,
    DAC_POWER_GND_1K,
    DAC_POWER_GND_100K,
    DAC_POWER_GND_500K
} DacPowerMode;

typedef enum {
    GPIO_LED_STAT = 0, GPIO_LED2,
    /* The only two GPIO inputs. SENSE1/SENSE2 are ADC channels above. The
     * buzzer and ~LDAC are NOT here -- they are the semantic members
     * buzzer_on and dac_ldac, which is where the rhythm grammar and the DAC
     * sequencing contract live. */
    GPIO_BOOT, GPIO_VBUS_VALID,
    GPIO_COUNT
} GpioPin;

/*
 * The only interface between logic and silicon. Every module above lib/HAL
 * takes an IHAL* so it can be exercised on the host with MockHal.
 *
 * nvs_get / nvs_set return contracts, stated HERE because they are the one part
 * of this interface every implementation must agree on exactly and no host test
 * can check EspHal against them:
 *   - nvs_set returns **0 on success, nonzero on failure** (mock returns 0;
 *     `ConfigStore` tests `!= 0`). EspHal returned `len` on success once, which
 *     made every device write read as a failure.
 *   - nvs_get returns the **number of bytes read on success, -1 if the key is
 *     absent OR the caller's buffer is smaller than the stored value**. It never
 *     truncates: IDF's nvs_get_blob rejects an undersized buffer with
 *     ESP_ERR_NVS_INVALID_LENGTH rather than shortening it.
 *
 * now_ms/now_us are part of the HAL on purpose: every timing rule in the spec
 * (500ms double-press window, 750ms long-press threshold, 200ms key send,
 * 5-minute maintenance timeout) is a tested rule, and the only way to test a
 * timing rule without sleeping is to make the clock an input.
 *
 * dac_set_code does NOT take a power mode. Setting a channel's gain mode *is*
 * a power-mode change (spec 2.3: PD1:PD0 = 01 selects gain 1.82), and it is
 * made independently of any code write, so it is its own member.
 *
 * dac_set_code returns void. **It DOES retry** (spec 6.8's "retry with backoff";
 * the policy is `DacRetry.h`: 3 attempts, 1 ms then 2 ms), and a write that fails
 * every attempt latches the fault that `dac_faulted` reports. What is also
 * honoured is the row's last clause: a failed write drives nothing rather than a
 * guessed code, because the frame is never partially applied.
 *
 * **`dac_faulted` exists because a void write is not a checkable one, and FR-37
 * needs a checkable one.** The rollback health-gate must confirm the device "can
 * drive the DAC" before cancelling a pending rollback; with a void write there is
 * no signal to branch on, so the gate in `app_main` evaluated a condition that
 * was constant-true (see open item N-21's sibling, and `OutputVerified`).
 * `dac_faulted` LATCHES: true once ANY DAC write or read-back has failed since
 * boot, and it is never cleared, because spec 7.3's reboot-only rule applies to a
 * hardware condition that does not fix itself. It is an accessor rather than a
 * return value so the existing void-write call sites do not all have to change.
 *
 * dac_read_code implements FR-13's step 3b, "VERIFY the DAC is in the safe state
 * (read back)": it issues the MCP4728 Read Command and decodes one output's input
 * register. Returns false for a bus failure (which also latches `dac_faulted`) or
 * for a channel that is not a real output. A successful read never CLEARS the
 * latch -- spec 7.3's rule is reboot-only.
 */
typedef struct IHAL {
    int      (*adc_read_mv)(void *ctx, AdcChannel ch);
    void     (*dac_set_code)(void *ctx, DacChannel ch, uint16_t code);
    void     (*dac_power_mode)(void *ctx, DacChannel ch, DacPowerMode mode);
    void     (*dac_ldac)(void *ctx, bool assert);
    // Read back a channel's code from the MCP4728's input register (FR-13).
    bool     (*dac_read_code)(void *ctx, DacChannel ch, uint16_t *out);
    // True once any DAC write has failed since boot. Never cleared. The one
    // falsifiable signal that the output is actually reachable (FR-13/FR-37).
    bool     (*dac_faulted)(void *ctx);
    void     (*gpio_write)(void *ctx, GpioPin pin, bool level);
    bool     (*gpio_read)(void *ctx, GpioPin pin);
    void     (*buzzer_on)(void *ctx, bool on);
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*now_us)(void *ctx);
    // Free heap in bytes, for spec 4.3's `status.heap_free` (open item N-22). A
    // function rather than a field because the value is read at emit time and a
    // stored copy would be a snapshot that ages between frames. Returns 0 when
    // the platform cannot answer, which the status body reports as 0 -- an
    // honest "unknown" for a diagnostic field, never a fabricated size.
    uint32_t (*heap_free)(void *ctx);
    int      (*nvs_get)(void *ctx, const char *key, void *out, size_t len);
    int      (*nvs_set)(void *ctx, const char *key, const void *in, size_t len);
    // Restart into the application (spec 4.3's `boot_target: "app"`).
    void     (*reboot)(void *ctx);
    // Restart into the ROM USB download bootloader (spec 4.3's
    // `boot_target: "bootloader"`), so a peer can flash the device without a
    // physical BOOT press. On the ESP32-S3 this is not a power-on-only event as
    // spec 3.2 once assumed: the ROM checks `RTC_CNTL_FORCE_DOWNLOAD_BOOT`
    // (`RTC_CNTL_OPTION1_REG` bit 0) on every reset, that bit lives in the RTC
    // domain so it survives `esp_restart()` but not a power cycle, and IDF's own
    // `esp_usb_console_before_restart` sets exactly this bit for its
    // `REBOOT_BOOTLOADER`. Separate from `reboot` rather than a parameter because
    // the two are different hardware actions, and a caller that wanted the app and
    // silently got the download stub is the "wrong destination, reported as
    // success" defect the frame's `bad_target` refusal was written to avoid.
    void     (*reboot_to_download)(void *ctx);
    void      *ctx;
} IHAL;

#ifdef __cplusplus
}
#endif
