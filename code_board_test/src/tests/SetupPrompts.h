#pragma once

// The manual-setup hooks, declared once for the registry.
//
// A hook returns a C string when the operator must do something before the body
// runs -- fit a loopback jumper, or attach a meter and tell the test it is there.
// The runner prints it; the web UI shows it against the test row.
//
// Kept in this small header rather than in TestDecls.h so the two lists have one
// job each: TestDecls.h pairs the registry with the test BODIES, this pairs it
// with the SETUP prompts. A test with no prompt passes nullptr and that is the
// common case.

namespace SwcTests {

// Test 8 asks the operator whether a jumper is available, because the test can run
// either way and says which result it is reporting.
const char *Setup08_AdcChannels();

// Test 23 wants a pull-up fitted on J3.3 for its part B. Without it parts A and C
// still run, so this is a request rather than a requirement.
const char *Setup23_IdleSafety();

}  // namespace SwcTests
