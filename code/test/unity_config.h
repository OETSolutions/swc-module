#ifndef UNITY_CONFIG_H
#define UNITY_CONFIG_H

/*
 * Project Unity configuration.
 *
 * PlatformIO normally GENERATES this file and supplies only output plumbing.
 * Two things it does not supply are needed here:
 *
 *   1. The test-definition macro. Unity 2.6.1 defines neither `TEST` nor a
 *      usable `TEST_CASE` (unity_internals.h defines TEST_CASE as EMPTY unless
 *      UNITY_SUPPORT_TEST_CASES is set, and even then it is the
 *      parameterized-test decorator, not a test definition). The plan's
 *      on-device tests use `TEST(Name, "[tag]")`, which could never compile.
 *   2. A runner. PlatformIO's espidf Unity config provides the output functions
 *      but no `main()`, so the suite links with "undefined reference to
 *      `app_main`".
 *
 * Both are solved with the register-on-construction pattern below: each TEST
 * puts itself in a static list, and one app_main runs the list. That means the
 * test files need no hand-maintained runner list to keep in sync -- a second
 * registry would be another place for the list and the tests to drift.
 *
 * IMPORTANT: PlatformIO looks for this file at `test/unity_config.h` (test_dir
 * root) or `test/<suite>/unity_config.h`. Its presence makes PlatformIO SKIP
 * generating one, so the output plumbing here must be complete -- and it must
 * match the espidf block's signatures exactly, or PlatformIO's own
 * unity_config.c will conflict at link time.
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void unityOutputStart(unsigned long baudrate);
void unityOutputChar(unsigned int c);
void unityOutputFlush(void);
void unityOutputComplete(void);

#define UNITY_OUTPUT_START()      unityOutputStart(115200)
#define UNITY_OUTPUT_CHAR(c)      unityOutputChar(c)
#define UNITY_OUTPUT_FLUSH()      unityOutputFlush()
#define UNITY_OUTPUT_COMPLETE()   unityOutputComplete()

#ifdef __cplusplus
}
#endif

/* --- The test-definition macro ------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*swc_test_fn)(void);

/*
 * The registry lives in the test suite's main file; these are its hooks.
 *
 * `setup`/`teardown` are PER FILE, and that is not decoration -- it is the fix
 * for a link failure. Unity's own runner (`UnityDefaultTestRun` in unity.c)
 * calls the GLOBAL `setUp`/`tearDown`, and this suite is three translation
 * units, each needing its own. Three definitions of one global symbol is a
 * multiple-definition error, so the suite could never link: every "D" row of
 * the spec's coverage matrix was unrunnable. The file's hooks are therefore
 * captured here, at registration, and the runner calls them itself.
 */
void swc_register_test(swc_test_fn fn, const char *name,
                       swc_test_fn setup, swc_test_fn teardown);

#ifdef __cplusplus
}
#endif

/*
 * TEST(SomeName, "[tag]") { body }
 *
 * Expands to the test function plus a constructor that registers it. The
 * name is stringified for the report; the tag is accepted for readability and
 * for a future filter, and is unused today.
 *
 * CONTRACT: the macro references `swc_setup` and `swc_teardown`, so EVERY file
 * that uses TEST must declare both, file-local:
 *
 *     static void swc_setup(void)    { ... }   // runs before each test here
 *     static void swc_teardown(void) { ... }   // runs after each test here
 *
 * They must be `static` (internal linkage) so the three suites do not collide,
 * and they must be named this way -- not `setUp`/`tearDown`, which Unity.h
 * declares as non-static globals and a static redefinition of which is an
 * error. A file with no per-test state still defines both as empty bodies.
 */
#ifndef TEST
#define TEST(name, tag)                                                       \
    static void test_##name(void);                                            \
    __attribute__((constructor)) static void register_##name(void) {          \
        swc_register_test(test_##name, #name, swc_setup, swc_teardown);       \
    }                                                                         \
    static void test_##name(void)
#endif

#endif /* UNITY_CONFIG_H */
