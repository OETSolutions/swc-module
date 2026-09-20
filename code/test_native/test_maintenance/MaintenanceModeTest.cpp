#include "Maintenance/MaintenanceMode.h"

#include "MockHAL.h"

#include <gtest/gtest.h>

TEST(MaintenanceMode, IsNotActiveUntilExplicitlyEntered) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    EXPECT_FALSE(m.Active()) << "FR-32: no radio during normal operation";
    EXPECT_EQ(m.Trigger(), MaintenanceTrigger::kNone);
}

TEST(MaintenanceMode, EntersOnEachOfTheDocumentedTriggers) {
    for (MaintenanceTrigger t : {MaintenanceTrigger::kUsbCommand,
                                 MaintenanceTrigger::kConfigFlag,
                                 MaintenanceTrigger::kAux1Hold,
                                 MaintenanceTrigger::kNoConfigAtBoot}) {
        MockHal hal;
        MaintenanceMode m(&hal.InterfaceRef());
        EXPECT_TRUE(m.Enter(t, 1000)) << "trigger " << static_cast<int>(t);
        EXPECT_TRUE(m.Active());
        EXPECT_EQ(m.Trigger(), t) << "the caller's shutdown path differs per trigger";
    }
}

TEST(MaintenanceMode, TimesOutAfterFiveMinutesOfInactivity) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    m.Update(299999);
    EXPECT_TRUE(m.Active());
    m.Update(300001);
    EXPECT_FALSE(m.Active())
        << "FR-38: a device left in maintenance cannot serve presses, so it must return";
}

TEST(MaintenanceMode, TheTimeoutIsExactlyAtTheBoundaryNotNearIt) {
    // The off-by-one the "after five minutes" phrasing hides: at exactly
    // timeout_ms the window is over, and one millisecond earlier it is not.
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 100);
    m.Update(100 + 299999);
    EXPECT_TRUE(m.Active()) << "one ms before the deadline is still inside";
    m.Update(100 + 300000);
    EXPECT_FALSE(m.Active()) << "the boundary itself closes the window";
}

TEST(MaintenanceMode, ActivityResetsTheTimeout) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    for (int i = 0; i < 10; ++i) {
        m.Update(static_cast<uint64_t>(i) * 299000);
        m.NoteActivity(static_cast<uint64_t>(i) * 299000);
    }
    EXPECT_TRUE(m.Active()) << "a user actively working must not be kicked out";
}

TEST(MaintenanceMode, NoteActivityOnAClosedWindowDoesNotReopenIt) {
    // NoteActivity is called from an HTTP handler that may race an exit. It must
    // never be a second way to enter maintenance, or the radio could be brought
    // up by a stray request.
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.NoteActivity(5000);
    EXPECT_FALSE(m.Active());
    m.Update(999999999);
    EXPECT_FALSE(m.Active());
}

TEST(MaintenanceMode, ExitingClearsTheActiveFlagAndTheTimer) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    m.Exit();
    EXPECT_FALSE(m.Active());
    m.Update(999999999);
    EXPECT_FALSE(m.Active());
}

TEST(MaintenanceMode, ReEntryAfterATimeoutStartsAFreshWindow) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    m.Update(400000);
    ASSERT_FALSE(m.Active());
    m.Enter(MaintenanceTrigger::kUsbCommand, 400000);
    m.Update(400000 + 299000);
    EXPECT_TRUE(m.Active()) << "the second window must not inherit the first's elapsed time";
}

TEST(MaintenanceMode, KeyPressesAreStillServedWhileMaintenanceIsActive) {
    // The requirement behind FR-38 is that maintenance must not make the device
    // useless. The orchestrator keeps ticking in maintenance; this asserts the
    // mode reports itself as non-exclusive so the caller keeps serving input.
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    EXPECT_FALSE(m.Exclusive()) << "maintenance must not stop the adapter working";
}

TEST(MaintenanceMode, AConfiguredTimeoutIsHonouredRatherThanTheDefault) {
    // A config may carry `settings.maintenance_timeout_ms`; the default is only a
    // fallback for a bare device.
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef(), /*timeout_ms=*/60000);
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    m.Update(59999);
    EXPECT_TRUE(m.Active());
    m.Update(60000);
    EXPECT_FALSE(m.Active());
}

TEST(MaintenanceMode, AClockThatGoesBackwardsDoesNotLookLikeATimeout) {
    // Unsigned wrap: `now - last_activity_` with an earlier `now` is a huge
    // number, which would read as "timed out long ago" and drop the user out
    // mid-task. An unobserved interval is not an elapsed one.
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 1000000);
    m.Update(500000);
    EXPECT_TRUE(m.Active()) << "a rewound clock must not close the window";
}

TEST(MaintenanceMode, ShouldTimeoutReportsTheTransitionTheCallerTearsDownOn) {
    MockHal hal;
    MaintenanceMode m(&hal.InterfaceRef());
    m.Enter(MaintenanceTrigger::kUsbCommand, 0);
    EXPECT_FALSE(m.ShouldTimeout(1000));
    EXPECT_TRUE(m.ShouldTimeout(300000));
    m.Update(300000);
    EXPECT_FALSE(m.Active());
    // And on a closed window it is false, so a caller polling it does not try to
    // tear down a radio that is not up.
    EXPECT_FALSE(m.ShouldTimeout(400000));
}
