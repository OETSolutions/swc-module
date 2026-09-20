// Unity runner for the on-device suites.
//
// PlatformIO's espidf Unity integration supplies the output functions but no
// entry point, so this file provides `app_main`. It also provides the registry
// that unity_config.h's TEST macro registers into.
//
// The registry exists so the test list is NOT a second thing to maintain: each
// TEST() registers itself from a GCC constructor, and app_main just runs
// whatever registered. A hand-written RUN_TEST(...) list would be a second home
// for "which tests exist", and the two would drift the first time a test was
// added -- the exact defect class this project keeps hitting.

#include "unity.h"

#include <stddef.h>
#include <stdint.h>

// setUp/tearDown are Unity's hooks, called around every test. This suite's
// setUp brings the HAL up once and caches it, because EspHalInit is not
// idempotent (it re-creates the I2C bus and ADC unit) and every test in the
// suite shares one board.
void setUp(void);
void tearDown(void);

typedef void (*swc_test_fn)(void);

#define SWC_MAX_TESTS 64

static struct {
    swc_test_fn fn;
    const char *name;
} s_tests[SWC_MAX_TESTS];
static int s_test_count = 0;

void swc_register_test(swc_test_fn fn, const char *name)
{
    if (s_test_count < SWC_MAX_TESTS) {
        s_tests[s_test_count].fn = fn;
        s_tests[s_test_count].name = name;
        ++s_test_count;
    }
}

void app_main(void)
{
    UNITY_BEGIN();
    for (int i = 0; i < s_test_count; ++i) {
        // UnityDefaultTestRun takes the function, its name and a line number.
        // The line is 0 because the constructor does not know its own line;
        // the name is what the report shows, and it is unique per test.
        UnityDefaultTestRun(s_tests[i].fn, s_tests[i].name, 0);
    }
    UNITY_END();
}
