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

// Per-file setUp/tearDown are passed in at registration (see unity_config.h).
// This runner calls them itself rather than relying on Unity's global
// `setUp`/`tearDown`: those are ONE symbol each, and this suite is three
// translation units with three different per-test needs, so three definitions
// collided at link time and the whole device suite was unrunnable. Unity *does*
// invoke the globals (`UnityDefaultTestRun`, unity.c:2201), so the hooks must
// still exist -- this file's are no-ops below -- but the per-file work happens
// through these captured pointers.
typedef void (*swc_test_fn)(void);

#define SWC_MAX_TESTS 64

static struct {
    swc_test_fn fn;
    const char *name;
    swc_test_fn setup;
    swc_test_fn teardown;
} s_tests[SWC_MAX_TESTS];
static int s_test_count = 0;

void swc_register_test(swc_test_fn fn, const char *name,
                       swc_test_fn setup, swc_test_fn teardown)
{
    if (s_test_count < SWC_MAX_TESTS) {
        s_tests[s_test_count].fn = fn;
        s_tests[s_test_count].name = name;
        s_tests[s_test_count].setup = setup;
        s_tests[s_test_count].teardown = teardown;
        ++s_test_count;
    }
}

// The single global hooks Unity's runner insists on. The real per-file work is
// in the captured pointers, invoked in app_main around each test.
void setUp(void) {}
void tearDown(void) {}

void app_main(void)
{
    UNITY_BEGIN();
    for (int i = 0; i < s_test_count; ++i) {
        if (s_tests[i].setup != NULL) s_tests[i].setup();
        // UnityDefaultTestRun takes the function, its name and a line number.
        // The line is 0 because the constructor does not know its own line;
        // the name is what the report shows, and it is unique per test.
        UnityDefaultTestRun(s_tests[i].fn, s_tests[i].name, 0);
        if (s_tests[i].teardown != NULL) s_tests[i].teardown();
    }
    UNITY_END();
}
