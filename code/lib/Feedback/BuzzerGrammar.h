#pragma once

#include <stdint.h>

#include "HAL/IHAL.h"

// Spec 7.2's complete pattern table. One enumerator per named pattern, and no
// more: the table is the vocabulary, and a pattern that exists only in a caller
// is a pattern that cannot be tested or referenced from config.
//
// kNone is not a pattern. It means "stop", and it is the only value Play may be
// given that leaves the buzzer silent rather than starting something.
enum class BuzzerPattern {
    kNone,
    kBootOk, kBootDegraded, kBootError,
    kKeyAccepted, kKeyUnknown,
    kProgramEnter, kProgramStep, kProgramSaved, kProgramExit, kProgramCancel,
    kLearnPrompt, kLearnOk, kLearnReject,
    kFaultDac, kFaultConfig, kFactoryReset,
    kOtaStart, kOtaOk, kOtaFail,
};

/*
 * The buzzer grammar. Spec 7.1 fixes the part at ~2.4 kHz with on/off gating
 * only, so every pattern is rhythm: a pulse train, never a melody.
 *
 * Update() is driven from the main loop and never blocks (FR-21) -- a pattern
 * that blocked would delay the key press it is acknowledging, which is worse
 * than no feedback at all. Play() while a pattern is running REPLACES it rather
 * than queueing: the newest event is the one the user is waiting on.
 */
class BuzzerGrammar {
public:
    BuzzerGrammar(IHAL *hal, uint8_t level);

    void Play(BuzzerPattern p);
    void Update(uint64_t now_ms);
    bool Busy() const { return playing_; }

private:
    // Whether `level_ == 0` may suppress this pattern. Spec 7.2: OFF silences
    // everything except BOOT_ERROR and FAULT_*. A device that cannot serve
    // output must still be able to say so, which is the whole reason those two
    // are exceptions.
    static bool IsFatal(BuzzerPattern p);

    IHAL    *hal_;
    uint8_t  level_;

    BuzzerPattern pattern_ = BuzzerPattern::kNone;
    bool     playing_  = false;
    // Play() has no clock argument (it is called from event handlers that may
    // not have one handy), so the start time is latched on the first Update
    // after Play. Until then the pattern is armed but has not begun.
    bool     started_  = false;
    uint64_t start_ms_ = 0;
    // The last level driven, so Update writes only on change. Re-driving the
    // line every tick would make GpioWriteCount meaningless as a signal.
    bool     out_     = false;
};
