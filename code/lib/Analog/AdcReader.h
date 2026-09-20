#pragma once

#include <stdint.h>

#include "HAL/IHAL.h"

/*
 * The ADC noise filter spec FR-3 requires (spec 5's timing table).
 *
 * **FR-3 is two requirements in one sentence, and they pull in opposite
 * directions**: "filter samples for noise while preserving a real button press's
 * edge; the filter's settling time MUST be shorter than the configured
 * `debounce_ms`". A filter that only smooths passes the first half and fails the
 * second; one that only tracks edges passes neither. Both halves are tested
 * separately, because a one-sided test cannot see the trade-off.
 *
 * **The 32 oversamples are a BURST inside one `Update`, not one sample per
 * call.** This is the fact that makes FR-3 satisfiable at all: 16-64 conversions
 * taken at the poll rate (≤100 Hz, spec 5) would be 160-640 ms of settling, and
 * `debounce_ms` is 25 ms -- so a per-poll window would *violate* the requirement
 * it exists to meet. Taking the window per-`Update` makes settling one or two
 * calls, comfortably inside `debounce_ms`, and it is what the timing table's
 * "16-64 oversamples averaged" means next to a ≤100 Hz sample rate.
 *
 * **Why a median rather than an IIR.** A median rejects an outlier conversion
 * outright -- an ADC glitch, a neighbour switching -- while an IIR smears it into
 * every later sample. It also makes the settling bound a consequence of the
 * window size rather than of a tuned coefficient.
 *
 * **This sits above the HAL.** `IHAL::adc_read_mv` already returns a CALIBRATED
 * millivolt value (the curve is applied inside EspHal), so this never touches raw
 * counts and never needs `CalibrationCurve`. That keeps it host-testable.
 */

/* Spec 5's table says 16-64 oversamples; 32 is the middle. A named constant
 * rather than a literal because the per-sample cost is a loop-budget question and
 * the number is expected to be tuned on hardware. */
constexpr int kAdcOversampleCount = 32;

/* How close two consecutive windows must agree before the value is called
 * settled. Not zero: a real ADC's noise floor is a few mV, so an exact-equality
 * test would leave `Settled()` permanently false on hardware -- a filter whose
 * callers never classify. */
constexpr int kAdcSettleToleranceMv = 4;

class AdcReader {
public:
    /*
     * Default-constructed as "unbound". This exists so the class can be a value
     * member of a state struct that seeds its members BEFORE the HAL is known --
     * the same pattern PressClassifier and GestureStateMachine already use.
     * `Update` returns false while unbound rather than dereferencing null.
     */
    AdcReader() = default;
    AdcReader(IHAL &hal, AdcChannel ch) : hal_(&hal), ch_(ch) {}

    /** Bind to a HAL and channel. Safe to call on an already-bound reader. */
    void Bind(IHAL &hal, AdcChannel ch)
    {
        hal_ = &hal;
        ch_ = ch;
        Reset();
    }

    /*
     * Take one filtered sample: a burst of `kAdcOversampleCount` conversions,
     * median-combined.
     *
     * Returns false when EVERY read in the burst failed. A failed burst does NOT
     * contribute a sample: `IHAL::adc_read_mv` returns -1 on error, and 0 mV is a
     * LEGAL reading (a button near the ladder's common), so averaging a failure
     * in as zero would invent a phantom press at the bottom of the ladder.
     */
    bool Update(uint64_t now_ms);

    /* The filtered value. On failure this stays at the last good value. */
    MilliVolt Value() const { return value_; }

    /*
     * False while the last burst was a mixture of old and new levels -- i.e. the
     * input was still moving during the window, so its median is a value the input
     * never held. A caller that classifies before this is reading a settling
     * filter.
     *
     * It also goes false on a failed read, because a caller that classifies on a
     * stale value is in exactly the position FR-3 warns about.
     */
    bool Settled() const { return settled_; }

    void Reset();

private:
    IHAL      *hal_ = nullptr;
    AdcChannel ch_ = ADC_CH_SWC1;

    // Fixed, no heap: one allocation at boot and none on the sample path.
    int      window_[kAdcOversampleCount] = {};
    MilliVolt value_ = 0;
    bool     settled_ = false;
};
