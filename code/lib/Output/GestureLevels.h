#pragma once

#include <stdint.h>

#include "Gesture/GestureStateMachine.h"
#include "Output/GainPolicy.h"

/*
 * The headless GESTURE LEVELS (spec §6.6 rule 4, §7.5).
 *
 * **The problem this solves.** The head unit is gesture-blind: it reads a voltage
 * and matches key windows. A wheel button is one resistor, so a plain press, a
 * double press and a long press all present the SAME level to the head unit
 * unless the adapter makes them differ. The 2022 design made them differ with
 * `lookup_single/double/long_press_val`, which divided the key's own value into
 * thirds of an output range. This is the same idea with the range replaced by a
 * fixed, ascending table of SLOTS, because -- as the user put it -- "we won't
 * always know ahead of time how many switches there are", and a table can be
 * sized and spaced once instead of re-derived per button.
 *
 * **Why it needs no storage, unlike a binding table.** The level a
 * `(button, gesture)` presents is a PURE FUNCTION of the button's ordinal
 * position and the gesture -- the same property the 2022 design relied on, and
 * the reason its `program_alt_key` could be an empty stub. The head unit is what
 * remembers "this voltage means this function", taught once by the programming
 * hold (§7.5); the adapter only has to be deterministic so it sends the SAME
 * level every time. Nothing here changes `sizeof(Config)` or the NVS budget.
 *
 * **Why the slots are fractions of the command band, not absolute millivolts.**
 * A fixed mV table cannot serve both a 5 V and a 3 V head unit: the command band
 * is `[kOutputFloorMv, V_KEY_idle - kCommandHeadroomMv]`, and a 3 V head unit's
 * band tops out far below a 5 V one's, so a fixed table would fold several slots
 * onto the same clamped level -- colliding exactly the gestures this exists to
 * separate. The table therefore holds each slot's position as a fraction of the
 * LIVE band, and `GestureSlotLevelMv` maps it in. The fractions are the predefined,
 * hand-checked part; the volts are derived per head unit.
 *
 * **The table is FINE on purpose, and the user climbs it empirically.** The
 * user's instruction: "Programming should use slot starting with lower voltages
 * and up from there since we don't always know where the cutoff is between switch
 * signals and idle voltage for a head unit." A head unit's real key-window count
 * is not knowable in advance, so a coarse table would either waste resolution or
 * offer too few switches. Instead the table is spaced finer than most head units
 * can resolve; the user programs gestures from the bottom up and stops when two
 * land on the same head-unit function -- which is how the usable count is
 * DISCOVERED rather than assumed. Climbing from the floor also keeps every slot as
 * far as possible from the head unit's own idle, the boundary that is uncertain; a
 * slot that strayed past it would reach the radio as no key at all (the output
 * only sinks).
 */

// 16 learned buttons (kLadderMaxButtons) x 3 gestures. The table is walked from
// the bottom, so a device with fewer buttons simply uses the first 3*N slots and
// gets WIDER spacing -- the spacing is a property of how many slots are in use,
// not of the table's length.
constexpr int kGestureSlotCount = 48;

/*
 * The predefined slot table: each slot's position in permille of the command
 * band, measured UP from the band's floor.
 *
 * Ascending by construction and evenly spaced 20 permille (~2 % of the band)
 * apart, so with only a few buttons in use the neighbours are far wider than that
 * -- the step is a property of how many slots a device uses, not of the table's
 * length. The floor of the table is 20 permille rather than 0 so slot 0 is not
 * pinned exactly to `kOutputFloorMv` -- a slot AT the servo's floor has no margin
 * for the loop's own settling.
 *
 * The values are written out rather than generated so the spacing can be reviewed
 * as a table, which is the point of "coded beforehand". If the count or the range
 * changes, re-check the spacing against the head unit's key-window width before
 * widening `kGestureSlotCount`.
 */
inline constexpr int kGestureSlotPermille[kGestureSlotCount] = {
     20,  40,  60,  80, 100, 120, 140, 160,
    180, 200, 220, 240, 260, 280, 300, 320,
    340, 360, 380, 400, 420, 440, 460, 480,
    500, 520, 540, 560, 580, 600, 620, 640,
    660, 680, 700, 720, 740, 760, 780, 800,
    820, 840, 860, 880, 900, 920, 940, 960,
};

// The gesture's ordinal within a button's three slots: SINGLE is the lowest,
// then DOUBLE, then LONG. Ascending so a "bigger" gesture is a higher level, the
// same direction the 2022 `lookup_*_press_val` family used. A gesture with no
// slot (`kNone`) returns -1 so a caller must handle it rather than silently
// presenting slot 0.
constexpr int GestureSlotOrdinal(Gesture g) {
    switch (g) {
        case Gesture::kSingle: return 0;
        case Gesture::kDouble: return 1;
        case Gesture::kLong:   return 2;
        default:               return -1;
    }
}

/*
 * The slot index a `(button ordinal, gesture)` pair owns, or -1 when the pair has
 * no slot (an unknown gesture, or a button ordinal past the table).
 *
 * `button_ordinal` is the button's position in its channel's learned ladder, NOT
 * its level -- the level moves with the rail, the ordinal does not. A ladder with
 * more buttons than the table admits simply stops getting slots at the top; those
 * buttons fall back to the caller's present-or-unknown path rather than being
 * given a colliding level.
 */
constexpr int GestureSlotIndex(int button_ordinal, Gesture g) {
    const int ord = GestureSlotOrdinal(g);
    if (ord < 0 || button_ordinal < 0) return -1;
    const int slot = button_ordinal * 3 + ord;
    return (slot < kGestureSlotCount) ? slot : -1;
}

/*
 * The KEY voltage, in millivolts, that a slot asks for against a head unit whose
 * measured idle is `head_unit_idle_mv`.
 *
 * Returns 0 -- the "absent" used everywhere in the output code -- when there is
 * no slot, or when the command band is empty (`head_unit_idle_mv` below
 * `kOutputFloorMv + kCommandHeadroomMv`). 0 means "nothing to present", and the
 * caller releases rather than driving a level it cannot justify, the same
 * direction `GainPolicyClampCommand` and FR-12 take.
 *
 * The mapping is `floor + (ceiling - floor) * permille / 1000`, then clamped into
 * the band so a table that had drifted past either edge still lands on a
 * reachable level.
 */
int GestureSlotLevelMv(int slot, int head_unit_idle_mv);
