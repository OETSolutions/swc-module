#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Config/ConfigModel.h"
#include "Gesture/PressClassifier.h"   // GestureTimings, GestureTimingsDefault
#include "HAL/IHAL.h"

/*
 * Host implementation of IHAL. All state is observable, so tests assert on
 * what the code *did to the hardware*, not on internal variables.
 */
class MockHal {
public:
    // A valid one-channel config and the default timings, so a test that needs
    // "a working device" writes two lines instead of thirty. The ladder is spec
    // 3.7's worked example: idle 2835 mV, `vol_up` SINGLE -> a 2400 mV output.
    //
    // A press pulls the input DOWN from idle (spec 6.3), so the button's
    // `mv_center` (1430) is BELOW `learned_idle_mv` (2835). That is not an
    // arbitrary pair -- inverting it would make the fixture physically
    // impossible and every classification test would pass for the wrong reason.
    struct Defaults {
        Config         config;
        GestureTimings timings;

        Defaults();
    };

    MockHal();

    IHAL &InterfaceRef() { return iface_; }   // every later task's tests take &hal.InterfaceRef()

    // --- clock -------------------------------------------------------------
    uint64_t NowMs() { return now_ms_; }
    uint64_t NowUs() { return now_ms_ * 1000ULL; }
    void AdvanceMs(uint64_t ms) { now_ms_ += ms; }

    // --- analog ------------------------------------------------------------
    void SetAdcMilliVolts(AdcChannel ch, int mv) { adc_mv_[static_cast<int>(ch)] = mv; }
    void ReleaseInputs();
    int AdcReadMv(AdcChannel ch) { return adc_mv_[static_cast<int>(ch)]; }

    void DacSetCode(DacChannel ch, uint16_t code);
    // NOTE: the type name must be qualified as ::DacPowerMode from here on.
    // A member function named DacPowerMode hides the enum type of the same name
    // for the remainder of class scope, so a bare DacPowerMode below would not
    // name a type (the plan's MockHAL.h as written does not compile). The
    // member *name* is fixed by the harness API, so the type is qualified.
    void DacPowerMode(DacChannel ch, ::DacPowerMode mode);
    void DacLdac(bool assert) { ldac_asserted_ = assert; }
    uint16_t LastDacCode(DacChannel ch) const;
    ::DacPowerMode LastDacPowerMode(DacChannel ch) const;
    int DacWriteCount(DacChannel ch) const;
    bool LastLdac() const { return ldac_asserted_; }

    // --- gpio --------------------------------------------------------------
    void GpioWrite(GpioPin pin, bool level);
    bool GpioRead(GpioPin pin) const;
    void SetGpioInput(GpioPin pin, bool level) { gpio_in_[static_cast<int>(pin)] = level; }
    int GpioWriteCount(GpioPin pin) const;

    // --- buzzer ------------------------------------------------------------
    // The buzzer is active at a fixed ~2.4 kHz with no pitch control (spec
    // 5.5), so it is a plain on/off line, not a GPIO. Level is what tests
    // assert on; BuzzerOnCount counts drive calls, which is what makes
    // "idle ticks must be silent" checkable.
    void BuzzerOn(bool on) { buzzer_on_ = on; ++buzzer_calls_; }
    bool BuzzerIsOn() const { return buzzer_on_; }
    int BuzzerOnCount() const { return buzzer_calls_; }

    // --- nvs ---------------------------------------------------------------
    int NvsSet(const char *key, const void *in, size_t len);
    int NvsGet(const char *key, void *out, size_t len);
    void FailNextNvsWrite() { fail_next_nvs_write_ = true; }
    // Simulate power loss partway through the next write: n bytes land, the
    // write reports failure, and the stored blob is short. Readers must catch
    // that via length + CRC, never by assuming the write completed.
    void TruncateNextNvsWriteAt(size_t bytes) {
        truncate_set_ = true;
        truncate_next_write_at_ = bytes;
    }
    // Truncate the next write TO A SPECIFIC KEY. TruncateNextNvsWriteAt targets
    // "the next write", which with chunked slots is always a payload chunk -- so
    // it cannot express a torn *sequence* write, where every payload chunk landed
    // and only the final key was lost. That is the other half of the A/B tear and
    // the case the write-order protocol exists for, so it needs its own injector.
    void TruncateNvsWriteTo(const char *key, size_t bytes) {
        truncate_key_ = key;
        truncate_key_at_ = bytes;
    }
    // Flip one bit at `offset` in a stored blob, to prove CRC catches it.
    void CorruptNvsValue(const char *key, size_t offset);
    void ClearNvs() { nvs_.clear(); }
    // Remove ONE key, as an interrupted erase or a partially-written chunk set
    // leaves. Distinct from ClearNvs: the test needs the header chunk intact
    // while a later chunk is missing, which is the case that catches a store
    // trusting the header's length over the chunks it actually read.
    void ClearNvsKey(const char *key) { nvs_.erase(key); }
    int RebootCount() const { return reboot_count_; }

    // Advance the clock and hand it to the interface (for poll loops).
    void Tick(uint64_t ms) { AdvanceMs(ms); }

private:
    static int  AdcReadMvThunk(void *ctx, AdcChannel ch);
    static void DacSetCodeThunk(void *ctx, DacChannel ch, uint16_t code);
    static void DacPowerModeThunk(void *ctx, DacChannel ch, ::DacPowerMode m);
    static void DacLdacThunk(void *ctx, bool assert);
    static void GpioWriteThunk(void *ctx, GpioPin pin, bool level);
    static bool GpioReadThunk(void *ctx, GpioPin pin);
    static void BuzzerOnThunk(void *ctx, bool on);
    static uint64_t NowMsThunk(void *ctx);
    static uint64_t NowUsThunk(void *ctx);
    static int  NvsGetThunk(void *ctx, const char *key, void *out, size_t len);
    static int  NvsSetThunk(void *ctx, const char *key, const void *in, size_t len);
    static void RebootThunk(void *ctx);

    IHAL iface_{};
    uint64_t now_ms_ = 0;
    // Seeded by the constructor's ReleaseInputs(), NOT left at zero. Zero is a
    // MEANINGFUL reading here: the AUX inputs are active-low (pulled to the rail,
    // shorted to ground when pressed), so a zeroed AUX1 reads as "held". Every
    // test that ticked past LearnWizard::kEnterHoldMs (1.5 s) without setting AUX1
    // was therefore entering the learn wizard by accident, and a test that passed
    // was passing for a reason unrelated to what it was named for.
    int adc_mv_[ADC_CH_COUNT] = {};
    uint16_t dac_code_[DAC_CH_COUNT] = {};
    ::DacPowerMode dac_mode_[DAC_CH_COUNT] = {};
    int dac_writes_[DAC_CH_COUNT] = {};
    bool ldac_asserted_ = false;
    bool gpio_out_[GPIO_COUNT] = {};
    bool gpio_in_[GPIO_COUNT] = {};
    int gpio_writes_[GPIO_COUNT] = {};
    bool buzzer_on_ = false;
    int buzzer_calls_ = 0;
    std::map<std::string, std::vector<uint8_t>> nvs_;
    bool fail_next_nvs_write_ = false;
    bool truncate_set_ = false;
    size_t truncate_next_write_at_ = 0;
    std::string truncate_key_;          // empty = no key-targeted truncation armed
    size_t truncate_key_at_ = 0;
    int reboot_count_ = 0;
};

/*
 * The one valid default config, as a free function so tests that need a WHOLE
 * config (rather than a MockHal) can get one without constructing a device.
 *
 * It returns the same config `MockHal::Defaults` builds -- that constructor now
 * calls this, so the two cannot drift. Task 15 needs a legal multi-chunk config
 * to exercise the chunked transport, and hand-writing a second one in the test
 * file is how a test ends up validating a config the device never sees.
 */
Config MockHalDefaultsConfig();
