#include <unity.h>

#include "DriverHarness.h"

// These assert the SHAPE of a synthetic gesture against the DUT's own limits.
// They are the reason a mistimed press is a failed host test instead of a
// misclassified gesture on the bench -- the timing is arithmetic, and it is
// checked without a board.

using namespace Harness;

static int CountAction(const Timeline &t, Action a)
{
    int n = 0;
    for (int i = 0; i < t.count; ++i) if (t.steps[i].action == a) ++n;
    return n;
}

static void test_a_single_is_a_press_then_the_double_window(void)
{
    const RigConfig c = kDefaultRig;
    const Timeline t = BuildSingle(c);
    // drive, wait(press_on), release, wait(double_window)
    TEST_ASSERT_EQUAL_INT(4, t.count);
    TEST_ASSERT_EQUAL_INT(1, CountAction(t, Action::kDrive));
    TEST_ASSERT_EQUAL_INT(1, CountAction(t, Action::kRelease));
    TEST_ASSERT_EQUAL_UINT32(c.press_on_ms + c.double_window_ms, t.total_ms);
    // The LAST step must be the window wait: a timeline that ended at the release
    // would measure nothing, because the DUT withholds the SINGLE until the
    // window expires.
    TEST_ASSERT_EQUAL_INT((int)Action::kMark, (int)t.steps[t.count - 1].action);
    TEST_ASSERT_EQUAL_UINT32(c.double_window_ms, t.steps[t.count - 1].after_ms);
}

static void test_a_double_is_two_presses_inside_the_window(void)
{
    const RigConfig c = kDefaultRig;
    const Timeline t = BuildDouble(c);
    TEST_ASSERT_EQUAL_INT(2, CountAction(t, Action::kDrive));
    TEST_ASSERT_EQUAL_INT(2, CountAction(t, Action::kRelease));
    // The two presses must be separated by less than the DUT's double window, or
    // they resolve as two SINGLEs.
    TEST_ASSERT_TRUE(c.inter_press_ms < kDutDoubleWindowMs);
    // And the gap between press 1's release and press 2's drive is exactly the
    // inter-press time -- that interval IS the gesture.
    TEST_ASSERT_EQUAL_UINT32(c.inter_press_ms, t.steps[3].after_ms);
    TEST_ASSERT_EQUAL_INT((int)Action::kMark, (int)t.steps[3].action);
}

static void test_a_long_holds_past_the_threshold(void)
{
    const RigConfig c = kDefaultRig;
    const Timeline t = BuildLong(c);
    TEST_ASSERT_EQUAL_INT(1, CountAction(t, Action::kDrive));
    // The drive is held (a wait) for at least the threshold before the release.
    TEST_ASSERT_EQUAL_UINT32(c.long_press_ms, t.steps[1].after_ms);
    TEST_ASSERT_EQUAL_INT((int)Action::kMark, (int)t.steps[1].action);
    TEST_ASSERT_TRUE(t.steps[1].after_ms >= kDutLongPressMs);
}

static void test_the_defaults_are_a_valid_config(void)
{
    TEST_ASSERT_EQUAL_INT((int)RigProblem::kOk, (int)Validate(kDefaultRig));
}

static void test_a_press_shorter_than_the_debounce_is_rejected(void)
{
    RigConfig c = kDefaultRig;
    c.press_on_ms = kDutDebounceMs - 1;
    TEST_ASSERT_EQUAL_INT((int)RigProblem::kPressTooShortForDebounce, (int)Validate(c));
}

static void test_a_press_at_the_debounce_is_accepted(void)
{
    // The boundary is inclusive: exactly the debounce is the shortest press the
    // DUT will see, so it must NOT be rejected.
    RigConfig c = kDefaultRig;
    c.press_on_ms = kDutDebounceMs;
    TEST_ASSERT_EQUAL_INT((int)RigProblem::kOk, (int)Validate(c));
}

static void test_a_press_reaching_the_long_threshold_is_rejected(void)
{
    RigConfig c = kDefaultRig;
    c.press_on_ms = kDutLongPressMs;
    TEST_ASSERT_EQUAL_INT((int)RigProblem::kSinglePressReachesLong, (int)Validate(c));
}

static void test_a_second_press_outside_the_window_is_rejected(void)
{
    RigConfig c = kDefaultRig;
    c.inter_press_ms = kDutDoubleWindowMs;
    TEST_ASSERT_EQUAL_INT((int)RigProblem::kDoubleSecondPressTooLate, (int)Validate(c));
}

static void test_a_second_press_with_no_gap_is_rejected(void)
{
    RigConfig c = kDefaultRig;
    c.inter_press_ms = 0;
    TEST_ASSERT_EQUAL_INT((int)RigProblem::kDoubleSecondPressTooSoon, (int)Validate(c));
}

static void test_the_drive_carries_the_commanded_level(void)
{
    RigConfig c = kDefaultRig;
    c.key_mv = 1550;
    const Timeline t = BuildPress(c);
    TEST_ASSERT_EQUAL_INT(1550, t.steps[0].mv);
    TEST_ASSERT_EQUAL_INT((int)Action::kDrive, (int)t.steps[0].action);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_single_is_a_press_then_the_double_window);
    RUN_TEST(test_a_double_is_two_presses_inside_the_window);
    RUN_TEST(test_a_long_holds_past_the_threshold);
    RUN_TEST(test_the_defaults_are_a_valid_config);
    RUN_TEST(test_a_press_shorter_than_the_debounce_is_rejected);
    RUN_TEST(test_a_press_at_the_debounce_is_accepted);
    RUN_TEST(test_a_press_reaching_the_long_threshold_is_rejected);
    RUN_TEST(test_a_second_press_outside_the_window_is_rejected);
    RUN_TEST(test_a_second_press_with_no_gap_is_rejected);
    RUN_TEST(test_the_drive_carries_the_commanded_level);
    return UNITY_END();
}
