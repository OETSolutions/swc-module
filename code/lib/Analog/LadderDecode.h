#pragma once

#include <stdint.h>

#include "HAL/IHAL.h"

// MilliVolt comes from IHAL.h -- spec 3.2's value type, a pin voltage in
// millivolts, 0-2900 (the ADC's calibrated ceiling). It was declared locally
// here while Task 2's IHAL.h block omitted it; the typedef is now in the frozen
// header, so the workaround is gone.

// Ratios are permille (thousandths of the idle reference) so the whole
// comparison path is integer arithmetic. Floating point on this target is
// slower and the windows are generous enough (tens of permille) that integer
// rounding is irrelevant.
constexpr int kLadderMaxButtons = 16;
// Widths are a budget input, not a preference: the config JSON must fit two
// NVS slots, and these strings are part of the structural worst case
// (spec 3.5's width table). Widening them overflows the partition.
constexpr int kLadderIdLen   = 16;
constexpr int kLadderNameLen = 16;

// The board's nominal +3V3 rail, which is what `learned_at_rail_mv` records
// (spec 3.4/FR-30). There is no rail SENSE channel on this board (AdcChannel
// carries SWC1/SWC2/TEMP/AUX1-3/KEY_SENSE1-2 and none is +3V3), so this is the
// nominal value, recorded as such. It lives here, beside the field it fills,
// because BOTH learn paths record it -- the app-driven session in `CommandRouter`
// and the headless wizard -- and a literal in each is two homes that drift.
constexpr MilliVolt kNominalRailMv = 3300;

// The spec 3.4 shape, in millivolts at the pin. The *ratio* the classifier
// compares is DERIVED at classify time from mv_center and the profile's learned
// idle -- it is deliberately not stored, because storing both forms is 105% of
// the NVS partition (spec 3.5) and a stored ratio is a second home for a value
// that mv_center already determines.
struct LadderButton {
    char      id[kLadderIdLen];        // stable slug, referenced by Binding.button
    char      name[kLadderNameLen];    // display only
    MilliVolt mv_center;               // pin voltage when this button is held
    MilliVolt mv_tolerance;            // half-width of the accept window
    MilliVolt learned_at_rail_mv;      // the +3V3 rail (approx 3300), NOT 12 V
    int16_t   temp_c_at_learn;         // tenths of a degree C, for FR-17
    uint16_t  sample_count;            // samples averaged at learn
    uint8_t   confidence;              // learn-quality score, 0-100
};

struct LadderProfile {
    uint8_t      source;            // spec 3.1: a direct analog input
    // The idle reading at LEARN time. This is the normalization reference, so it
    // must NOT be the current idle: mv_center is pinned to the rail that was
    // present when it was learned, and the rail-health check (FR-30) compares
    // the CURRENT idle against this one. The wire field is `idle_mv` (spec 3.7);
    // the struct keeps the `learned_` prefix because the distinction is what the
    // rail-health check is made of.
    MilliVolt    learned_idle_mv;
    uint8_t      count;
    LadderButton buttons[kLadderMaxButtons];
};

enum class ClassifyResult { kIdle, kButton, kUnknown, kFault };

struct ClassifyOutcome {
    ClassifyResult result;
    uint8_t        index;   // valid only when result == kButton
    int16_t        ratio_permille;
};

// level_mv as a fraction of the IDLE reading, in permille (spec 6.3:
// n = V_ADC / V_ADC_idle). Idle is 1000 by construction; a press pulls the
// input DOWN, so every learned button's ratio is below 1000.
// Callers must guarantee idle_mv > 0; LadderClassify reports kFault otherwise.
int16_t LadderRatioPermille(int level_mv, int idle_mv);

ClassifyOutcome LadderClassify(const LadderProfile &profile, int level_mv, int idle_mv);

/*
 * Rescale a profile's ABSOLUTE millivolts from the rail they were measured at
 * onto another rail, so a profile can keep ONE denominator after a re-learn.
 *
 * **Why this has to exist.** `LadderProfile` carries a single `learned_idle_mv`,
 * while each `LadderButton` carries `mv_center`/`mv_tolerance` as absolute pin
 * millivolts measured at that one rail. A learn measures on the LIVE rail and
 * both learn paths SEED the profile with the channel's existing buttons (so a
 * re-learn corrects one entry instead of deleting the rest -- spec 7.4). If the
 * stored profile was measured at a different rail than the live one, the newly
 * measured button lands in the live frame while every seeded sibling stays in the
 * stored frame, and the profile's single denominator then applies to both. The
 * ratios are read on the wrong scale, and that does not fail loudly: the press
 * matches whichever window it now falls inside, so the WRONG button fires.
 *
 * Measured through the real learn path: two buttons 200 mV apart learned at
 * 2835 mV, then one re-learned at 2693 mV (a -5 % regulator deviation, inside the
 * documented +-5 % band). Pressing the OTHER button -- stored ratio 864 permille
 * -- fired the button at 794 permille, and its own window was no longer reachable.
 *
 * Scaling is the correct transform, not ratio preservation in the abstract: the
 * ladder is a divider off +3V3, so a button's pin voltage (and its spread) scale
 * with the rail. Both `mv_center` and `mv_tolerance` therefore scale, which keeps
 * every relative permille window EXACTLY invariant -- `LadderRatioPermille` of a
 * scaled button against the scaled denominator returns the same permille, so
 * `LadderWindowsAreDistinguishable` cannot change under a rebase.
 *
 * A non-positive `from_idle_mv` (a profile that has no reference yet -- a fresh
 * learn, or a channel with no learned ladder) is left untouched: there is no
 * source frame to convert from, and the caller's own measurement is already in
 * the target frame.
 */
void LadderProfileRebase(LadderProfile &p, int from_idle_mv, int to_idle_mv);

/*
 * Is this profile one a config may CARRY? Every check `ConfigValidate` applies to a
 * channel's ladder, in one place.
 *
 * **It exists because `LearnSession::Commit` must refuse to produce a profile the
 * validator would reject, and `ConfigStore::Save` does not validate.** Three
 * separate routes were found, all of them silent and all of them ending in the same
 * place: the learn reports LEARN_OK, the config is written, and the next boot's
 * decode refuses it -- `ConfigStore::Load` then falls back to defaults and the user
 * loses every learned button, reported only as a corrupt config. The routes were a
 * window overlapping its neighbour, a duplicate id, and a level of 0 (what an
 * unreadable ADC reports) producing `mv_center == 0`. Checking only the window
 * relation would have missed the last two, which is why the gate is the WHOLE
 * predicate rather than the one relation that was noticed first.
 *
 * The `id` field is deliberately NOT checked here: it is the CALLER's, learn cannot
 * invent it, and a profile that is not yet named is a legitimate intermediate.
 */
bool LadderProfileIsValid(const LadderProfile &p);

// Are every two of this profile's windows distinguishable, so classification is a
// measurement rather than a coin toss?
//
// **This predicate has ONE home, and it has two callers on purpose.** The config
// validator refuses a profile that fails it (FR-26), and `LearnSession` refuses to
// COMMIT one -- because `ConfigStore::Save` writes whatever it is handed, so a
// learn that skipped this check could persist a config its own decoder then
// refused to load, costing the user their whole config at the next boot. Deriving
// the relation in two places is how the two would drift; the comparison is in the
// same derived permille form the classifier uses, and it must stay that way.
//
// Adjacent windows may legitimately OVERLAP by a few permille -- the classifier
// resolves that by nearest centre. What is ambiguous is when the centres are
// closer together than the wider of the two tolerances: every reading in the
// overlap is then equally close to both.
bool LadderWindowsAreDistinguishable(const LadderProfile &p);
