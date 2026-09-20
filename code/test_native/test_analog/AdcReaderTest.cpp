// Tests for AdcReader: the two halves of FR-3.
//
// FR-3 is one sentence with two requirements that pull in opposite directions --
// "filter samples for noise while preserving a real button press's edge; the
// filter's settling time MUST be shorter than the configured `debounce_ms`". A
// filter that only smooths passes the first half; one that only tracks edges
// passes neither. So there is a test for each half, and a smoothing-only filter
// cannot pass both.
//
// The suite directory is shared with Task 3's LadderDecode tests; `test_main.cpp`
// already exists and is not duplicated here.

#include "Analog/AdcReader.h"
#include "Gesture/PressClassifier.h"   // GestureTimings, GestureTimingsDefault
#include "HAL/IHAL.h"
#include "MockHAL.h"

#include <gtest/gtest.h>

namespace {

// A HAL double whose ADC value the test drives directly. AdcReader consumes the
// CALIBRATED millivolt interface, so this is the whole surface it needs.
class ScriptedHal {
public:
    IHAL &InterfaceRef() { return iface_; }

    void SetMv(int mv) { mv_ = mv; }

    /** Fail the next `n` conversions, as a bus fault or a saturated input would. */
    void FailNext(int n) { fail_ = n; }

    int Reads() const { return reads_; }

private:
    static int ReadThunk(void *ctx, AdcChannel ch) {
        (void)ch;
        auto *self = static_cast<ScriptedHal *>(ctx);
        ++self->reads_;
        if (self->fail_ > 0) {
            --self->fail_;
            return -1;   // the HAL's error code; 0 mV is a LEGAL reading
        }
        return self->mv_;
    }

    IHAL iface_{};
    int  mv_ = 0;
    int  fail_ = 0;
    int  reads_ = 0;

public:
    ScriptedHal() {
        iface_.ctx = this;
        iface_.adc_read_mv = &ReadThunk;
    }
};

constexpr AdcChannel kCh = ADC_CH_SWC1;

}  // namespace

TEST(AdcReader, AveragesTheOversampleWindowRatherThanReportingOneConversion) {
    // The reported value must be built from a WINDOW, not one conversion: a
    // reader that returned the first sample would still pass a value-only check
    // on a settled input, so this asserts the burst actually happens and the
    // value follows the input.
    ScriptedHal hal;
    hal.SetMv(1400);
    AdcReader r(hal.InterfaceRef(), kCh);
    ASSERT_TRUE(r.Update(0));
    EXPECT_EQ(1400, r.Value());
    EXPECT_GE(hal.Reads(), kAdcOversampleCount)
        << "one conversion is not an oversample window";

    hal.SetMv(1500);
    ASSERT_TRUE(r.Update(10));
    EXPECT_EQ(1500, r.Value()) << "the filter ignored the new level";
}

TEST(AdcReader, TheFilterSettlesWithinTheDebounceWindow) {
    // FR-3's second half, the one a per-poll window silently fails: the filter's
    // settling time MUST be shorter than debounce_ms. Settling is MEASURED here,
    // not asserted -- the test finds the first Update at which the value has
    // reached the new level and compares that elapsed time to the bound.
    ScriptedHal hal;
    const GestureTimings t = GestureTimingsDefault();
    hal.SetMv(2800);
    AdcReader r(hal.InterfaceRef(), kCh);
    r.Update(0);
    r.Update(10);

    hal.SetMv(1400);   // the button is now pressed
    uint64_t settled_at = 0;
    for (uint64_t ms = 20; ms <= t.debounce_ms; ms += 10) {
        r.Update(ms);
        if (r.Settled() && r.Value() <= 1420) {
            settled_at = ms;
            break;
        }
    }
    EXPECT_NE(settled_at, 0u)
        << "the filter did not settle within debounce_ms (" << t.debounce_ms
        << " ms); classification would run on a settling value, which is the "
           "failure FR-3 names";
}

TEST(AdcReader, ARealButtonEdgeIsNotFilteredAway) {
    // FR-3's FIRST half, and the one a heavy smoothing filter fails. A real press
    // is a step of the FULL ladder swing; the filter must let it through rather
    // than average it into mush for tens of milliseconds.
    ScriptedHal hal;
    const int idle = 2800, pressed = 1430;   // spec 3.7's worked example
    hal.SetMv(idle);
    AdcReader r(hal.InterfaceRef(), kCh);
    for (uint64_t ms = 0; ms < 100; ms += 10) r.Update(ms);

    hal.SetMv(pressed);
    // Two calls: the first window is the median of a half-old burst, the second
    // is fully the new level. One call must NOT be "most of the way there".
    for (uint64_t ms = 100; ms < 130; ms += 10) r.Update(ms);
    const int got = static_cast<int>(r.Value());
    EXPECT_LT(got, idle - (idle - pressed) / 2)
        << "a full-ladder press of " << (idle - pressed)
        << " mV was not through the filter after two windows (got " << got << ")";
    EXPECT_EQ(pressed, got) << "the second window should be the new level exactly";
}

TEST(AdcReader, AFailedReadIsReportedAndDoesNotBecomeZeroMillivolts) {
    // IHAL::adc_read_mv returns -1 on error and 0 mV is a LEGAL reading (a button
    // near the ladder's common). A reader that clamped the failure to 0 would
    // invent a phantom press at the bottom of the ladder -- a key fired that the
    // user never pressed.
    ScriptedHal hal;
    hal.SetMv(2800);
    AdcReader r(hal.InterfaceRef(), kCh);
    ASSERT_TRUE(r.Update(0));
    const MilliVolt good = r.Value();

    hal.FailNext(1000);   // every conversion in the next burst fails
    ASSERT_FALSE(r.Update(10)) << "a failed burst must report failure";
    EXPECT_EQ(good, r.Value()) << "a failed read must not overwrite the last good value";
    EXPECT_FALSE(r.Settled()) << "a stale value must not be reported as settled";
}

TEST(AdcReader, ASingleOutlierConversionDoesNotMoveTheResult) {
    // This is the reason for a median rather than a running IIR. An ADC glitch, or
    // one conversion taken while a neighbour switched, is rejected outright -- an
    // IIR would carry it into every sample after it.
    ScriptedHal hal;
    hal.SetMv(1400);
    AdcReader r(hal.InterfaceRef(), kCh);
    ASSERT_TRUE(r.Update(0));
    EXPECT_EQ(1400, r.Value());

    // One wild conversion inside an otherwise-steady burst. The median of 32
    // values with one outlier is still the steady value.
    ScriptedHal spiky;
    spiky.SetMv(1400);
    // A HAL that returns a 1-sample spike every burst.
    struct SpikeHal {
        IHAL iface{};
        int  n = 0;
        static int Read(void *ctx, AdcChannel ch) {
            (void)ch;
            auto *s = static_cast<SpikeHal *>(ctx);
            return (++s->n % kAdcOversampleCount == 0) ? 2900 : 1400;
        }
    } sh;
    sh.iface.ctx = &sh;
    sh.iface.adc_read_mv = &SpikeHal::Read;
    AdcReader rs(sh.iface, kCh);
    ASSERT_TRUE(rs.Update(0));
    EXPECT_EQ(1400, rs.Value())
        << "a single outlier conversion moved the reported level; a mean would "
           "have, which is why this is a median";
}
