#include "Output/GestureLevels.h"

int GestureSlotLevelMv(int slot, int head_unit_idle_mv) {
    if (slot < 0 || slot >= kGestureSlotCount) return 0;

    // The command band, spec 6.2's `[kOutputFloorMv, V_KEY_idle - headroom]`.
    // Built here from the same two constants `GainPolicyClampCommand` uses, so
    // the "is there room to command" question has ONE definition; the clamp below
    // is what actually enforces it.
    const int floor_mv = kOutputFloorMv;
    const int ceil_mv  = head_unit_idle_mv - kCommandHeadroomMv;
    // An empty band: no slot can be reached below the head unit's own rest.
    // Returning 0 is the caller's "nothing to present" -- release, do not guess.
    if (ceil_mv < floor_mv) return 0;

    const int span_mv = ceil_mv - floor_mv;
    const int permille = kGestureSlotPermille[slot];
    // `span_mv` is positive and bounded by the envelope (~3.4 V), and `permille`
    // by 1000, so the product is ~3.4e6 -- well inside `int`. Widened anyway so a
    // future wider envelope cannot quietly overflow the multiply.
    int level_mv = floor_mv +
                   static_cast<int>((static_cast<int64_t>(span_mv) * permille) / 1000);

    // Clamp into the band. The table is ascending and bounded today, so this is
    // belt-and-braces -- but it is the guard that keeps a future table edit from
    // driving a level the servo cannot reach, which is the phantom/absent-key
    // hazard the output path exists to avoid.
    if (level_mv < floor_mv) level_mv = floor_mv;
    if (level_mv > ceil_mv)  level_mv = ceil_mv;
    return level_mv;
}
