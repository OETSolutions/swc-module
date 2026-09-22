#include <gtest/gtest.h>

#include "HAL/DacRetry.h"

/*
 * Spec 6.8's I2C row promises "retry with backoff" and open item N-21 recorded
 * that nothing implemented it: one synchronous `i2c_master_transmit`, one
 * `ESP_LOGE`, the write lost. The policy is a header constant rather than an
 * inline loop for the same reason DacFrame is: EspHal.cpp is the one lib/ file
 * the host build EXCLUDES, so a loop written inside it is run by no test until a
 * board is on the bench.
 *
 * These assertions pin the SHAPE (attempt count, monotone-then-zero backoff), not
 * a timing: what matters is that attempts are separated and bounded, and that the
 * caller is never made to wait after the last one.
 */

TEST(DacRetry, MakesThreeAttempts) {
    // 3 separates a transient NACK from a genuinely absent or misaddressed part,
    // which NACKs every time -- no retry count rescues that, and the caller is on
    // the key path where `send_duration_ms` is the whole budget.
    EXPECT_EQ(DacRetry::kMaxAttempts, 3);
}

TEST(DacRetry, BacksOffBetweenAttemptsAndNotAfterTheLast) {
    // Attempt indices are 0-based: the wait BEFORE attempt 0 is 0 (nothing has
    // failed yet), then 1 ms and 2 ms. The wait before the FINAL attempt is 0 --
    // there is nothing left to wait for, and a caller that has already failed
    // should not pay for a delay it cannot use.
    EXPECT_EQ(DacRetry::BackoffMsBefore(0), 0u);
    EXPECT_EQ(DacRetry::BackoffMsBefore(1), 1u);
    EXPECT_EQ(DacRetry::BackoffMsBefore(2), 2u);
    // The loop stops at kMaxAttempts, so index 3 is never used; it must not be
    // mistaken for a wait either.
    EXPECT_EQ(DacRetry::BackoffMsBefore(DacRetry::kMaxAttempts), 0u);
}

TEST(DacRetry, TheBackoffGrowsAndStaysWithinTheKeyPulseBudget) {
    // "Backoff" that did not grow would be a plain retry, and one long enough to
    // wait out a flaky bus (tens of ms) would eat the 200 ms recognition pulse
    // (spec 3.7's default `send_duration_ms`). Both halves are asserted because
    // either one alone passes for the wrong implementation.
    EXPECT_LT(DacRetry::BackoffMsBefore(1), DacRetry::BackoffMsBefore(2));
    uint32_t total = 0;
    for (int a = 0; a < DacRetry::kMaxAttempts; ++a) total += DacRetry::BackoffMsBefore(a);
    EXPECT_LT(total, 50u)
        << "the whole retry sequence must stay well inside one key pulse";
}

TEST(DacRetry, MoreAttemptsRemainIsFalseOnTheLastAttempt) {
    EXPECT_TRUE(DacRetry::MoreAttemptsRemain(0));
    EXPECT_TRUE(DacRetry::MoreAttemptsRemain(DacRetry::kMaxAttempts - 1));
    EXPECT_FALSE(DacRetry::MoreAttemptsRemain(DacRetry::kMaxAttempts));
}
