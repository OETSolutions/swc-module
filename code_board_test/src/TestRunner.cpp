#include "TestRunner.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "Log.h"

namespace TestRunner {

// The outcome currently being built. Tests call Check::* then return
// Check::Build(), which finalises it. One at a time, single-threaded.
static Outcome s_current;
static Outcome s_results[64];
static size_t  s_result_count = 0;
static int     s_running = -1;   // the test in flight, for the web page

const char *ResultName(Result r)
{
    switch (r) {
        case Result::kNotRun:  return "NOT RUN";
        case Result::kPass:    return "PASS";
        case Result::kFail:    return "FAIL";
        case Result::kSkip:    return "SKIP";
        case Result::kBlocked: return "BLOCKED";
        case Result::kWarn:    return "WARN";
    }
    return "?";
}

Outcome &MutableCurrent() { return s_current; }
const Outcome &Current() { return s_current; }

// ---------------------------------------------------------------------------
// Check
// ---------------------------------------------------------------------------
namespace Check {

static Outcome *s_out = nullptr;
static bool     s_any_fail = false;

void Init(Outcome *out)
{
    s_out = out;
    s_any_fail = false;
}

// Record a failure. The FIRST failure's text becomes the one-line summary, so a
// web table shows the root cause rather than the last thing that happened to be
// checked. `what` is the assertion; `detail` carries the numbers.
static void FailDetail(const char *what, const char *detail)
{
    if (!s_out) return;
    if (s_out->summary[0] == '\0') {
        if (detail && detail[0]) {
            snprintf(s_out->summary, sizeof(s_out->summary), "%s -- %s", what, detail);
        } else {
            snprintf(s_out->summary, sizeof(s_out->summary), "%s", what);
        }
    }
    s_any_fail = true;
    Log::Printf("  [FAIL] %s", what);
    if (detail && detail[0]) Log::Printf("         %s", detail);
}

void True(bool cond, const char *what)
{
    if (cond) {
        Log::Printf("  [ok]   %s", what);
    } else {
        FailDetail(what, "");
    }
}

void InRange(long value, long lo, long hi, const char *what)
{
    if (value >= lo && value <= hi) {
        Log::Printf("  [ok]   %s (%ld in [%ld, %ld])", what, value, lo, hi);
    } else {
        char d[96];
        snprintf(d, sizeof(d), "measured %ld, wanted [%ld, %ld]", value, lo, hi);
        FailDetail(what, d);
    }
}

void Near(long got, long want, long tol, const char *what)
{
    const long d = labs(got - want);
    if (d <= tol) {
        Log::Printf("  [ok]   %s (got %ld, want %ld +/- %ld)", what, got, want, tol);
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "measured %ld, wanted %ld +/- %ld (off by %ld)",
                 got, want, tol, d);
        FailDetail(what, msg);
    }
}

void Note(const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // Notes wrap: they are the prose that explains a measurement, and a bench log
    // that scrolls sideways is a log nobody reads.
    const char *p = buf;
    while (*p) {
        const size_t avail = strlen(p);
        size_t take = avail > 110 ? 110 : avail;
        if (take < avail) {
            // Break at the last space so words are not split.
            size_t brk = take;
            while (brk > 40 && p[brk] != ' ') --brk;
            if (brk > 40) take = brk;
        }
        char line[128];
        const size_t n = take < sizeof(line) - 1 ? take : sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        Log::Printf("  note: %s", line);
        p += take;
        while (*p == ' ') ++p;
    }
}

bool Passing() { return !s_any_fail; }

void Finish()
{
    if (!s_out) return;
    if (s_out->summary[0] == '\0') {
        snprintf(s_out->summary, sizeof(s_out->summary), "all checks passed");
    }
}

}  // namespace Check

Outcome Blocked()
{
    s_current.result = Result::kBlocked;
    snprintf(s_current.summary, sizeof(s_current.summary),
             "a precondition failed (see the note above)");
    return s_current;
}

// ---------------------------------------------------------------------------
// The runner
// ---------------------------------------------------------------------------
static const size_t kMaxResults = sizeof(s_results) / sizeof(s_results[0]);

void ResetOutcomes()
{
    for (size_t i = 0; i < kMaxResults; ++i) s_results[i] = Outcome{};
    s_result_count = 0;
}

const Outcome &LastOutcome(size_t index)
{
    static Outcome empty;
    if (index >= kMaxResults) return empty;
    if (index >= s_result_count) return empty;
    return s_results[index];
}

int RunningIndex() { return s_running; }

Outcome Run(size_t index)
{
    const Test *t = Get(index);
    s_current = Outcome{};
    if (!t) {
        s_current.result = Result::kFail;
        snprintf(s_current.summary, sizeof(s_current.summary), "no such test");
        return s_current;
    }

    Log::Rule('=');
    Log::Printf("TEST %d  %s", t->number, t->title);
    Log::Printf("  covers: %s", t->covers);
    if (t->needs && t->needs[0]) {
        Log::Printf("  needs : %s", t->needs);
    }
    if (t->manual_setup) {
        const char *msg = t->manual_setup();
        if (msg && msg[0]) Log::Printf("  setup : %s", msg);
    }
    Log::Rule('-');

    Check::Init(&s_current);
    s_running = (int)index;
    const uint32_t t0 = millis();
    s_current = t->body();
    s_current.duration_ms = millis() - t0;
    s_running = -1;

    if (s_current.result == Result::kNotRun) {
        Check::Finish();
        s_current.result = Check::Passing() ? Result::kPass : Result::kFail;
    } else {
        Check::Finish();
    }

    if (index < kMaxResults) {
        s_results[index] = s_current;
        if (index >= s_result_count) s_result_count = index + 1;
    }

    Log::Rule('-');
    Log::Printf("RESULT %d: %s  (%u ms)  %s", t->number, ResultName(s_current.result),
                s_current.duration_ms, s_current.summary);
    Log::Printf("");
    return s_current;
}

int RunAll()
{
    int bad = 0;
    for (size_t i = 0; i < Count(); ++i) {
        const Test *t = Get(i);
        if (!t) continue;
        const Outcome o = Run(i);
        if (o.result == Result::kFail || o.result == Result::kBlocked) ++bad;
    }
    PrintSummary();
    return bad;
}

void PrintSummary()
{
    Log::Section("SUMMARY");
    Log::Printf("  %-4s %-9s %-9s %s", "no.", "result", "ms", "title");
    int nfail = 0, npass = 0, nskip = 0, nblocked = 0, nnotrun = 0;
    for (size_t i = 0; i < Count(); ++i) {
        const Test *t = Get(i);
        if (!t) continue;
        const Outcome &o = LastOutcome(i);
        Log::Printf("  %-4d %-9s %-9u %s", t->number, ResultName(o.result), o.duration_ms,
                    t->title);
        switch (o.result) {
            case Result::kPass: ++npass; break;
            case Result::kFail: ++nfail; break;
            case Result::kSkip: ++nskip; break;
            case Result::kBlocked: ++nblocked; break;
            case Result::kWarn: ++npass; break;
            default: ++nnotrun; break;
        }
    }
    Log::Rule('-');
    Log::Printf("  passed %d, failed %d, skipped %d, blocked %d, not run %d",
                npass, nfail, nskip, nblocked, nnotrun);

    // Failures first, then blocked: the two that need action.
    if (nfail) {
        Log::Printf("");
        Log::Printf("  FAILURES:");
        for (size_t i = 0; i < Count(); ++i) {
            const Test *t = Get(i);
            const Outcome &o = LastOutcome(i);
            if (o.result == Result::kFail) {
                Log::Printf("    %d. %s", t->number, t->title);
                Log::Printf("       %s", o.summary);
            }
        }
    }
    if (nblocked) {
        Log::Printf("");
        Log::Printf("  BLOCKED (a precondition was not met):");
        for (size_t i = 0; i < Count(); ++i) {
            const Test *t = Get(i);
            const Outcome &o = LastOutcome(i);
            if (o.result == Result::kBlocked) {
                Log::Printf("    %d. %s -- %s", t->number, t->title, o.summary);
            }
        }
    }
}

}  // namespace TestRunner
