#include "MockHAL.h"

Config MockHalDefaultsConfig() {
    Config config{};
    config.schema_version = kConfigSchemaVersion;
    std::strncpy(config.device_id, "SWC-0001", sizeof(config.device_id) - 1);
    config.settings.timings = GestureTimingsDefault();
    config.settings.gain_policy = GainPolicy::kAuto;
    config.settings.buzzer_level = 2;
    config.settings.led_level = 2;
    config.settings.maintenance_timeout_ms = 300000;
    config.channel_count = 1;

    ChannelConfig &ch = config.channels[0];
    ch.enabled = true;
    std::strncpy(ch.name, "SWC1", sizeof(ch.name) - 1);
    // Spec 3.7's worked example. `learned_idle_mv` is the normalization
    // reference, so the button levels are the values measured AT that idle.
    ch.ladder.learned_idle_mv = 2835;
    ch.ladder.count = 3;
    ch.ladder.buttons[0] = {"vol_up", "Volume Up",   1430, 120, 3300, 235, 200, 98};
    ch.ladder.buttons[1] = {"vol_dn", "Volume Down", 1785, 120, 3300, 235, 200, 97};
    ch.ladder.buttons[2] = {"next",   "Next Track",  2145, 110, 3300, 235, 200, 99};
    ch.output.gain_mode = GainMode::kAmplified;
    ch.output.idle_dac_code = 4095;   // full scale is the safe state (spec 6.7)

    // vol_up SINGLE drives the output; its LONG releases. `next` DOUBLE is the
    // app's extra function, which is the product's core case (spec 3.5/3.6).
    config.binding_count = 3;
    Binding &b1 = config.bindings[0];
    std::strncpy(b1.id, "b1", sizeof(b1.id) - 1);
    b1.channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(b1.button, "vol_up", sizeof(b1.button) - 1);
    b1.gesture = Gesture::kSingle;
    b1.enabled = true;
    b1.action_count = 1;
    b1.actions[0].kind = ActionKind::kOutVoltage;
    b1.actions[0].key_mv = 2400;   // spec 3.7's b1

    Binding &b2 = config.bindings[1];
    std::strncpy(b2.id, "b2", sizeof(b2.id) - 1);
    b2.channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(b2.button, "vol_up", sizeof(b2.button) - 1);
    b2.gesture = Gesture::kLong;
    b2.enabled = true;
    b2.action_count = 1;
    b2.actions[0].kind = ActionKind::kOutRelease;

    Binding &b3 = config.bindings[2];
    std::strncpy(b3.id, "b3", sizeof(b3.id) - 1);
    b3.channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(b3.button, "next", sizeof(b3.button) - 1);
    b3.gesture = Gesture::kDouble;
    b3.enabled = true;
    b3.action_count = 1;
    b3.actions[0].kind = ActionKind::kAppIntent;
    std::strncpy(b3.actions[0].target, "com.oetsolutions.swc.ACTION_NAVIGATE",
                 sizeof(b3.actions[0].target) - 1);
    return config;
}

MockHal::Defaults::Defaults()
    : config(MockHalDefaultsConfig()), timings(GestureTimingsDefault()) {}

MockHal::MockHal() {
    ReleaseInputs();
    iface_.adc_read_mv    = &MockHal::AdcReadMvThunk;
    iface_.dac_set_code   = &MockHal::DacSetCodeThunk;
    iface_.dac_power_mode = &MockHal::DacPowerModeThunk;
    iface_.dac_ldac       = &MockHal::DacLdacThunk;
    iface_.dac_read_code  = &MockHal::DacReadCodeThunk;
    iface_.dac_faulted    = &MockHal::DacFaultedThunk;
    iface_.gpio_write     = &MockHal::GpioWriteThunk;
    iface_.gpio_read      = &MockHal::GpioReadThunk;
    iface_.buzzer_on      = &MockHal::BuzzerOnThunk;
    iface_.now_ms         = &MockHal::NowMsThunk;
    iface_.now_us         = &MockHal::NowUsThunk;
    iface_.heap_free      = &MockHal::HeapFreeThunk;
    iface_.reset_reason   = &MockHal::ResetReasonThunk;
    iface_.calibration_degraded = &MockHal::CalibrationDegradedThunk;
    iface_.nvs_get        = &MockHal::NvsGetThunk;
    iface_.nvs_set        = &MockHal::NvsSetThunk;
    iface_.reboot         = &MockHal::RebootThunk;
    iface_.reboot_to_download = &MockHal::RebootToDownloadThunk;
    iface_.ctx            = this;
}

/*
 * Put every input the orchestrator reads into its RELEASED state.
 *
 * The defaults matter because zero is not neutral: AUX1-AUX3 are active-low and
 * 0 mV is a valid "held" reading, so a zeroed mock makes the device think a
 * programming button is being held down. Tests that ran longer than the learn
 * wizard's 1.5 s entry hold silently entered a learn run, which is exactly the
 * kind of accidental state that makes a green test mean nothing.
 *
 * The ladders idle at their fixture's `learned_idle_mv` (2835), the KEY senses sit
 * above the output envelope's floor (so the head unit is "present"), and TEMP is
 * left at 0 because no path consults it yet (the NTC conversion is a bring-up
 * deliverable, spec 6.4).
 */
void MockHal::ReleaseInputs() {
    adc_mv_[ADC_CH_SWC1] = 2835;
    adc_mv_[ADC_CH_SWC2] = 2835;
    adc_mv_[ADC_CH_AUX1] = 3300;
    adc_mv_[ADC_CH_AUX2] = 3300;
    adc_mv_[ADC_CH_AUX3] = 3300;
    adc_mv_[ADC_CH_KEY_SENSE1] = 2490;
    adc_mv_[ADC_CH_KEY_SENSE2] = 2490;
}

void MockHal::DacSetCode(DacChannel ch, uint16_t code) {
    ++dac_writes_[static_cast<int>(ch)];
    if (fail_next_dac_write_) {
        // The write FAILED, so it landed nothing -- mirroring EspHal, where a
        // failed `i2c_master_transmit` means the channel is not updated. Leaving
        // the previous code in place (rather than storing `code`) is what lets a
        // test distinguish "attempted" from "reached the part".
        fail_next_dac_write_ = false;
        dac_failed_ = true;
        return;
    }
    dac_code_[static_cast<int>(ch)] = code;
}

void MockHal::DacPowerMode(DacChannel ch, ::DacPowerMode mode) {
    dac_mode_[static_cast<int>(ch)] = mode;
}

bool MockHal::DacReadCode(DacChannel ch, uint16_t *out) {
    ++dac_reads_[static_cast<int>(ch)];
    if (fail_dac_read_) {
        // A failed read latches the same fault as a failed write (the bus is not
        // answering), and reports nothing -- mirroring EspHal, where a failed
        // `i2c_master_receive` leaves `out` untouched.
        fail_dac_read_ = false;
        dac_failed_ = true;
        return false;
    }
    if (out == nullptr) return false;
    // The READ-BACK MISMATCH hook. Without it a test can only exercise "the read
    // failed"; this lets one exercise the other half of FR-13 -- a bus that
    // answers, with a code that is NOT what was written. `read_back_bias_` is
    // added to the stored code, so a test that wants a mismatch arms a nonzero
    // bias without needing a second DAC model.
    const uint16_t stored = dac_code_[static_cast<int>(ch)];
    const int biased = static_cast<int>(stored) + read_back_bias_;
    *out = static_cast<uint16_t>(biased < 0 ? 0 : (biased > 4095 ? 4095 : biased));
    return true;
}

void MockHal::CorruptNvsValue(const char *key, size_t offset) {
    auto it = nvs_.find(key);
    if (it == nvs_.end() || offset >= it->second.size()) return;
    it->second[offset] ^= 0x01;
}

uint16_t MockHal::LastDacCode(DacChannel ch) const { return dac_code_[static_cast<int>(ch)]; }
::DacPowerMode MockHal::LastDacPowerMode(DacChannel ch) const { return dac_mode_[static_cast<int>(ch)]; }
int MockHal::DacWriteCount(DacChannel ch) const { return dac_writes_[static_cast<int>(ch)]; }

void MockHal::GpioWrite(GpioPin pin, bool level) {
    const int i = static_cast<int>(pin);
    gpio_out_[i] = level;
    ++gpio_writes_[i];
}

bool MockHal::GpioRead(GpioPin pin) const {
    const int i = static_cast<int>(pin);
    // Outputs read back what was written (the LEDs); the two digital inputs read
    // their programmed input state. Nothing else is a GpioPin -- SENSE1/SENSE2
    // are ADC channels (AdcReadMv), and the buzzer and ~LDAC are the semantic
    // members BuzzerIsOn() and LastLdac().
    switch (pin) {
        case GPIO_BOOT: case GPIO_VBUS_VALID:
            return gpio_in_[i];
        default:
            return gpio_out_[i];
    }
}

int MockHal::GpioWriteCount(GpioPin pin) const { return gpio_writes_[static_cast<int>(pin)]; }

int MockHal::NvsSet(const char *key, const void *in, size_t len) {
    if (fail_next_nvs_write_) {
        fail_next_nvs_write_ = false;
        return -1;
    }
    const uint8_t *p = static_cast<const uint8_t *>(in);
    if (truncate_set_) {
        const size_t store = truncate_next_write_at_ < len ? truncate_next_write_at_ : len;
        nvs_[key].assign(p, p + store);
        truncate_set_ = false;
        truncate_next_write_at_ = 0;
        return -1;
    }
    // Key-targeted failure, armed by TruncateNvsWriteTo. Checked after the
    // next-write form so the two injectors stay independent, and one-shot so a
    // later write of the same key lands whole.
    //
    // A write that fails to COMMIT leaves the previous value in place: NVS is
    // copy-on-write, so an interrupted write never destroys the old entry. This
    // is not a detail -- it is what makes the A/B protocol work. If a torn
    // cfg_seq clobbered the old sequence, the store could not tell which slot is
    // authoritative and would have to fall back to defaults, losing a perfectly
    // good config. So `bytes` applies only when the key is NEW and there is
    // nothing to preserve; for an existing key the old value survives and only
    // the failure is reported.
    if (!truncate_key_.empty() && truncate_key_ == key) {
        if (nvs_.find(key) == nvs_.end()) {
            const size_t store = truncate_key_at_ < len ? truncate_key_at_ : len;
            nvs_[key].assign(p, p + store);
        }
        truncate_key_.clear();
        truncate_key_at_ = 0;
        return -1;
    }
    nvs_[key].assign(p, p + len);
    return 0;
}

int MockHal::NvsGet(const char *key, void *out, size_t len) {
    auto it = nvs_.find(key);
    if (it == nvs_.end()) return -1;
    // STRICT, like IDF. nvs_get_blob returns ESP_ERR_NVS_INVALID_LENGTH when the
    // caller's buffer is smaller than the stored value -- it does NOT truncate --
    // and EspHal::HalNvsGet maps that to -1. This used to truncate and return the
    // short length, which hid a store that read a 2048-byte chunk into a 16-byte
    // buffer: the config loaded on the host and never on the device.
    if (it->second.size() > len) return -1;
    std::memcpy(out, it->second.data(), it->second.size());
    return static_cast<int>(it->second.size());
}

int  MockHal::AdcReadMvThunk(void *ctx, AdcChannel ch) {
    return static_cast<MockHal *>(ctx)->AdcReadMv(ch);
}
void MockHal::DacSetCodeThunk(void *ctx, DacChannel ch, uint16_t code) {
    static_cast<MockHal *>(ctx)->DacSetCode(ch, code);
}
void MockHal::DacPowerModeThunk(void *ctx, DacChannel ch, ::DacPowerMode m) {
    static_cast<MockHal *>(ctx)->DacPowerMode(ch, m);
}
void MockHal::DacLdacThunk(void *ctx, bool assert) {
    static_cast<MockHal *>(ctx)->ldac_asserted_ = assert;
}
bool MockHal::DacReadCodeThunk(void *ctx, DacChannel ch, uint16_t *out) {
    return static_cast<MockHal *>(ctx)->DacReadCode(ch, out);
}
bool MockHal::DacFaultedThunk(void *ctx) {
    return static_cast<MockHal *>(ctx)->dac_failed_;
}
void MockHal::BuzzerOnThunk(void *ctx, bool on) {
    static_cast<MockHal *>(ctx)->BuzzerOn(on);
}
void MockHal::GpioWriteThunk(void *ctx, GpioPin pin, bool level) {
    static_cast<MockHal *>(ctx)->GpioWrite(pin, level);
}
bool MockHal::GpioReadThunk(void *ctx, GpioPin pin) {
    return static_cast<MockHal *>(ctx)->GpioRead(pin);
}
uint64_t MockHal::NowMsThunk(void *ctx) { return static_cast<MockHal *>(ctx)->NowMs(); }
uint64_t MockHal::NowUsThunk(void *ctx) { return static_cast<MockHal *>(ctx)->NowUs(); }
uint32_t MockHal::HeapFreeThunk(void *ctx) { return static_cast<MockHal *>(ctx)->HeapFree(); }
int MockHal::ResetReasonThunk(void *ctx) { return static_cast<MockHal *>(ctx)->ResetReason(); }
bool MockHal::CalibrationDegradedThunk(void *ctx) { return static_cast<MockHal *>(ctx)->CalibrationDegraded(); }
int MockHal::NvsGetThunk(void *ctx, const char *key, void *out, size_t len) {
    return static_cast<MockHal *>(ctx)->NvsGet(key, out, len);
}
int MockHal::NvsSetThunk(void *ctx, const char *key, const void *in, size_t len) {
    return static_cast<MockHal *>(ctx)->NvsSet(key, in, len);
}
void MockHal::RebootThunk(void *ctx) { ++static_cast<MockHal *>(ctx)->reboot_count_; }
void MockHal::RebootToDownloadThunk(void *ctx) {
    ++static_cast<MockHal *>(ctx)->reboot_to_download_count_;
}
