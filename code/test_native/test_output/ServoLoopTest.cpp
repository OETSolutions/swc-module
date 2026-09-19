#include "Output/ServoLoop.h"
#include <gtest/gtest.h>

namespace {
// A plant model: sense = code-derived key volts / 2, with a 3% gain error that
// the loop must correct for.
class Plant {
public:
    explicit Plant(int gain_error_permille) : err_(gain_error_permille) {}
    int SenseMv(uint16_t code) {
        return (GainPolicyKeyMvForCode(GainMode::kAmplified, code) * err_) / 1000 / 2;
    }
private:
    int err_;
};
}  // namespace

TEST(ServoLoop, CorrectsAThreePercentGainErrorWithinTheCodeBudget) {
    Plant plant(1030);
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 4000);
    for (int i = 0; i < 40; ++i) loop.Update(plant.SenseMv(loop.Code()));
    EXPECT_TRUE(loop.Settled());
    // The sense pin sees target/2; allow the deadband.
    EXPECT_NEAR(plant.SenseMv(loop.Code()), 2000, 20);
}

TEST(ServoLoop, NeverMovesMoreThanMaxStepPerUpdate) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 5000);
    const uint16_t before = loop.Code();
    loop.Update(0);  // measured far below target
    const int moved = std::abs(static_cast<int>(loop.Code()) - static_cast<int>(before));
    EXPECT_LE(moved, ServoConfigDefault().max_step_codes);
}

TEST(ServoLoop, StopsAdjustingInsideTheDeadband) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 4000);
    for (int i = 0; i < 20; ++i) loop.Update(2000);  // exactly on target
    const uint16_t settled_code = loop.Code();
    loop.Update(2005);  // inside the 20mV deadband
    EXPECT_EQ(loop.Code(), settled_code);
}

TEST(ServoLoop, DoesNotOscillateEvenWithAMeasuredOvershoot) {
    // Feed the loop alternating readings that BRACKET the target from outside
    // the deadband. An unbounded integrator would ring; a bounded one must stay
    // inside its authority band and never leave it.
    //
    // The readings must be further than deadband_mv (20) from target/2 = 2000.
    // 1990/2010 are only +-10mV -- INSIDE the deadband -- so the loop never
    // moves and this test would pass with the step cap deleted. Use +-100mV.
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 4000);
    const uint16_t base = GainPolicyCodeForTarget(GainMode::kAmplified, 4000).dac_code;
    bool moved = false;
    for (int i = 0; i < 200; ++i) {
        loop.Update((i % 2) ? 1900 : 2100);
        if (loop.Code() != base) moved = true;
        const int drift = std::abs(static_cast<int>(loop.Code()) - static_cast<int>(base));
        ASSERT_LE(drift, ServoConfigDefault().max_total_codes)
            << "left the authority band at i=" << i;
    }
    // Bounded, but it MUST have trimmed: a loop that never moves trivially
    // satisfies the bound above. Note the final code returns to `base` by
    // parity (the last update is odd -> 1900), so assert on movement seen
    // during the run, not on where it happened to stop.
    EXPECT_TRUE(moved) << "the loop must have trimmed, not sat still";
    const uint16_t code_a = loop.Code();
    for (int i = 0; i < 20; ++i) loop.Update(2000);
    EXPECT_EQ(loop.Code(), code_a) << "must not keep moving once inside the deadband";
}

TEST(ServoLoop, RespectsTheTotalCodeBudget) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 5000);
    for (int i = 0; i < 10000; ++i) loop.Update(0);  // never reaches target
    const int base = GainPolicyCodeForTarget(GainMode::kAmplified, 5000).dac_code;
    const int drift = std::abs(static_cast<int>(loop.Code()) - base);
    EXPECT_LE(drift, ServoConfigDefault().max_total_codes)
        << "an unreachable target must not walk the code to an extreme";
}

TEST(ServoLoop, IsInertInTrackingModeBecauseTheNodeAlreadyTracks) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kTracking, 4000);
    const uint16_t c = loop.Code();
    for (int i = 0; i < 50; ++i) loop.Update(100);  // absurd reading
    EXPECT_EQ(loop.Code(), c) << "tracking mode needs no trim; correcting it would fight the servo";
}

TEST(ServoLoop, ResetReturnsToTheOpenLoopCode) {
    ServoLoop loop(ServoConfigDefault());
    loop.Target(GainMode::kAmplified, 4000);
    for (int i = 0; i < 10; ++i) loop.Update(1000);
    loop.Reset();
    EXPECT_EQ(loop.Code(), GainPolicyCodeForTarget(GainMode::kAmplified, 4000).dac_code);
    EXPECT_FALSE(loop.Settled());
}
