#include "Log.h"

#include <Arduino.h>
#include <string.h>

namespace Log {

// ---------------------------------------------------------------------------
// The capture buffer.
//
// A fixed arena of one ring of lines. Deliberately not a std::vector<String>:
// this firmware runs on a part with no PSRAM and a long full-suite run is
// thousands of lines, so an always-growing buffer is the one failure mode that
// would take the heap out *during* the run meant to diagnose the board.
//
// 24 KB holds a complete suite with room to spare. Lines longer than kLineMax
// are truncated -- a test that needs more than 200 characters per line is
// printing a table, and should print rows.
// ---------------------------------------------------------------------------
static const size_t kLineMax   = 200;
static const size_t kRingLines = 120;
static const size_t kLineBytes = kLineMax + 1;

static char   s_ring[kRingLines][kLineBytes];
static size_t s_next  = 0;      // where the next line goes
static size_t s_count = 0;      // how many lines the ring holds, saturating
static char   s_flat[kRingLines * kLineBytes + 1];
static size_t s_flat_len = 0;

void Begin()
{
    Serial.begin(115200);
    // HWCDC's begin() allocates its ring buffers; give the host a moment to
    // enumerate so the banner is not the thing that gets dropped.
    delay(50);
    s_next = s_count = 0;
    s_flat_len = 0;
    s_flat[0] = '\0';
}

// Append one already-formatted line to the ring.
static void CaptureLine(const char *line)
{
    char *dst = s_ring[s_next];
    size_t n = strlen(line);
    if (n > kLineMax) n = kLineMax;
    memcpy(dst, line, n);
    dst[n] = '\0';
    s_next = (s_next + 1) % kRingLines;
    if (s_count < kRingLines) ++s_count;
}

static const char *RingLine(size_t i)
{
    // Oldest first: if the ring is full, the oldest is at s_next.
    size_t start = (s_count < kRingLines) ? 0 : s_next;
    return s_ring[(start + i) % kRingLines];
}

void Printf(const char *fmt, ...)
{
    char buf[kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    Serial.println(buf);
    CaptureLine(buf);
}

void Prompt(const char *fmt, ...)
{
    char buf[kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
}

void Rule(char ch, int width)
{
    char buf[kLineBytes];
    if (width > (int)sizeof(buf) - 1) width = (int)sizeof(buf) - 1;
    if (width < 1) width = 1;
    memset(buf, ch, width);
    buf[width] = '\0';
    Printf("%s", buf);
}

void Section(const char *title)
{
    Rule('=');
    Printf("== %s", title);
    Rule('=');
}

const char *Capture(size_t *out_len)
{
    // Flatten oldest-first so the web reader sees the same order a terminal did.
    size_t off = 0;
    for (size_t i = 0; i < s_count; ++i) {
        const char *ln = RingLine(i);
        size_t n = strlen(ln);
        if (off + n + 1 >= sizeof(s_flat)) break;
        memcpy(s_flat + off, ln, n);
        off += n;
        s_flat[off++] = '\n';
    }
    s_flat[off] = '\0';
    s_flat_len = off;
    if (out_len) *out_len = off;
    return s_flat;
}

void ClearCapture()
{
    s_next = s_count = 0;
    s_flat_len = 0;
    s_flat[0] = '\0';
}

size_t CaptureLines() { return s_count; }

}  // namespace Log
