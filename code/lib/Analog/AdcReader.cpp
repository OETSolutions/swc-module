#include "Analog/AdcReader.h"



namespace {

/*
 * The median of the burst.
 *
 * A selection sort over 32 ints -- ~500 comparisons per Update, at a ≤100 Hz
 * call rate, which is well inside the loop budget. The point is that the RESULT
 * is order-independent: one outlying conversion cannot move it the way it moves a
 * mean, which is the whole reason for choosing a median.
 */
MilliVolt MedianOf(const int *v, int n) {
    int tmp[kAdcOversampleCount];
    for (int i = 0; i < n; ++i) tmp[i] = v[i];
    for (int i = 0; i < n - 1; ++i) {
        int m = i;
        for (int j = i + 1; j < n; ++j) {
            if (tmp[j] < tmp[m]) m = j;
        }
        if (m != i) {
            const int t = tmp[i];
            tmp[i] = tmp[m];
            tmp[m] = t;
        }
    }
    // Even count: the LOWER of the two middle values, not their mean. A mean
    // would be a fractional value no conversion produced, and it would make the
    // reported level depend on arithmetic rather than on a real sample -- which
    // matters because this value is compared against learned windows.
    return static_cast<MilliVolt>(tmp[n / 2]);
}

}  // namespace

bool AdcReader::Update(uint64_t now_ms) {
    (void)now_ms;   // the burst is per-call; see the header
    if (hal_ == nullptr) return false;   // unbound; see the default constructor

    int  got = 0;
    for (int i = 0; i < kAdcOversampleCount; ++i) {
        const int raw = hal_->adc_read_mv(hal_->ctx, ch_);
        if (raw < 0) continue;   // a single glitchy conversion is dropped, not fatal
        window_[got++] = raw;
    }

    if (got == 0) {
        // The whole burst failed. Report it and keep the last good value: the
        // HAL's -1 is not a reading, and 0 mV is a legal one, so substituting
        // either would be inventing data.
        settled_ = false;
        return false;
    }

    const MilliVolt v = MedianOf(window_, got);
    value_ = v;

    // Settled means the BURST itself is not a mixture of old and new. That is the
    // physically meaningful condition and the one FR-3 needs: a window taken while
    // the input is still moving spans both levels, so its median is a value the
    // input never actually held. A uniform burst -- steady, or already stepped --
    // is settled immediately, which is why settling costs ONE window and fits
    // inside debounce_ms.
    //
    // An exact-equality test would be wrong on hardware: a real ADC's noise floor
    // is a few mV, so the spread tolerance is what keeps `Settled()` from being
    // permanently false.
    int lo = window_[0], hi = window_[0];
    for (int i = 1; i < got; ++i) {
        if (window_[i] < lo) lo = window_[i];
        if (window_[i] > hi) hi = window_[i];
    }
    settled_ = (hi - lo) <= kAdcSettleToleranceMv;
    return true;
}

void AdcReader::Reset() {
    for (int i = 0; i < kAdcOversampleCount; ++i) window_[i] = 0;
    value_ = 0;
    settled_ = false;
}
