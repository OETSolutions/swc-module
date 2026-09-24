#include "Feedback/BuzzerGrammar.h"

#include <string.h>

namespace {

// Spec 7.2's table, transcribed. `reps` is a property of the named pattern, not
// a runtime argument -- that is what the grammar
// (`pattern := pulse(on_ms, off_ms), repeat, gap_ms`) expresses, and it is why
// PROGRAM_STEP is one pulse that the CALLER repeats n times.
//
// Every entry is {on_ms, off_ms, reps}. The final pulse's off is trimmed at
// playback so the pattern's total is exactly sum(on+off) - trailing_off; a
// pattern that ended with a full off would hold the line silent for one extra
// interval and make "count the beeps in T ms" depend on T.
struct Step {
    uint16_t on_ms;
    uint16_t off_ms;
    uint8_t  reps;
};

Step StepsFor(BuzzerPattern p) {
    switch (p) {
        case BuzzerPattern::kBootOk:        return {60, 60, 1};
        case BuzzerPattern::kBootDegraded:  return {60, 60, 3};
        case BuzzerPattern::kBootError:     return {500, 200, 2};
        case BuzzerPattern::kKeyAccepted:   return {25, 0, 1};
        case BuzzerPattern::kKeyUnknown:    return {120, 80, 1};
        case BuzzerPattern::kProgramEnter:  return {40, 40, 2};
        case BuzzerPattern::kProgramStep:   return {40, 40, 1};
        case BuzzerPattern::kProgramSaved:  return {40, 20, 4};
        case BuzzerPattern::kProgramExit:   return {200, 0, 1};
        case BuzzerPattern::kProgramCancel: return {300, 100, 1};
        case BuzzerPattern::kLearnPrompt:   return {100, 100, 1};
        case BuzzerPattern::kLearnOk:       return {40, 30, 2};
        case BuzzerPattern::kLearnReject:   return {300, 80, 2};
        case BuzzerPattern::kFaultDac:      return {500, 300, 3};
        case BuzzerPattern::kFaultConfig:   return {500, 300, 4};
        // Spec 7.2's INPUT fault (N-10): a ladder/rail wiring fault. 3 reps like
        // FAULT_DAC -- a distinct, insistent rhythm -- so a user with led_level 0
        // still hears that the device is faulted and needs attention.
        case BuzzerPattern::kFaultInput:    return {500, 300, 3};
        case BuzzerPattern::kFactoryReset:  return {800, 200, 3};
        // Spec 7.2's OTA row has no 2022 numbers and its prose says "rising
        // double", which a fixed-tone gated buzzer cannot produce. Encoded as
        // the length-based reading: OTA_OK's pulses are longer, not higher.
        case BuzzerPattern::kOtaStart:      return {400, 0, 1};
        case BuzzerPattern::kOtaOk:         return {150, 100, 2};
        case BuzzerPattern::kOtaFail:       return {80, 40, 3};
        case BuzzerPattern::kNone:
        default:                            return {0, 0, 0};
    }
}

// Total milliseconds a pattern occupies, with the final off trimmed.
uint32_t TotalMs(Step s) {
    if (s.reps == 0) return 0;
    return static_cast<uint32_t>(s.reps) * (s.on_ms + s.off_ms) - s.off_ms;
}

}  // namespace

BuzzerGrammar::BuzzerGrammar(IHAL *hal, uint8_t level) : hal_(hal), level_(level) {}

bool BuzzerGrammar::IsFatal(BuzzerPattern p) {
    return p == BuzzerPattern::kBootError || p == BuzzerPattern::kFaultDac ||
           p == BuzzerPattern::kFaultConfig || p == BuzzerPattern::kFaultInput;
}

void BuzzerGrammar::Play(BuzzerPattern p) {
    // Replace, never queue. A queued pattern would play after the event it
    // describes is stale, and the user would hear feedback for something that
    // already scrolled past.
    if (hal_ != nullptr) {
        hal_->buzzer_on(hal_->ctx, false);
        out_ = false;
    }
    pattern_ = p;
    playing_ = p != BuzzerPattern::kNone;
    started_ = false;
    start_ms_ = 0;
}

void BuzzerGrammar::Update(uint64_t now_ms) {
    if (hal_ == nullptr) return;

    if (!playing_) {
        // Idle ticks must be silent, and must not drive the line at all -- the
        // count of buzzer writes is what makes "feedback cannot delay a key
        // press" checkable.
        return;
    }

    // Suppressed patterns are still "played": they run their course so Busy()
    // reports honestly, but the line is never driven. Doing it by refusing to
    // start would make Busy() false during a pattern that is notionally running.
    const bool silent = (level_ == 0) && !IsFatal(pattern_);

    if (!started_) {
        started_ = true;
        start_ms_ = now_ms;
    }

    const Step s = StepsFor(pattern_);
    const uint32_t elapsed = static_cast<uint32_t>(now_ms - start_ms_);
    const uint32_t total = TotalMs(s);

    bool want = false;
    if (elapsed < total && s.on_ms > 0) {
        const uint32_t period = static_cast<uint32_t>(s.on_ms) + s.off_ms;
        const uint32_t phase = elapsed % period;
        want = phase < s.on_ms;
    }

    if (silent) want = false;

    if (want != out_) {
        hal_->buzzer_on(hal_->ctx, want);
        out_ = want;
    }

    if (elapsed >= total) {
        // Force the line low at the end of EVERY pattern, fatal or not. A stuck
        // buzzer is a stuck-on hardware fault, and this is the only place that
        // can prevent it.
        if (out_) {
            hal_->buzzer_on(hal_->ctx, false);
            out_ = false;
        }
        playing_ = false;
        started_ = false;
        pattern_ = BuzzerPattern::kNone;
    }
}

/*
 * Spec 3.6's `BUZZ` parameter -> a pattern, or kNone for a name this build does
 * not know.
 *
 * **The names are the enumerator names minus the `k` prefix**, which is the same
 * spelling spec 7.2's table uses and the same one `BuzzerGrammarTest` drives
 * `EverySpecPatternCompletesAndReleasesTheLine` from. That test's list is the
 * coverage check: a pattern added to the enum without a row here answers kNone,
 * and a `BUZZ` binding naming it would be silently inert.
 *
 * kNone ("stop", per the header) is deliberately NOT reachable by name: a stored
 * action that silences the buzzer is a different feature, and admitting the name
 * here would let a typo'd or hostile config do it.
 */
BuzzerPattern BuzzerPatternFromName(const char *name) {
    if (name == nullptr) return BuzzerPattern::kNone;
    struct Row { const char *name; BuzzerPattern p; };
    static constexpr Row kRows[] = {
        {"BootOk", BuzzerPattern::kBootOk},
        {"BootDegraded", BuzzerPattern::kBootDegraded},
        {"BootError", BuzzerPattern::kBootError},
        {"KeyAccepted", BuzzerPattern::kKeyAccepted},
        {"KeyUnknown", BuzzerPattern::kKeyUnknown},
        {"ProgramEnter", BuzzerPattern::kProgramEnter},
        {"ProgramStep", BuzzerPattern::kProgramStep},
        {"ProgramSaved", BuzzerPattern::kProgramSaved},
        {"ProgramExit", BuzzerPattern::kProgramExit},
        {"ProgramCancel", BuzzerPattern::kProgramCancel},
        {"LearnPrompt", BuzzerPattern::kLearnPrompt},
        {"LearnOk", BuzzerPattern::kLearnOk},
        {"LearnReject", BuzzerPattern::kLearnReject},
        {"FaultDac", BuzzerPattern::kFaultDac},
        {"FaultConfig", BuzzerPattern::kFaultConfig},
        {"FaultInput", BuzzerPattern::kFaultInput},
        {"FactoryReset", BuzzerPattern::kFactoryReset},
        {"OtaStart", BuzzerPattern::kOtaStart},
        {"OtaOk", BuzzerPattern::kOtaOk},
        {"OtaFail", BuzzerPattern::kOtaFail},
    };
    for (const Row &r : kRows) {
        if (strcmp(r.name, name) == 0) return r.p;
    }
    return BuzzerPattern::kNone;
}
