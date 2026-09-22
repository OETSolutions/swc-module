#pragma once

// The gesture timeline: the geometry of a single, double or long press, expressed
// as a list of timed actions the device layer plays out.
//
// WHY THIS IS SEPARATE AND PURE. The timing of a synthetic press is the whole
// ballgame: if the release comes too late it is a LONG, if the second press of a
// double comes after the device's double window it is two SINGLEs, and if the
// hold is shorter than the debounce the device never sees it at all. Those are
// arithmetic facts about millisecond deadlines, and they are exactly the kind of
// thing a host test can assert without a board -- so they live here, in a file
// with no Arduino and no IDF in it, and `test_host/` checks them.
//
// The device layer (src/Driver.cpp) knows how to DRIVE a level; it does not know
// what a double press is. This file knows what a double press is; it does not
// know how to drive anything.

#include <stdint.h>

namespace Harness {

// ---------------------------------------------------------------------------
// The DUT's own gesture limits, mirrored from the product firmware
// (`code/lib/Gesture/PressClassifier.h`). A synthetic press has to straddle
// these, so they live here in the open -- the host test asserts against them and
// the builders are tuned to them. A rig tuned against a GUESS about the DUT is a
// rig that reports the DUT wrong.
// ---------------------------------------------------------------------------
constexpr uint32_t kDutDebounceMs     = 25;
constexpr uint32_t kDutDoubleWindowMs = 500;
constexpr uint32_t kDutLongPressMs    = 750;

// One thing to do, at a moment relative to the previous step.
enum class Action {
    kDrive,      // present `mv` on the KEY line for the selected channel
    kRelease,    // let the KEY line float (the released / idle state)
    kMark,       // a pure wait: the device judges the gesture during it
};

struct Step {
    Action    action;
    uint32_t  after_ms;   // wait this long before performing it (0 = immediately)
    int16_t   mv;         // for kDrive: the target KEY voltage; else unused
};

constexpr int kMaxSteps = 12;

struct Timeline {
    Step     steps[kMaxSteps];
    int      count;
    uint32_t total_ms;    // the sum of every wait; the wall time the sequence takes
};

// ---------------------------------------------------------------------------
// The rig's timing knobs. Defaults mirror the DUT's own defaults (DutTimings.h);
// a config that sets them differently must be changed here to match, or the
// synthetic gesture will not be the gesture it claims to be.
// ---------------------------------------------------------------------------
struct RigConfig {
    int key_mv;              // the learned button's mv_center on the DUT channel
    uint32_t press_on_ms;    // how long a press is held before release
    uint32_t inter_press_ms; // the gap between the two presses of a double
    uint32_t long_press_ms;  // the DUT's long-press threshold (hold at least this)
    uint32_t double_window_ms; // the DUT's double-press window (wait it out)
};

constexpr RigConfig kDefaultRig = {
    /*key_mv=*/1240,
    /*press_on_ms=*/150,
    /*inter_press_ms=*/120,
    /*long_press_ms=*/750,
    /*double_window_ms=*/500,
};

// A press-and-release with nothing before or after: the basic building block.
// Used directly for a "hold to learn" (pass a long press_on_ms) and as the body
// of the compound gestures below.
Timeline BuildPress(const RigConfig &c);

// A single press, followed by the DUT's double-press window so the gesture is
// unambiguous -- a SINGLE is not emitted by the DUT until that window expires,
// so a timeline that ended at the release would measure nothing.
Timeline BuildSingle(const RigConfig &c);

// Two presses inside the double window, then the window itself. The DUT emits
// DOUBLE at the end of it.
Timeline BuildDouble(const RigConfig &c);

// A hold past the long threshold. The DUT emits LONG the moment the threshold
// elapses, so the release that follows is just tidying up.
Timeline BuildLong(const RigConfig &c);

// ---------------------------------------------------------------------------
// Validation. Returns nullptr when the config produces the gesture it claims to
// and a description of the first problem when it does not -- so a typo in the
// timing tables is a message, not a silently-misclassified press.
// ---------------------------------------------------------------------------
enum class RigProblem {
    kOk = 0,
    kPressTooShortForDebounce,
    kSinglePressReachesLong,
    kDoubleSecondPressTooLate,
    kDoubleSecondPressTooSoon,
};

RigProblem Validate(const RigConfig &c);
const char *ProblemName(RigProblem p);

// The action's name, for a log line that reads like the sequence it describes.
const char *ActionName(Action a);

}  // namespace Harness
