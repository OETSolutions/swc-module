#include "DriverHarness.h"

namespace Harness {

namespace {

void Push(Timeline &t, Action a, uint32_t after_ms, int mv)
{
    if (t.count >= kMaxSteps) return;
    t.steps[t.count].action   = a;
    t.steps[t.count].after_ms = after_ms;
    t.steps[t.count].mv       = (int16_t)mv;
    ++t.count;
    t.total_ms += after_ms;
}

Timeline Empty()
{
    Timeline t{};
    t.count    = 0;
    t.total_ms = 0;
    return t;
}

}  // namespace

Timeline BuildPress(const RigConfig &c)
{
    Timeline t = Empty();
    Push(t, Action::kDrive,   0,          c.key_mv);
    Push(t, Action::kMark,    c.press_on_ms, 0);
    Push(t, Action::kRelease, 0,          0);
    return t;
}

Timeline BuildSingle(const RigConfig &c)
{
    Timeline t = Empty();
    Push(t, Action::kDrive,   0,                      c.key_mv);
    Push(t, Action::kMark,    c.press_on_ms,          0);
    Push(t, Action::kRelease, 0,                      0);
    // The DUT withholds the SINGLE until the double window expires, so the wait
    // is part of the gesture, not slack after it.
    Push(t, Action::kMark,    c.double_window_ms,     0);
    return t;
}

Timeline BuildDouble(const RigConfig &c)
{
    Timeline t = Empty();
    Push(t, Action::kDrive,   0,                      c.key_mv);
    Push(t, Action::kMark,    c.press_on_ms,          0);
    Push(t, Action::kRelease, 0,                      0);
    Push(t, Action::kMark,    c.inter_press_ms,       0);
    Push(t, Action::kDrive,   0,                      c.key_mv);
    Push(t, Action::kMark,    c.press_on_ms,          0);
    Push(t, Action::kRelease, 0,                      0);
    Push(t, Action::kMark,    c.double_window_ms,     0);
    return t;
}

Timeline BuildLong(const RigConfig &c)
{
    Timeline t = Empty();
    Push(t, Action::kDrive,   0,                  c.key_mv);
    // Hold to (at least) the threshold. The DUT emits LONG the instant it
    // elapses, so anything beyond this is only the release.
    Push(t, Action::kMark,    c.long_press_ms,    0);
    Push(t, Action::kRelease, 0,                  0);
    return t;
}

RigProblem Validate(const RigConfig &c)
{
    // A press shorter than the debounce is filtered out entirely: the DUT sees
    // nothing, and the rig would report "no gesture" for a healthy device.
    if (c.press_on_ms < kDutDebounceMs) {
        return RigProblem::kPressTooShortForDebounce;
    }
    // A press that reaches the long threshold while claiming to be a single or a
    // double resolves as LONG instead -- the rig would be driving a different
    // gesture than the one it names.
    if (c.press_on_ms >= kDutLongPressMs) {
        return RigProblem::kSinglePressReachesLong;
    }
    // The second press of a double must land inside the window, and after the
    // DUT has actually committed the first release (else the two collapse into
    // one press).
    if (c.inter_press_ms >= kDutDoubleWindowMs) {
        return RigProblem::kDoubleSecondPressTooLate;
    }
    if (c.inter_press_ms == 0) {
        return RigProblem::kDoubleSecondPressTooSoon;
    }
    return RigProblem::kOk;
}

const char *ProblemName(RigProblem p)
{
    switch (p) {
        case RigProblem::kOk:                        return "ok";
        case RigProblem::kPressTooShortForDebounce:  return "press shorter than the DUT debounce";
        case RigProblem::kSinglePressReachesLong:    return "press reaches the long-press threshold";
        case RigProblem::kDoubleSecondPressTooLate:  return "second press lands after the double window";
        case RigProblem::kDoubleSecondPressTooSoon:  return "second press follows the first with no gap";
    }
    return "?";
}

const char *ActionName(Action a)
{
    switch (a) {
        case Action::kDrive:   return "drive";
        case Action::kRelease: return "release";
        case Action::kMark:    return "wait";
    }
    return "?";
}

}  // namespace Harness
