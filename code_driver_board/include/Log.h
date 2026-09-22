#pragma once

// Output, in one place, because there are two front ends (serial and web) and a
// test must not care which one is watching.
//
// Every test writes through here. Lines are emitted to the serial console AND
// appended to a capture buffer the web UI serves. That is the whole design: no
// test knows about either transport, so the same 30 tests work tethered or over
// WiFi, and neither path can have a formatting quirk the other lacks.
//
// THREADING. Tests run on their own FreeRTOS task (see include/TestTask.h) while the
// web server and serial menu run on loop()'s task, so this IS multi-threaded and the
// guard lives here. Two writers would otherwise interleave inside a line and the
// reader could flatten the ring mid-update, producing garbled output on the exact page
// you are using to diagnose the board.
//
// The cost is one mutex per line, which is nothing next to a serial write.

#include <stdarg.h>
#include <stddef.h>

namespace Log {

void Begin();

// The mutex must exist before any other call. Called from Begin(); exposed so the
// task that owns the tests can be sure of the ordering.
void Lock();
void Unlock();

// printf-style, appends a newline. The workhorse.
void Printf(const char *fmt, ...);

// No newline, no capture -- for interactive prompts that a terminal should show
// immediately and a web reader has no use for.
void Prompt(const char *fmt, ...);

// A section rule, for readability on a terminal.
void Rule(char ch = '-', int width = 72);

// A titled section.
void Section(const char *title);

// ---- capture -------------------------------------------------------------
// The most recent lines, oldest first. Sized to hold a whole full-suite run.
const char *Capture(size_t *out_len);
void       ClearCapture();
size_t     CaptureLines();

}  // namespace Log
