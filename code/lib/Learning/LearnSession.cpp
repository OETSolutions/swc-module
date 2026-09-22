#include "Learning/LearnSession.h"

#include <string.h>

#include "Analog/LadderDecode.h"

namespace {

// At least this many readings, and at least this long a span. The span matters
// independently of the count: ten readings taken within one millisecond are one
// instant, not a hold, and a burst like that is what a contact bounce looks like.
constexpr int      kMinSamples  = 10;
constexpr uint64_t kMinSpanMs   = 100;

// Spec 3.2: the calibrated ADC ceiling. Above this no pin reading is possible,
// so a sample exceeding it is a wiring or calibration fault, not a level. Read
// from the shared constant (CalibrationCurve.h, via LadderDecode.h) rather than a
// second local literal -- `LadderDecode.cpp` range-checks against the same fact.
constexpr int kAdcCeilingMv = kAdcFullScaleMv12dB;

// The learn's noise gate, as a permille of the idle reference -- NOT an absolute
// millivolt figure. The spread a "clean" hold produces scales with the rail (the
// ladder is a divider off +3V3, spec 6.3), so an absolute threshold is stricter
// at a high rail and looser at a low one. 60 permille is ~170 mV at the 2835 mV
// nominal idle, which is what the earlier absolute constant encoded; making it
// permille keeps that behaviour at nominal and rail-invariant everywhere else,
// matching how `LadderRatioPermille` already normalizes.
constexpr int kNoiseLimitPermille = 60;

// The "not pressed" band is the CLASSIFIER's idle band (`kIdleMarginPermille`,
// LadderDecode.h), not a local constant. It used to be a narrower local 20, so a
// learn whose mean landed at ratio 970-979 passed every gate here while
// `LadderClassify` returned `kIdle` for its own centre -- committing a DEAD button
// with a LEARN_OK beep. The learn gate must be no weaker than the classifier's,
// and sharing the one constant is what guarantees it.

int RoundDiv(int64_t num, int den) {
    if (den <= 0) return 0;
    return static_cast<int>((num + den / 2) / den);
}

}  // namespace

const char *LearnRejectReason(LearnReject r) {
    switch (r) {
        case LearnReject::kNone:               return "ok";
        case LearnReject::kTooFewSamples:      return "too_few_samples";
        case LearnReject::kOutOfRange:         return "out_of_range";
        case LearnReject::kNoIdleReference:    return "no_idle_reference";
        case LearnReject::kAtIdle:             return "at_idle";
        case LearnReject::kTooNoisy:           return "too_noisy";
        case LearnReject::kTooCloseToExisting: return "too_close_to_existing";
        // The SAME wire string the app path uses for the same condition
        // (`HandleLearnCommit`'s `no_space` nack), so a client reading the reason
        // sees one vocabulary whether the learn was driven by the app or by AUX1.
        case LearnReject::kNoSpace:            return "no_space";
    }
    return "unknown";
}

void LearnSession::Start(const LadderProfile &existing) {
    existing_ = existing;

    sample_count_ = 0;
    in_range_count_ = 0;
    sum_mv_ = 0;
    min_mv_ = 0;
    max_mv_ = 0;
    out_of_range_seen_ = false;
    first_ms_ = 0;
    last_ms_ = 0;
    have_sample_ = false;
    have_idle_ = false;
    learned_idle_mv_ = 0;
    rail_mv_ = 0;
    temp_tenths_c_ = 0;
}

void LearnSession::AddSample(int level_mv, int idle_mv, MilliVolt rail_mv,
                             int16_t temp_tenths_c, uint64_t now_ms) {
    // The first sample with a USABLE IDLE fixes the frame this session measures
    // in, so the SEEDED neighbour set is converted into that frame exactly once.
    //
    // This is not cosmetic. `existing_` holds the channel's already-learned
    // buttons as absolute millivolts at the rail they were STORED against, while
    // `level_mv` -- and so `mean_mv`, which the gates below compare to those
    // buttons -- is an absolute reading on the LIVE rail. Two frames in one
    // comparison is the defect class this project keeps re-finding, and here it
    // is silent: the new measurement matches the wrong sibling, and the
    // tolerance's nearest-gap is measured to a centre that moved. The caller
    // rebases its own copy of the seeded buttons the same way (`LadderProfileRebase`)
    // so what is STORED and what was GATED agree.
    //
    // **Gated on the IDLE being usable, not on the sample being the first one.**
    // The condition is `idle_mv > 0` because the conversion needs a target frame;
    // a sample that carries no idle cannot supply one. Keying it off `have_sample_`
    // instead (the first sample, whatever it carried) skipped the rebase for a
    // session whose opening reading had no idle and then stamped a LATER reading's
    // idle as `learned_idle_mv` -- so the commit's denominator was the live frame
    // while the seeded siblings stayed in the stored one, exactly the two-frames
    // failure above, and reachable by an ADC read that failed on the first prompt
    // tick.
    //
    // **`have_idle_` latches BOTH consumers of the reference**, so the rebase
    // frame and the commit denominator are the SAME sample's idle. They were two
    // policies before: this gate took the FIRST usable idle while the line below
    // overwrote `learned_idle_mv_` on every sample -- last-writer-wins -- and a
    // later reading of 0 (the `-1` sentinel's neighbour, an unreadable tick)
    // reset the denominator to 0 and refused a learn that had seen a perfectly
    // good reference. The two frames diverging is the same silent failure as
    // above, so one latch fixes both directions at once.
    if (!have_idle_ && idle_mv > 0) {
        LadderProfileRebase(existing_, existing_.learned_idle_mv, idle_mv);
        learned_idle_mv_ = static_cast<MilliVolt>(idle_mv);
        have_idle_ = true;
    }
    ++sample_count_;
    if (!have_sample_) {
        first_ms_ = now_ms;
        have_sample_ = true;
    }
    last_ms_ = now_ms;

    // The rail and temperature are recorded from the SAMPLES, not from
    // `existing`: a fresh learn is passed an empty profile, so its
    // `learned_idle_mv` is zero, while these are real readings the caller hands
    // us every time. (The plan's prose said "recorded at Start"; that cannot be
    // right, and its own test asserts the sampled value.) The IDLE reference is
    // NOT re-recorded here -- it latched above, once, with the rebase.
    rail_mv_ = rail_mv;
    temp_tenths_c_ = temp_tenths_c;

    if (level_mv < 0 || level_mv > kAdcCeilingMv) {
        // Counted by the sample gate above, excluded from the statistics here.
        out_of_range_seen_ = true;
        return;
    }

    if (in_range_count_ == 0) {
        min_mv_ = level_mv;
        max_mv_ = level_mv;
    } else {
        if (level_mv < min_mv_) min_mv_ = level_mv;
        if (level_mv > max_mv_) max_mv_ = level_mv;
    }
    sum_mv_ += level_mv;
    ++in_range_count_;
}

LearnReject LearnSession::Commit(LadderButton *out) {
    if (out == nullptr) return LearnReject::kTooFewSamples;

    // Gate order is the whole design: the FIRST failure is returned, so the user
    // gets the most actionable reason. That only works if gate 1 can be passed
    // by the same samples gate 2 is about to reject -- hence the two counters.
    if (sample_count_ < kMinSamples || (last_ms_ - first_ms_) < kMinSpanMs) {
        return LearnReject::kTooFewSamples;
    }
    if (out_of_range_seen_) return LearnReject::kOutOfRange;
    if (in_range_count_ == 0) return LearnReject::kOutOfRange;

    // No usable idle reference: REFUSE, do not commit. Every gate below is a
    // RATIO against the idle, and they substitute a nominal 2835 mV when the
    // session recorded none -- which is right for the ratio itself (a reading can
    // still be judged "pressed" and "steady" without the true rail) but wrong for
    // the OUTCOME, because the caller stores `LearnedIdleMv()` as the profile's
    // `learned_idle_mv`. Zero there is a profile `LadderProfileIsValid` refuses
    // (`learned_idle_mv <= 0`), so the commit would be accepted, reported as
    // LEARN_OK, applied in memory, and then persisted by a `Save` that does NOT
    // validate -- and the next boot's `ConfigDecodeBlob` refuses the whole config,
    // `Load` falls back to defaults, and the user loses every learned button and
    // binding, reported only as a corrupt config. That is the exact chain the
    // `LadderProfileIsValid(prospective)` gate below was added to close for a
    // noisy WINDOW; this closes it for the REFERENCE, which that gate cannot see
    // because it validates a profile whose `learned_idle_mv` is set from the
    // session's (possibly zero) `learned_idle_mv_`.
    //
    // Reachable: `AddSample` records the idle it is GIVEN, so a call site that
    // passes 0 (a channel whose live idle is unreadable, or a `ladder_sample`
    // that reported 0 for a stale reading) is enough. A zero idle cannot build a
    // profile the device can classify, so there is nothing to commit.
    if (learned_idle_mv_ <= 0) return LearnReject::kNoIdleReference;

    const int mean_mv = RoundDiv(sum_mv_, in_range_count_);
    const int spread_mv = max_mv_ - min_mv_;

    // At idle: the button was not pressed. Measured against the idle reference
    // rather than an absolute floor, so it holds at any rail.
    //
    // LadderRatioPermille returns level/idle * 1000, so a reading AT idle is
    // ~1000 -- NOT ~0. (An earlier revision compared against zero, which made
    // this gate fire on every plausible reading instead of only on an unpressed
    // one. The convention is shared with the classifier, so it is worth stating
    // where it is easy to get backwards.)
    //
    // The reference is the session's OWN `learned_idle_mv_`, with no fallback: the
    // gate above refuses a session whose reference is absent, so the nominal
    // substitute that used to sit here was the one number that could let a
    // reference-less session through to a commit that cannot be stored. One
    // denominator, the one the caller will persist.
    const int idle = learned_idle_mv_;
    const int ratio = LadderRatioPermille(mean_mv, idle);
    if (ratio >= 1000 - kIdleMarginPermille && ratio <= 1000 + kIdleMarginPermille) {
        return LearnReject::kAtIdle;
    }

    if (LadderRatioPermille(spread_mv, idle) > kNoiseLimitPermille) {
        return LearnReject::kTooNoisy;
    }

    // Too close to something already learned: every reading in the overlap is
    // equally close to both, so the classifier could not choose.
    for (uint8_t i = 0; i < existing_.count && i < kLadderMaxButtons; ++i) {
        const LadderButton &b = existing_.buttons[i];
        const int d = mean_mv - static_cast<int>(b.mv_center);
        const int ad = (d < 0) ? -d : d;
        if (ad <= static_cast<int>(b.mv_tolerance)) {
            return LearnReject::kTooCloseToExisting;
        }
    }

    // --- accepted ---------------------------------------------------------
    // Tolerance is HALF THE GAP to the nearest neighbouring centre, capped, and
    // floored so the window always covers the spread it was measured through.
    //
    // Deriving it from the spread instead ("spread * 2") is the classic cause of
    // two buttons triggering the same action: a wide spread would WIDEN the
    // window toward its neighbour, when the window should be bounded by how far
    // away that neighbour actually is. The spread's only role is as a floor.
    int nearest_gap = kMaxToleranceMv * 2;   // no neighbour -> the cap governs
    for (uint8_t i = 0; i < existing_.count && i < kLadderMaxButtons; ++i) {
        const int d = mean_mv - static_cast<int>(existing_.buttons[i].mv_center);
        const int ad = (d < 0) ? -d : d;
        if (ad < nearest_gap) nearest_gap = ad;
    }
    int tolerance = nearest_gap / 2;
    if (tolerance > kMaxToleranceMv) tolerance = kMaxToleranceMv;
    if (tolerance < spread_mv) tolerance = spread_mv;
    if (tolerance < 1) tolerance = 1;

    // REFUSE a window the config validator would reject, checked against the
    // profile this button would COMPLETE. Without this the learn commits, reports
    // LEARN_OK, is applied in memory, and persists -- and then the next boot's
    // `ConfigDecodeBlob` refuses the whole config (`ConfigValidate` runs at the end
    // of every decode), so `ConfigStore::Load` falls back to defaults and the user
    // loses every button they ever taught, with the cause reported only as a
    // corrupt config. `Save` does not validate, which is what makes that reachable.
    //
    // Reachable in practice: the tolerance FLOOR above runs after the cap, so a
    // noisy learn (spread just under kNoiseLimitPermille) can push the window past
    // kMaxToleranceMv and into its neighbour. The gate below the "too close" check
    // asks the same question of the MEAN; this one asks it of the WINDOW, which is
    // what the validator compares.
    {
        LadderProfile prospective = existing_;
        if (prospective.count < kLadderMaxButtons) {
            LadderButton probe{};
            probe.mv_center = static_cast<MilliVolt>(mean_mv);
            probe.mv_tolerance = static_cast<MilliVolt>(tolerance);
            prospective.buttons[prospective.count] = probe;
            ++prospective.count;
            // The reference the validator and the classifier both use -- the
            // session's OWN, and NOT a substitute. It is guaranteed positive here
            // by the gate at the top of `Commit`, so this validation checks the
            // real stored reference rather than a stand-in that could pass while
            // the value the caller stamps is zero.
            prospective.learned_idle_mv = learned_idle_mv_;
            // The WHOLE validity predicate, not just the window relation. Three
            // routes were found to a commit the validator refuses: a window
            // overlapping its neighbour, a duplicate id (handled by the wizard,
            // which owns ids), and a level of 0 from an unreadable ADC giving
            // `mv_center == 0`. Checking one relation would have missed the last.
            if (!LadderProfileIsValid(prospective)) {
                return LearnReject::kTooNoisy;
            }
        }
    }

    out->mv_center = static_cast<MilliVolt>(mean_mv);
    out->mv_tolerance = static_cast<MilliVolt>(tolerance);
    out->learned_at_rail_mv = rail_mv_;
    out->temp_c_at_learn = temp_tenths_c_;
    out->sample_count = static_cast<uint16_t>(in_range_count_);

    // Confidence: how much of the window the noise actually consumed. A steady
    // level scores 100; a level whose spread fills its own window scores 0.
    int conf = 100 - ((spread_mv * 100) / (2 * tolerance));
    if (conf > 100) conf = 100;
    if (conf < 0) conf = 0;
    out->confidence = static_cast<uint8_t>(conf);
    // id and name are deliberately NOT touched -- the caller's, not learn's.
    return LearnReject::kNone;
}
