#include "Output/GestureLevels.h"

#include <gtest/gtest.h>

/*
 * The gesture-level table: the predefined, ascending slots that give a button's
 * SINGLE, DOUBLE and LONG three DISTINCT voltages for a gesture-blind head unit
 * (spec §6.6 rule 4 / §7.5). These are pure functions of the slot and the head
 * unit's measured idle, so they are pinned here at the unit level.
 */

TEST(GestureLevels, TheTableIsStrictlyAscendingSoNoTwoSlotsCollide) {
    // The whole point of "coded beforehand ... reliably without any overlapping":
    // every slot must sit ABOVE its neighbour, or two gestures would map to the
    // same command. A non-ascending table is what the user asked to be avoided.
    for (int i = 1; i < kGestureSlotCount; ++i) {
        EXPECT_GT(kGestureSlotPermille[i], kGestureSlotPermille[i - 1])
            << "slot " << i << " is not above slot " << (i - 1);
    }
}

TEST(GestureLevels, TheThreeGesturesOfOneButtonGetThreeAdjacentDistinctSlots) {
    // SINGLE < DOUBLE < LONG for the SAME button, and the three slots are
    // consecutive -- so one button's gestures are as close as the table allows,
    // leaving room for as many other buttons as possible.
    const int s = GestureSlotIndex(0, Gesture::kSingle);
    const int d = GestureSlotIndex(0, Gesture::kDouble);
    const int l = GestureSlotIndex(0, Gesture::kLong);
    EXPECT_LT(s, d);
    EXPECT_LT(d, l);
    EXPECT_EQ(d, s + 1);
    EXPECT_EQ(l, s + 2);
}

TEST(GestureLevels, DifferentButtonsDoNotShareASlot) {
    // Button 1's SINGLE and button 0's DOUBLE must be different slots: two
    // different buttons must be distinguishable.
    EXPECT_NE(GestureSlotIndex(0, Gesture::kDouble), GestureSlotIndex(1, Gesture::kSingle));
    EXPECT_EQ(GestureSlotIndex(1, Gesture::kSingle), GestureSlotIndex(0, Gesture::kSingle) + 3);
}

TEST(GestureLevels, AGestureWithNoSlotIsRefusedRatherThanDefaulted) {
    EXPECT_EQ(GestureSlotIndex(0, Gesture::kNone), -1)
        << "kNone has no slot; a caller must not silently get slot 0";
    // A button ordinal past the table's capacity has no slot either, so the caller
    // falls back to the present-or-unknown path rather than a colliding level.
    EXPECT_EQ(GestureSlotIndex(kGestureSlotCount, Gesture::kSingle), -1);
    EXPECT_EQ(GestureSlotIndex(0xFE, Gesture::kLong), -1);
}

TEST(GestureLevels, ASlotAlwaysLandsInsideTheCommandBand) {
    // A 5 V head unit: idle 4980, so the band is [1800, 4780].
    const int head_idle = 4980;
    const int floor_mv = kOutputFloorMv;
    const int ceil_mv  = head_idle - kCommandHeadroomMv;
    for (int slot = 0; slot < kGestureSlotCount; ++slot) {
        const int mv = GestureSlotLevelMv(slot, head_idle);
        EXPECT_GE(mv, floor_mv) << "slot " << slot << " is below the servo floor";
        EXPECT_LE(mv, ceil_mv) << "slot " << slot << " is above the head unit's rest";
    }
}

TEST(GestureLevels, ASlotScalesWithTheHeadUnitIdleSoAThreeVoltUnitStillWorks) {
    // The reason the table is FRACTIONS, not millivolts: a smaller head-unit idle
    // must still fit every slot below its own rest. A 3.5 V head unit.
    const int head_idle_5v = 4980;
    const int head_idle_3v = 3500;
    // The same slot maps to a LOWER millivolt value on the lower-idle head unit.
    const int a = GestureSlotLevelMv(10, head_idle_5v);
    const int b = GestureSlotLevelMv(10, head_idle_3v);
    EXPECT_LT(b, a) << "the slot must scale with the head unit's range";
    // And it still fits the smaller band.
    EXPECT_LE(b, head_idle_3v - kCommandHeadroomMv);
    EXPECT_GE(b, kOutputFloorMv);
}

TEST(GestureLevels, AnEmptyCommandBandYieldsZeroSoTheCallerReleases) {
    // A head unit idling below floor + headroom has no reachable command; the
    // function must say so with 0, the "absent" the output code uses everywhere,
    // rather than fabricating a level.
    EXPECT_EQ(GestureSlotLevelMv(0, kOutputFloorMv), 0);
    EXPECT_EQ(GestureSlotLevelMv(0, 1000), 0);
    EXPECT_EQ(GestureSlotLevelMv(0, 0), 0);
}

TEST(GestureLevels, AnUnknownSlotYieldsZero) {
    EXPECT_EQ(GestureSlotLevelMv(-1, 4980), 0);
    EXPECT_EQ(GestureSlotLevelMv(kGestureSlotCount, 4980), 0);
}
