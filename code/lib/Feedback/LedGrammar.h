#pragma once

#include <stdint.h>

#include "HAL/IHAL.h"

// Spec 7.3's two channels. They are SEPARATE grammars, not one flat enum: the
// spec gives LED_STAT a state vocabulary (solid / breathe / blink / double-flash
// / alternate) and LED2 an activity vocabulary (off / flick / solid / long
// pulse), and several states exist on one channel and not the other.
//
// A single flat enum (an earlier revision's 9-value `LedPattern`) cannot express
// that: it forces every caller to say which channel a pattern is for anyway, and
// it cannot represent "LED_STAT breathes while LED2 flickers", which is the
// normal connected-and-idle state.
enum class LedStatPattern {
    kOff,        // no power / not running
    kSolid,      // all-good: running, output safe, USB connected, config valid
    kBreathe,    // 1 Hz: running normally, no USB
    kBlink,      // 5 Hz: fault -- the buzzer's FAULT_* says which
    kDoubleFlash,// maintenance mode active
    kAlternate,  // learn mode, awaiting a press (pairs with LED2)
};

enum class Led2Pattern {
    kOff,        // idle, no recent key activity
    kFlick,      // a gesture was recognised (mirrors KEY_ACCEPTED)
    kSolid,      // a key value is being presented -- the line is driven
    kLongPulse,  // gain mode changed, or the head unit was (re)detected
};

/*
 * Drives both LEDs from the injected clock. Never blocks (same reason as the
 * buzzer), and both channels run independently -- setting one does not disturb
 * the other's phase, which is what makes "alternate" an alternation rather than
 * two blinks that happen to be out of step.
 */
class LedGrammar {
public:
    LedGrammar(IHAL *hal, uint8_t level);

    void SetStat(LedStatPattern p);
    void Set2(Led2Pattern p);
    void Update(uint64_t now_ms);

    /*
     * Adopt a new feedback level WITHOUT disturbing the patterns in flight.
     *
     * A config that arrives over the link carries `settings.led_level`, and the
     * obvious `leds_ = LedGrammar(hal_, new_level)` is a bug: the freshly
     * constructed object has `stat_ == kOff`, so it SILENTLY CANCELS whatever is
     * showing -- and the learn wizard, which owns both LEDs as its prompts while it
     * runs, sets its pattern once on entry and does not re-set it per tick. A config
     * push landing mid-learn would blank the prompt the user is reading. Same
     * reasoning as `BuzzerGrammar::SetLevel`.
     */
    void SetLevel(uint8_t level) { level_ = level; }

private:
    IHAL    *hal_;
    uint8_t  level_;

    LedStatPattern stat_ = LedStatPattern::kOff;
    Led2Pattern    led2_ = Led2Pattern::kOff;

    bool     stat_out_ = false;
    bool     led2_out_ = false;
    bool     started_  = false;
    uint64_t start_ms_ = 0;
};
