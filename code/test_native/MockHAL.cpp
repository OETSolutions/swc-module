#include "MockHAL.h"

MockHal::MockHal() {
    iface_.adc_read_mv    = &MockHal::AdcReadMvThunk;
    iface_.dac_set_code   = &MockHal::DacSetCodeThunk;
    iface_.dac_power_mode = &MockHal::DacPowerModeThunk;
    iface_.dac_ldac       = &MockHal::DacLdacThunk;
    iface_.gpio_write     = &MockHal::GpioWriteThunk;
    iface_.gpio_read      = &MockHal::GpioReadThunk;
    iface_.buzzer_on      = &MockHal::BuzzerOnThunk;
    iface_.now_ms         = &MockHal::NowMsThunk;
    iface_.now_us         = &MockHal::NowUsThunk;
    iface_.nvs_get        = &MockHal::NvsGetThunk;
    iface_.nvs_set        = &MockHal::NvsSetThunk;
    iface_.reboot         = &MockHal::RebootThunk;
    iface_.ctx            = this;
}

void MockHal::DacSetCode(DacChannel ch, uint16_t code) {
    const int i = static_cast<int>(ch);
    dac_code_[i] = code;
    ++dac_writes_[i];
}

void MockHal::DacPowerMode(DacChannel ch, ::DacPowerMode mode) {
    dac_mode_[static_cast<int>(ch)] = mode;
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
    const size_t n = it->second.size() < len ? it->second.size() : len;
    std::memcpy(out, it->second.data(), n);
    return static_cast<int>(n);
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
int MockHal::NvsGetThunk(void *ctx, const char *key, void *out, size_t len) {
    return static_cast<MockHal *>(ctx)->NvsGet(key, out, len);
}
int MockHal::NvsSetThunk(void *ctx, const char *key, const void *in, size_t len) {
    return static_cast<MockHal *>(ctx)->NvsSet(key, in, len);
}
void MockHal::RebootThunk(void *ctx) { ++static_cast<MockHal *>(ctx)->reboot_count_; }
