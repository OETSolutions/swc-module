#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * now_ms/now_us are part of the HAL on purpose: every timing rule in the spec
 * (500ms double-press window, 750ms long-press threshold, 200ms key send,
 * 5-minute maintenance timeout) is a tested rule, and the only way to test a
 * timing rule without sleeping is to make the clock an input.
 *
 * dac_set_code does NOT take a power mode. Setting a channel's gain mode *is*
 * a power-mode change (spec 2.3: PD1:PD0 = 01 selects gain 1.82), and it is
 * made independently of any code write, so it is its own member.
 *
 * dac_set_code returns void deliberately (spec 6.8): the driver retries with
 * backoff internally and latches a fault on persistent failure; it never
 * drives a guessed code. There is no useful failure for a caller to branch on.
 */
typedef struct IHAL {
    int      (*adc_read_mv)(void *ctx, AdcChannel ch);
    void     (*dac_set_code)(void *ctx, DacChannel ch, uint16_t code);
    void     (*dac_power_mode)(void *ctx, DacChannel ch, DacPowerMode mode);
    void     (*dac_ldac)(void *ctx, bool assert);
    void     (*gpio_write)(void *ctx, GpioPin pin, bool level);
    bool     (*gpio_read)(void *ctx, GpioPin pin);
    void     (*buzzer_on)(void *ctx, bool on);
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*now_us)(void *ctx);
    int      (*nvs_get)(void *ctx, const char *key, void *out, size_t len);
    int      (*nvs_set)(void *ctx, const char *key, const void *in, size_t len);
    void     (*reboot)(void *ctx);
    void      *ctx;
} IHAL;

#ifdef __cplusplus
}
#endif
