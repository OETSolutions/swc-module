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

// Test 31 walks the operator through shorting each AUX input to GND in turn, so the
// board can prove each one is a live analog path rather than a stuck node.
const char *Setup31_AuxManual();

// Test 32 has the operator confirm the RT1 reading against a known temperature, and
// optionally hold the sensor to change it -- the only way to verify a thermistor's
// SCALE without a calibrated chamber.
const char *Setup32_TempVerify();

// A hook the front end installs so an operator prompted by a test can continue from
// EITHER console. Tests 31 and 32 wait for the operator; on the serial side that is a
// keystroke, but the web UI has no keystroke.
//
// The hook SERVES the web server (it is a pump, not a poll) and returns true when a
// request has arrived. Pumping is the load-bearing part: while a test is blocked in
// this wait the HTTP server is otherwise never serviced, so the page could not even
// be refreshed -- which is why a web-UI run of these tests appeared to hang with no
// activity at all, and why they could only ever be completed over serial.
void SetWaitPump(bool (*fn)());

}  // namespace SwcTests
