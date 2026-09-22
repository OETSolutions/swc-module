#pragma once

#include <stdint.h>

#include "Analog/LadderDecode.h"

/*
 * Learn mode (FR-28..FR-31): measure a button's level on a steering-wheel ladder
 * and turn it into a LadderButton.
 *
 * **The session fills six of LadderButton's eight fields, not two.** Spec 3.4
 * defines eight, and learn is the only source of `mv_center`, `mv_tolerance`,
 * `learned_at_rail_mv`, `temp_c_at_learn`, `sample_count` and `confidence`.
 * `id` and `name` are the CALLER's -- a slug and a display label are not facts
 * about a voltage, and learn cannot invent them.
 *
 * **Host-testable by construction**: no IHAL, no clock of its own. Samples and
 * the timestamp arrive as arguments, so a whole learn run is exercised in
 * microseconds with no hardware.
 */

enum class LearnReject {
    kNone = 0,            // committed
    kTooFewSamples,       // not enough samples, or too short a span
    kOutOfRange,          // a sample above the calibrated ADC ceiling
    kNoIdleReference,     // the live idle reference was absent (0)
    kAtIdle,              // the button was never pressed
    kTooNoisy,            // the level wandered further than classification can tolerate
    kTooCloseToExisting,  // indistinguishable from a button already learned
    kNoSpace,             // the ladder is full; nothing could be stored
};

// The wire string for a rejection. FR-29 requires the reason to be specific:
// "it didn't work" is not actionable for a user holding a button one-handed.
const char *LearnRejectReason(LearnReject r);

class LearnSession {
public:
    // `existing` is the set of buttons this learn must NOT collide with.
    //
    // **It is the channel's ladder MINUS the entry this learn is replacing**, and
    // that subtraction is the CALLER's job because only the caller knows which
    // entry that is. The wizard seeds its profile from the channel's whole ladder
    // (otherwise a learn DELETES the channel's other buttons -- see
    // LearnWizard::Enter), and it replaces an existing entry by matching the id it
    // is about to generate. So the button being re-measured is in the neighbour
    // set unless the wizard removes it, and re-learning that button would be
    // refused as `too_close_to_existing` -- blaming it for being too close to
    // ITSELF, since a re-measure lands within the old window by definition. That
    // would make a button impossible to correct.
    //
    // This class deliberately knows nothing about ids: it compares VOLTAGES. The
    // id is the wizard's vocabulary (it generates `swc1_bt2`), so the id-based
    // decision stays there and this takes the already-adjusted set.
    void Start(const LadderProfile &existing);

    // One reading. `level_mv` is the calibrated ladder level and `idle_mv` the
    // live idle reference; `rail_mv` is spec 3.4's "+3V3 rail measured during
    // learn" and is a PARAMETER because this board has no rail sense channel
    // (AdcChannel has SWC1/SWC2/TEMP/AUX1-3/KEY_SENSE1-2 and none is the rail).
    // `temp_tenths_c` comes from ADC_CH_TEMP -- the NTC on the ladder.
    void AddSample(int level_mv, int idle_mv, MilliVolt rail_mv, int16_t temp_tenths_c,
                   uint64_t now_ms);

    // Runs the gates and, on success, fills the six fields learn owns. `out`'s
    // id/name are left EXACTLY as the caller set them.
    //
    // **A session that never saw a usable idle reference CANNOT commit.** The
    // callers store `LearnedIdleMv()` as the profile's `learned_idle_mv`, and
    // `LadderProfileIsValid` refuses a zero reference -- so a commit without one
    // would be accepted here, reported as success, and then persist a config the
    // next boot's `ConfigValidate` rejects, losing the user's whole config to
    // `kFellBackToDefaults` (the same "validator runs on the way IN, never OUT"
    // hole as the noisy-tolerance defect). The gates below deliberately substitute
    // a nominal idle for their RATIO math, so they could all pass with the stored
    // reference still 0; the check that catches it is here, at the point of
    // decision, and it is the last thing the caller needs before it stamps the
    // profile.
    LearnReject Commit(LadderButton *out);

    int SampleCount() const { return sample_count_; }
    MilliVolt LearnedIdleMv() const { return learned_idle_mv_; }

    // The tolerance cap (spec 3.4). Exposed because it is a design constant a
    // caller may legitimately override for a bench sweep, and because a test
    // asserting the cap should name the same number the code uses.
    static constexpr int kMaxToleranceMv = 120;

private:
    // Gates 1 and 2 need different sample sets: the COUNT gate counts every
    // AddSample call, while the statistics (mean, spread) use only in-range
    // readings. With one counter, 30 implausible readings would fail the count
    // gate and report "hold the button longer" when nothing the user does can
    // help. Two counters, deliberately.
    int      sample_count_ = 0;       // every AddSample
    int      in_range_count_ = 0;     // the ones the statistics used
    int64_t  sum_mv_ = 0;
    int      min_mv_ = 0;
    int      max_mv_ = 0;
    bool     out_of_range_seen_ = false;
    uint64_t first_ms_ = 0;
    uint64_t last_ms_ = 0;
    bool     have_sample_ = false;
    // Whether the session's idle REFERENCE has been established -- by the first
    // sample that carries a usable idle, and then for good. Its OWN flag, NOT
    // `have_sample_`, and ONE flag for BOTH consumers of the reference: the
    // rebase below converts the seeded neighbours into this frame, and
    // `learned_idle_mv_` is the denominator `Commit` stamps. Keying either off
    // the ordinal skipped the rebase for a session whose first reading had no
    // idle, and letting the denominator take the LAST sample's idle (or a later
    // zero reset it) put the stored siblings in one frame and the committed
    // ratio in another -- the silent wrong-button failure the rebase exists to
    // prevent (see AddSample).
    bool     have_idle_ = false;

    MilliVolt learned_idle_mv_ = 0;
    MilliVolt rail_mv_ = 0;
    int16_t   temp_tenths_c_ = 0;

    LadderProfile existing_{};
};
