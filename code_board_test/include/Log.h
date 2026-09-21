#pragma once

// Output, in one place, because there are two front ends (serial and web) and a
// test must not care which one is watching.
//
// Every test writes through here. Lines are emitted to the serial console AND
// appended to a capture buffer the web UI serves. That is the whole design: no
// test knows about either transport, so the same 30 tests work tethered or over
// WiFi, and neither path can have a formatting quirk the other lacks.
//
// Threading: the web server (Arduino WebServer) is polled from loop(), and the
// serial menu runs there too, so all of this is single-threaded. The buffer is
// deliberately NOT mutex-guarded -- if that ever stops being true, the guard
// belongs here and the comment should say so.

#include <stdarg.h>
#include <stddef.h>

namespace Log {

void Begin();

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
