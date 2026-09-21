#pragma once

// The test registry and the runner.
//
// Every hardware function on the board is one entry here. A test is a struct with
// a number, a title, a "what you need wired" prerequisite, an optional setup hook
// the OPERATOR is asked to confirm, and a body. The runner is shared by both front
// ends (serial menu, web page) so a test behaves identically either way.
//
// WHY A REGISTRY rather than a switch: the number-to-title mapping has to be
// printable (a menu, a web table) and runnable, and two copies of it drift. There
// is exactly one ordered list below, and `AllTests()` is its only reader.

#include <stddef.h>
#include <stdint.h>

namespace TestRunner {

enum class Result : uint8_t {
    kNotRun = 0,
    kPass,
    kFail,
    kSkip,      // ran but the condition was not present (e.g. no ladder fitted)
    kBlocked,   // a precondition failed; the test's premise is void
    kWarn,      // measured, outside the expected window, but not a defect
};

const char *ResultName(Result r);

// The outcome of one test, plus a one-line summary for the web table.
struct Outcome {
    Result result = Result::kNotRun;
    char   summary[160] = {0};
    uint32_t duration_ms = 0;
};

// A test.
//
// `needs` is printed before the test runs: it is what the operator must have
// wired or attached. It is NOT machine-checked -- the board cannot see a jumper
// -- so it is stated in the log and repeated on the web page, and the test's
// measurements carry the diagnosis when the wiring turns out to be absent.
//
// `manual_setup` returns a C string when the operator must do something before
// the body runs (fit a loopback jumper, for instance). Returning non-null makes
// the runner PAUSE: on the serial front end it waits for a keypress, on the web
// front end it records a "needs setup" state and the page shows the instruction.
// Returning null runs straight through.
struct Test {
    int number;
    const char *title;
    const char *needs;
    const char *(*manual_setup)();
    Outcome (*body)();
    // Which board function this covers, for the coverage table in README.
    const char *covers;
};

// The ordered list. Index 0 is test 1.
const Test *AllTests();
size_t      Count();
const Test *Get(size_t index);   // by 0-based index, null if out of range

// Run one test by its 0-based index. Prints a header, a result line, and stores
// the outcome so the web table and the final summary can read it back.
Outcome Run(size_t index);

// Helpers a test body returns. `Current()` returns the outcome under
// construction -- what a passing test returns. `Blocked()` marks it and returns
// it, for a test whose precondition is absent.
const Outcome &Current();
Outcome Blocked();

// The stored outcome for a test, for the summary and the web table.
const Outcome &LastOutcome(size_t index);
void ResetOutcomes();

// The index of the test currently running, or -1 when none is. The web page uses
// this to show RUNNING: a test executes synchronously inside loop() for up to ~15 s,
// during which the HTTP server cannot answer, so the page needs a way to render
// "in flight" on the reload that follows.
int RunningIndex();

// Run every test in order. Returns the number that failed or errored.
int RunAll();

// A summary table of everything run so far.
void PrintSummary();

// ---------------------------------------------------------------------------
// Assertions, shared by all tests. A test's body returns an Outcome.
//
// These are functions rather than macros so a failure can carry its numbers into
// the log line rather than just a source position -- on a bench, "expected 1.82
// gain, measured 1.02" is the whole value.
// ---------------------------------------------------------------------------
namespace Check {

// Records a pass/fail into the outcome being built. The runner owns a pointer to
// the current outcome; tests call these and then return `Runner::Current()`.
void Init(Outcome *out);

// A boolean condition with a message.
void True(bool cond, const char *what);

// A measured value inside [lo, hi].
void InRange(long value, long lo, long hi, const char *what);

// Two values equal within a tolerance.
void Near(long got, long want, long tol, const char *what);

// Informational -- never fails, always logged.
void Note(const char *fmt, ...);

// True if everything so far passed.
bool Passing();

// Attach the one-line summary (first failure, or "all checks passed").
void Finish();

}  // namespace Check

}  // namespace TestRunner
