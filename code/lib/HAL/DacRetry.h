#pragma once

#include <stdint.h>

/*
 * Spec 6.8's I2C-to-DAC-fails row: "Retry with backoff; if persistent, release
 * the line and report a fault; never drive a guessed code."
 *
 * The policy lives here, in a header with no IDF dependency, for the same reason
 * `DacFrame` does: `EspHal.cpp` is the one lib/ translation unit the host build
 * excludes, so a retry loop written inside it is executed by nothing until a
 * board is on the bench. Open item N-21 recorded that gap -- the comment in both
 * `IHAL.h` and `EspHal.cpp` claimed dac_set_code "retries with backoff
 * internally and latches a fault on persistent failure", and neither half was
 * true: one synchronous `i2c_master_transmit`, one `ESP_LOGE`.
 *
 * The numbers are chosen against the WRITE path's tolerance, not the bus's:
 *
 * - **3 attempts.** An MCP4728 NACK on a healthy bus is a transient -- a glitch,
 *   a clock stretch that overran the timeout. A genuinely absent or misaddressed
 *   part NACKs every time, and no retry count rescues that; 3 separates the two
 *   without stalling the caller on the second case.
 * - **1 ms then 2 ms of backoff.** A key press resolves inside `Tick`, and
 *   `send_duration_ms` (spec 3.7's default 200 ms) is the whole budget for
 *   driving it. A backoff long enough to matter to a flaky bus (tens of ms) is
 *   long enough to eat that budget, so the waits are deliberately short: they
 *   exist to get past a transient, not to wait one out. Doubling is the
 *   conventional shape and is what "backoff" in the spec names.
 *
 * **No delay after a failed final attempt.** There is nothing left to wait for,
 * and a caller on the key path should not pay for a failure it can already
 * report. The waits are *between* attempts (1 ms then 2 ms), so the total the
 * retry can add is 3 ms.
 */
namespace DacRetry {

constexpr int kMaxAttempts = 3;

// Milliseconds to wait BEFORE attempt `attempt` (0-based). Nothing has failed
// before the first attempt, so that wait is zero, and the loop's `attempt > 0`
// guard skips it anyway; the real waits are the two RETRIES, 1 ms then 2 ms.
constexpr uint32_t BackoffMsBefore(int attempt)
{
    return (attempt == 1) ? 1u : ((attempt == 2) ? 2u : 0u);
}

// How many attempts have already been made, from the attempt count that just
// succeeded or ran out. Named so the caller's log line does not have to
// re-derive "was this the first try".
constexpr bool MoreAttemptsRemain(int attempt) { return attempt < kMaxAttempts; }

}  // namespace DacRetry
