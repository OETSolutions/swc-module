#pragma once

// The test task: tests run HERE, on their own FreeRTOS task, not in loop().
//
// WHY THIS EXISTS. Running a test synchronously inside loop() meant the web server
// was never serviced for the duration of the test -- 9 s for test 20, 12 s for test
// 22, and up to 90 s for test 31 because it waits for the operator. During that time
// the page could not load, could not refresh, and clicks did nothing, so a test that
// was running perfectly looked like a dead page. Every attempt to patch around this
// (a pump inside the wait, a deferred request flag, a spinner) was treating the
// symptom. The cause is that one loop cannot both run a blocking test and serve HTTP.
//
// So the work moved off the loop:
//
//   loop()'s task          the web server and the serial menu -- responsive always
//   the test task (this)   one test at a time, blocking as much as it likes
//
// Nothing above needs a state machine or a coroutine, and a test body can still be
// written as a straight line, which is what makes them readable.

#include <stddef.h>
#include <stdint.h>

namespace TestTask {

// Create the task. Called once from the boot sequence.
void Begin();

// True once the task and its queue exist. Callers that submit work check this so a
// failed start reports itself rather than looking like "busy forever".
bool Ready();

// Ask for a test to be run by NUMBER (the number the menu and the web page show).
// Returns false if a test is already running or the number is unknown. Non-blocking:
// the request is queued and the task picks it up.
bool RequestByNumber(int number);

// Ask for every test, in order. Same non-blocking contract.
bool RequestAll();

// The index of the test in flight, or -1. Safe to call from the web task.
int RunningIndex();

// True while a test is running.
bool Busy();

// ---------------------------------------------------------------------------
// The operator prompt, for the tests that need a human.
//
// A test calls AskOperator() and blocks on THIS task until either the serial console
// or the web page says "continue". The web side works because the web server is on
// the other task and never stopped running.
// ---------------------------------------------------------------------------

// Set by the web front end when the operator presses Continue.
void SignalContinue();

// True while a test is waiting for the operator. The serial front end MUST check
// this before consuming input: both tasks read the same UART, and without this the
// loop task eats the keystroke the waiting test is listening for -- so a keystroke
// appears to do nothing and the test times out despite the operator pressing ENTER.
bool WaitingForOperator();

// Ask the operator to do something, and wait. `what` is shown prominently on both
// front ends. Returns true if they responded, false on timeout.
bool AskOperator(const char *what, uint32_t timeout_ms);

// The text of the current prompt, or empty if none. The web page renders this
// prominently so the operator cannot miss what is being asked.
const char *CurrentPrompt();

}  // namespace TestTask
