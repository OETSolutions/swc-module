#pragma once

#include <stdint.h>

// Ratios are permille (thousandths of the rail) so the whole comparison path is
// integer arithmetic. Floating point on this target is slower and the windows
// are generous enough (tens of permille) that integer rounding is irrelevant.
constexpr int kLadderMaxButtons = 16;
constexpr int kLadderIdLen = 24;

struct LadderButton {
    char    id[kLadderIdLen];      // stable identity for bindings
    int16_t ratio_permille;        // measured centre
    int16_t tolerance_permille;    // half-width of the accept window
    uint8_t action_id;             // resolved elsewhere; opaque here
};

struct LadderProfile {
    LadderButton buttons[kLadderMaxButtons];
    uint8_t      count;
    int          learned_idle_mv;   // the idle reading at learn time (spec 3.4)
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
