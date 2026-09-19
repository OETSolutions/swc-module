#include "Analog/LadderDecode.h"

#include <stdlib.h>

namespace {
// The margin either side of the idle reference. Above idle+margin the reading
// exceeds the reference, which is a short to a higher supply rather than a
// button or an idle.
constexpr int16_t kIdleMarginPermille = 30;
// The idle reading has collapsed relative to the one learned. Expressed
// against the LEARNED idle, not the ratio: ratio normalization deliberately
// cancels rail movement out of the ratios, so a dead supply looks perfectly
// normal to it. This is the check that catches that (FR-30).
constexpr int16_t kRailHealthFloorPermille = 200;
}  // namespace

int16_t LadderRatioPermille(int level_mv, int idle_mv) {
    if (idle_mv <= 0) return -1;
    const long long scaled = (static_cast<long long>(level_mv) * 1000LL + idle_mv / 2) / idle_mv;
    if (scaled > 32767) return 32767;
    if (scaled < -32768) return -32768;
    return static_cast<int16_t>(scaled);
}

ClassifyOutcome LadderClassify(const LadderProfile &profile, int level_mv, int idle_mv) {
    ClassifyOutcome out{ClassifyResult::kFault, 0, 0};
    if (idle_mv <= 0) return out;

    const int16_t ratio = LadderRatioPermille(level_mv, idle_mv);
    out.ratio_permille = ratio;

    // FR-30: a 3V3 sag to <=20% of the learned idle is a rail fault. Checked
    // before anything else, because at that level every ratio is garbage.
    if (profile.learned_idle_mv > 0 &&
        idle_mv < (profile.learned_idle_mv * kRailHealthFloorPermille) / 1000) {
        return out;                                                    // reference collapsed
    }

    if (ratio < 0 || ratio > 1000 + kIdleMarginPermille) return out;   // above the reference

    if (ratio >= 1000 - kIdleMarginPermille) {
        out.result = ClassifyResult::kIdle;
        return out;
    }

    // Nearest-centre match, so two overlapping windows resolve deterministically
    // to whichever button the user actually pressed rather than to array order.
    int best = -1;
    int best_distance = 0;
    for (uint8_t i = 0; i < profile.count && i < kLadderMaxButtons; ++i) {
        const int centre = profile.buttons[i].ratio_permille;
        const int half   = profile.buttons[i].tolerance_permille;
        const int distance = abs(ratio - centre);
        if (distance > half) continue;
        if (best < 0 || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }

    if (best >= 0) {
        out.result = ClassifyResult::kButton;
        out.index = static_cast<uint8_t>(best);
    } else {
        out.result = ClassifyResult::kUnknown;
    }
    return out;
}
