#include "Feedback/LedGrammar.h"

namespace {

// LED_STAT's cadences, from spec 7.3. A pattern is a repeating sequence of
// {on_ms, off_ms} pulses, so a burst (double-flash) is representable and is
// genuinely distinct from a blink rather than a relabelled one.
//
// The representation is fixed-size because every pattern in the spec is short
// and known at compile time; a heap-allocated step list would be the only
// allocation in the feedback path, for no gain.
struct Sequence {
    uint16_t on_ms[4];
    uint16_t off_ms[4];
    uint8_t  steps;
};

bool StatIsConstant(LedStatPattern p, bool *value) {
    if (p == LedStatPattern::kOff)   { *value = false; return true; }
    if (p == LedStatPattern::kSolid) { *value = true;  return true; }
    return false;
}

Sequence StatSequence(LedStatPattern p) {
    switch (p) {
        // 1 Hz breathe, approximated as a square cycle. The LED is on/off only
        // (spec 7.1 says software-PWM is possible but the grammar is defined by
        // rate and rhythm), so "breathe" here means the 1 Hz rate.
        case LedStatPattern::kBreathe:
            return {{500, 0, 0, 0}, {500, 0, 0, 0}, 1};
        // 5 Hz blink: 100/100.
        case LedStatPattern::kBlink:
            return {{100, 0, 0, 0}, {100, 0, 0, 0}, 1};
        // Double-flash burst, then a long gap so the PAIR reads as a burst.
        // A single 100/100 cycle would be indistinguishable from kBlink, which
        // is the one thing this pattern exists not to be.
        case LedStatPattern::kDoubleFlash:
            return {{100, 100, 0, 0}, {100, 600, 0, 0}, 2};
        case LedStatPattern::kAlternate:
            return {{250, 0, 0, 0}, {250, 0, 0, 0}, 1};
        default:
            return {{0, 0, 0, 0}, {0, 0, 0, 0}, 0};
    }
}

// LED2's cadences, from spec 7.3.
bool Led2IsConstant(Led2Pattern p, bool *value) {
    if (p == Led2Pattern::kOff)   { *value = false; return true; }
    if (p == Led2Pattern::kSolid) { *value = true;  return true; }
    return false;
}

Sequence Led2Sequence(Led2Pattern p) {
    switch (p) {
        // A flick is one short pulse with a long gap, so it reads as an event
        // rather than as a low-rate blink.
        case Led2Pattern::kFlick:
            return {{80, 0, 0, 0}, {920, 0, 0, 0}, 1};
        // 0.5 s long pulse, once per 2 s.
        case Led2Pattern::kLongPulse:
            return {{500, 0, 0, 0}, {1500, 0, 0, 0}, 1};
        default:
            return {{0, 0, 0, 0}, {0, 0, 0, 0}, 0};
    }
}

uint32_t SequencePeriod(const Sequence &s) {
    uint32_t total = 0;
    for (uint8_t i = 0; i < s.steps; ++i) {
        total += static_cast<uint32_t>(s.on_ms[i]) + s.off_ms[i];
    }
    return total;
}

bool PhaseOn(const Sequence &s, uint64_t elapsed) {
    const uint32_t period = SequencePeriod(s);
    if (period == 0) return false;
    uint32_t t = static_cast<uint32_t>(elapsed % period);
    for (uint8_t i = 0; i < s.steps; ++i) {
        if (t < s.on_ms[i]) return true;
        t -= s.on_ms[i];
        if (t < s.off_ms[i]) return false;
        t -= s.off_ms[i];
    }
    return false;
}

}  // namespace

LedGrammar::LedGrammar(IHAL *hal, uint8_t level) : hal_(hal), level_(level) {}

void LedGrammar::SetStat(LedStatPattern p) {
    stat_ = p;
    // Restart the phase so a state change is visible immediately rather than
    // resuming a cycle the previous state left half-done.
    started_ = false;
}

void LedGrammar::Set2(Led2Pattern p) {
    led2_ = p;
    started_ = false;
}

void LedGrammar::Update(uint64_t now_ms) {
    if (hal_ == nullptr) return;

    if (!started_) {
        started_ = true;
        start_ms_ = now_ms;
    }
    const uint64_t elapsed = now_ms - start_ms_;

    bool stat_want = false;
    if (!StatIsConstant(stat_, &stat_want)) {
        stat_want = PhaseOn(StatSequence(stat_), elapsed);
    }

    bool led2_want = false;
    if (stat_ == LedStatPattern::kAlternate) {
        // Spec 7.3's "alternating with LED2" is a property of the PAIR, not of
        // one channel: the point is that exactly one of the two is lit, which is
        // what distinguishes it from two independent blinks that might overlap.
        // So while LED_STAT is alternating, it owns LED2 and its complement is
        // what LED2 shows. This is why the two channels share one phase clock.
        led2_want = !stat_want;
    } else if (!Led2IsConstant(led2_, &led2_want)) {
        led2_want = PhaseOn(Led2Sequence(led2_), elapsed);
    }

    // Level 0 silences both channels (spec 7.2's buzzer rule applies to the LEDs
    // by the same argument: a user who asked for no feedback gets none). Unlike
    // the buzzer there is no fatal exception -- the LEDs are not how a fault is
    // reported when feedback is off, the buzzer's FAULT_* is.
    if (level_ == 0) {
        stat_want = false;
        led2_want = false;
    }

    if (stat_want != stat_out_) {
        hal_->gpio_write(hal_->ctx, GPIO_LED_STAT, stat_want);
        stat_out_ = stat_want;
    }
    if (led2_want != led2_out_) {
        hal_->gpio_write(hal_->ctx, GPIO_LED2, led2_want);
        led2_out_ = led2_want;
    }
}
