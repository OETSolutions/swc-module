#include "Log.h"

#include <Arduino.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

// Recursive, because Capture() is called from a handler that may already hold it and
// a plain mutex there would self-deadlock. Created in Begin() before anything else
// can touch the buffer.
static SemaphoreHandle_t s_mtx = nullptr;

void Lock()   { if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
void Unlock() { if (s_mtx) xSemaphoreGiveRecursive(s_mtx); }

void Begin()
{
    if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex();
    Serial.begin(115200);

    // DO NOT BLOCK ON THE USB CONSOLE.
    //
    // HWCDC::write() blocks trying to push each line into its TX ring buffer, and if
    // no host is draining the USB CDC it waits up to 20 x tx_timeout_ms = 2 SECONDS
    // per write before giving up. That made this tool look wildly slow whenever
    // nobody had a serial terminal open: test 31 emits ~65 lines, so it took
    // 65 x 2 s ~= 130 s instead of 0.4 s. The measurement was never slow -- the
    // LOGGING was, and only when the web UI was being used without a serial monitor.
    //
    // The USB CDC connection stays "plugged" as long as any host has the port open
    // (a browser serial page counts), so the disconnect path never fires and every
    // write pays the full timeout.
    //
    // A timeout of 0 makes the write NON-BLOCKING. The first attempt at this used
    // 10 ms and it was still far too slow, because HWCDC::write() retries up to
    // max_consec_timeouts = 20 times before giving up -- so a "10 ms" timeout was
    // really 200 ms per line, and a 65-line test still took ~13 s of pure stall.
    // Zero removes the retry loop's cost entirely: the ring send is attempted once
    // and the write returns immediately whether or not it fit.
    //
    // Nothing is lost when a terminal IS attached, because the ring buffer drains
    // between writes and the send succeeds first time -- the timeout is only ever
    // reached when there is no reader, which is exactly when waiting is pointless.
    // The web UI's copy is unaffected either way: it is captured into the ring buffer
    // in CaptureLine() before this write happens.
    Serial.setTxTimeoutMs(0);
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
    Lock();
    char buf[kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    Serial.println(buf);
    CaptureLine(buf);
    Unlock();
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
    Lock();
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
    const char *r = s_flat;
    Unlock();
    return r;
}

void ClearCapture()
{
    Lock();
    s_next = s_count = 0;
    s_flat_len = 0;
    s_flat[0] = '\0';
    Unlock();
}

size_t CaptureLines() { Lock(); const size_t n = s_count; Unlock(); return n; }

}  // namespace Log
