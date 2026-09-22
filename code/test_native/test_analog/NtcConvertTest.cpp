#include <gtest/gtest.h>

#include <cmath>

#include "Analog/NtcConvert.h"

/*
 * FR-1's first clause -- "sample both ladder channels AND the NTC continuously" --
 * was unimplemented, and open item N-67 recorded that the raw millivolts would not
 * be a temperature even if the channel were read: there was no B3380 routine, no
 * divider inversion, nothing.
 *
 * This is that conversion. The expected values below are computed from the
 * DATASHEET model (`1/T = 1/T0 + ln(R/R0)/B`, R0 = 10k at 25 C, B = 3380) applied
 * to the divider read off the schematic (+3V3 -> R29 10k -> node -> RT1 -> GND),
 * NOT from this implementation -- so a sign error or a swapped divider side fails
 * here rather than cancelling out.
 */

TEST(NtcConvert, InvertsTheDividerWithThePartOnTheLowSide) {
    // The schematic's topology, not a guess: RT1 is from the node to GND and R29
    // from +3V3 to the node. So an EQUAL resistance puts the node at half the
    // rail, and the node RISES as the part gets hotter. Getting the side wrong
    // reverses the sign of the whole control law, so it is asserted directly.
    int ohms = 0;
    ASSERT_TRUE(Ntc::ResistanceFromMv(1650, 3300, &ohms));
    EXPECT_EQ(ohms, 10000) << "half the rail means equal resistances";

    // Hotter -> lower resistance -> LOWER node voltage.
    int hot = 0, cold = 0;
    ASSERT_TRUE(Ntc::ResistanceFromMv(200, 3300, &hot));
    ASSERT_TRUE(Ntc::ResistanceFromMv(2896, 3300, &cold));
    EXPECT_LT(hot, cold) << "a hotter NTC has less resistance";
}

TEST(NtcConvert, ReportsTwentyFiveCAtThePartsNominalResistance) {
    // The anchor every B-constant model is pinned to. If this is wrong, every
    // other value is wrong by the same offset and nothing downstream would see it.
    int t = 0;
    ASSERT_TRUE(Ntc::ConvertTenthsC(10000, &t));
    EXPECT_EQ(t, 250);
}

TEST(NtcConvert, MatchesTheDatasheetModelAcrossTheAutomotiveRange) {
    // The whole point of the conversion. Each node voltage is the one the divider
    // produces at that temperature; the expectation is the datasheet model, so an
    // implementation error shows as a disagreement rather than as agreement with
    // itself. Tolerance is 0.5 C: the fixed-point series is accurate to ~0.25 C
    // and this asserts the MODEL, not the arithmetic's last bit.
    struct Case { int node_mv; double expected_c; };
    const Case cases[] = {
        {2896, -19.13},   // cold end of the cabin range
        {2437,   0.00},
        {1650,  25.00},
        { 970,  49.98},
        { 430,  84.97},
    };
    for (const Case &c : cases) {
        int t = 0;
        ASSERT_TRUE(Ntc::NodeMvToTenthsC(c.node_mv, 3300, &t))
            << "node " << c.node_mv << " mV";
        EXPECT_NEAR(t / 10.0, c.expected_c, 0.5)
            << "node " << c.node_mv << " mV vs the datasheet model";
    }
}

TEST(NtcConvert, TheEndToEndValueAndTheSplitChainAgree) {
    // `NodeMvToTenthsC` is the composition; asserting they agree catches a caller
    // wiring only half the chain (the exact shape of N-67: the division without
    // the conversion).
    int ohms = 0, direct = 0, split = 0;
    ASSERT_TRUE(Ntc::ResistanceFromMv(970, 3300, &ohms));
    ASSERT_TRUE(Ntc::ConvertTenthsC(ohms, &direct));
    ASSERT_TRUE(Ntc::NodeMvToTenthsC(970, 3300, &split));
    EXPECT_EQ(direct, split);
}

TEST(NtcConvert, TheRailVoltageIsAnInputBecauseTheDividerScalesWithIt) {
    // A 3.25 V board puts the node LOWER for the same temperature, so using the
    // 3.3 V nominal would read it cold. The error is small but real and
    // systematic, which is why the function takes the rail rather than inlining a
    // constant.
    int on_nominal = 0, on_low_rail = 0;
    ASSERT_TRUE(Ntc::NodeMvToTenthsC(1650, 3300, &on_nominal));
    // The same physical temperature on a 3.25 V rail:
    const int node_at_low_rail = static_cast<int>(3250.0 * 10000 / (10000 + 10000));
    ASSERT_TRUE(Ntc::NodeMvToTenthsC(node_at_low_rail, 3250, &on_low_rail));
    EXPECT_EQ(on_nominal, on_low_rail)
        << "the SAME node/rail RATIO is the same temperature";
}

TEST(NtcConvert, RefusesARailOrAnNodeVoltageItCannotInvert) {
    // At or above the rail the inversion is undefined (VDD - V <= 0), and past it
    // the arithmetic would produce a NEGATIVE resistance that still passes any
    // magnitude check -- a fabricated temperature from a shorted or open part.
    int t = 12345;
    EXPECT_FALSE(Ntc::ResistanceFromMv(3300, 3300, &t)) << "node == rail: an open NTC";
    EXPECT_FALSE(Ntc::ResistanceFromMv(3500, 3300, &t)) << "node above the rail is not a divider at all";
    EXPECT_FALSE(Ntc::ResistanceFromMv(0, 3300, &t)) << "a shorted NTC";
    EXPECT_FALSE(Ntc::ResistanceFromMv(-1, 3300, &t)) << "the ADC error sentinel (N-43)";
    EXPECT_FALSE(Ntc::ResistanceFromMv(1650, 0, &t)) << "no rail reference";
    EXPECT_FALSE(Ntc::ResistanceFromMv(1650, 3300, nullptr));
    // The refusal is the point, and it must be a REFUSAL: a caller that got a
    // value here would report a temperature the hardware cannot produce.
    EXPECT_FALSE(Ntc::NodeMvToTenthsC(3300, 3300, &t));
}

TEST(NtcConvert, RefusesAResistanceOutsideThePartsPlausibleSpan) {
    // A sanity gate, not a spec: it must never catch a real reading, so the bounds
    // are wide. What it exists for is an arithmetic runaway or a wildly wrong
    // resistance mapping to a temperature nothing could have.
    int t = 0;
    EXPECT_FALSE(Ntc::ConvertTenthsC(10, &t)) << "a shorted part";
    EXPECT_FALSE(Ntc::ConvertTenthsC(5000000, &t)) << "an open part";
    EXPECT_FALSE(Ntc::ConvertTenthsC(0, &t));
    EXPECT_FALSE(Ntc::ConvertTenthsC(10000, nullptr));
    // And the extremes it DOES accept stay inside the stated span.
    ASSERT_TRUE(Ntc::ConvertTenthsC(235831, &t));
    EXPECT_NEAR(t / 10.0, -40.0, 1.0);
    ASSERT_TRUE(Ntc::ConvertTenthsC(646, &t));
    EXPECT_NEAR(t / 10.0, 120.0, 1.0);
}

TEST(NtcConvert, NeverOverflowsItsSixtyFourBitIntermediatesAtTheExtremes) {
    // The series raises a scaled `y` to the 61st power; the bound that makes that
    // safe is |y| < 2^16, which holds because |y| < 1 by construction. This walks
    // the ends of the accepted range, where |y| is largest, and asserts the result
    // stays in the plausible band -- an overflow would wrap to a wild value, not
    // to a refusal.
    for (int ohms : {Ntc::kMinOhms, 50, Ntc::kNominalOhms - 9999, Ntc::kNominalOhms,
                     Ntc::kMaxOhms, 999999}) {
        int t = 0;
        if (!Ntc::ConvertTenthsC(ohms, &t)) continue;
        EXPECT_GE(t, -400);
        EXPECT_LE(t, 1500) << "ohms=" << ohms << " produced " << t;
    }
}
