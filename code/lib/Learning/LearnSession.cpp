#include "Learning/LearnSession.h"

#include <string.h>

namespace {

// At least this many readings, and at least this long a span. The span matters
// independently of the count: ten readings taken within one millisecond are one
// instant, not a hold, and a burst like that is what a contact bounce looks like.
constexpr int      kMinSamples  = 10;
constexpr uint64_t kMinSpanMs   = 100;

// Spec 3.2: the calibrated ADC ceiling. Above this no pin reading is possible,
// so a sample exceeding it is a wiring or calibration fault, not a level.
constexpr int kAdcCeilingMv = 2900;

// ~60 permille at a 2835 mV idle. Wider than the classification tolerance on
// purpose: this gate rejects a learn the classifier could not serve, not one
// that is merely imperfect.
constexpr int kNoiseLimitMv = 170;

// How close to the idle reference still counts as "not pressed", in permille.
constexpr int kIdleMarginPermille = 20;

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
        case LearnReject::kAtIdle:             return "at_idle";
        case LearnReject::kTooNoisy:           return "too_noisy";
        case LearnReject::kTooCloseToExisting: return "too_close_to_existing";
    }
    return "unknown";
}

void LearnSession::Start(int channel, const LadderProfile &existing) {
    channel_ = channel;
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
    learned_idle_mv_ = 0;
    rail_mv_ = 0;
    temp_tenths_c_ = 0;
}

void LearnSession::AddSample(int level_mv, int idle_mv, MilliVolt rail_mv,
                             int16_t temp_tenths_c, uint64_t now_ms) {
    ++sample_count_;
    if (!have_sample_) {
        first_ms_ = now_ms;
        have_sample_ = true;
    }
    last_ms_ = now_ms;

    // The live idle and the rail are recorded from the SAMPLES, not from
    // `existing`: a fresh learn is passed an empty profile, so its
    // `learned_idle_mv` is zero, while the live idle is a real ADC reading the
    // caller hands us every time. (The plan's prose said "recorded at Start";
    // that cannot be right, and its own test asserts the sampled value.)
    learned_idle_mv_ = static_cast<MilliVolt>(idle_mv > 0 ? idle_mv : 0);
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
    const int idle = (learned_idle_mv_ > 0) ? learned_idle_mv_ : 2835;
    const int ratio = LadderRatioPermille(mean_mv, idle);
    if (ratio >= 1000 - kIdleMarginPermille && ratio <= 1000 + kIdleMarginPermille) {
        return LearnReject::kAtIdle;
    }

    if (spread_mv > kNoiseLimitMv) return LearnReject::kTooNoisy;

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
