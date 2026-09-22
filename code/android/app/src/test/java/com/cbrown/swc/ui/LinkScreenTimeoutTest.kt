package com.oetsolutions.swc.ui

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * The maintenance card's timeout copy.
 *
 * It used to read "It returns to normal by itself after 5 minutes of no
 * activity." Two things were wrong with that. The firmware closes on a FIXED
 * deadline from entry, not on inactivity — nothing calls `NoteActivity` (spec
 * open item N-35) — so "of no activity" described a behaviour the device does
 * not have. And 5 minutes is only the DEFAULT: `maintenance_timeout_ms` is a
 * validated setting, so a device told to hold the window for 20 minutes was
 * still described to its user as 5.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class LinkScreenTimeoutTest {

    @get:Rule
    val composeRule = createComposeRule()

    @Test
    fun `the window length is stated in minutes and rounds up`() {
        assertEquals("after 5 minutes", describeTimeout(300_000L))
        assertEquals("after 1 minute", describeTimeout(60_000L))
        assertEquals("after 20 minutes", describeTimeout(1_200_000L))
        // Rounded UP: telling a user the window is shorter than it is makes them
        // rush; telling them it is longer is how they get cut off mid-task.
        assertEquals("after 3 minutes", describeTimeout(121_000L))
        assertEquals("after 2 minutes", describeTimeout(60_001L))
    }

    @Test
    fun `a sub-minute window is not rounded down to zero`() {
        // The codec accepts any timeout in `(0, kMaintenanceTimeoutMaxMs]`, so a
        // sub-minute value is legal and must not render as "after 0 minutes".
        assertEquals("after under a minute", describeTimeout(30_000L))
        assertEquals("after under a minute", describeTimeout(1L))
        assertEquals("after a short delay", describeTimeout(0L))
    }

    @Test
    fun `the open card names the configured timeout and not the inactivity story`() {
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(
                    maintenanceOpen = true,
                    maintenanceTimeoutMs = 1_200_000L,
                ),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        // The device's OWN value, not the 5-minute default.
        composeRule
            .onNodeWithText(
                "The device is in maintenance mode. Its WiFi setup page is " +
                    "not available yet -- this build does not start the " +
                    "device's radio. The mode returns to normal by itself " +
                    "after 20 minutes.",
            )
            .assertIsDisplayed()
    }

    @Test
    fun `the maintenance card does not claim the device's radio is up`() {
        // The device acks `maintenance_enter` and enters the mode, but nothing
        // brings up NimBLE, `wifi_provisioning` or the web server (`MaintenanceMode`
        // is pure state; spec open item N-15). The card used to read "The device's
        // WiFi is on. Connect to its setup page…" and "Turn the device's WiFi on"
        // -- both asserting a radio no code starts, which is the N-35/N-39 shape:
        // user-facing copy promising a behaviour the device does not have. A user
        // hunting for an access point that does not exist is the concrete harm.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(maintenanceOpen = true),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule.onNodeWithText("The device's WiFi is on", substring = true)
            .assertDoesNotExist()
        composeRule.onNodeWithText("Turn the device's WiFi on", substring = true)
            .assertDoesNotExist()
    }

    @Test
    fun `the closed maintenance card does not claim it turns the radio on either`() {
        // The CLOSED branch made the same promise ("Turn the device's WiFi on"),
        // so a fix that only corrected the open branch would leave the other lying.
        // Asserted separately because a Compose test rule allows one `setContent`.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(maintenanceOpen = false),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule.onNodeWithText("The device's WiFi is on", substring = true)
            .assertDoesNotExist()
        composeRule.onNodeWithText("Turn the device's WiFi on", substring = true)
            .assertDoesNotExist()
    }

    @Test
    fun `the device card shows the board temperature and free heap`() {
        // Open item N-22: `temp_c` and `heap_free` now arrive, so the screen must
        // show them -- a value the transport delivers and no screen renders is the
        // "produced, consumed by nobody" shape this project keeps finding.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(lastTempC = 23.5, lastHeapFree = 123456L),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule.onNodeWithText("Board temp: 23.5 C").assertIsDisplayed()
        composeRule.onNodeWithText("Free heap: 123456 bytes").assertIsDisplayed()
    }

    @Test
    fun `an unmeasured temperature says so rather than showing zero degrees`() {
        // The firmware sends JSON null until an NTC conversion is good, and 0 C is
        // a legal temperature -- so a `0.0` here would claim a measured freezing
        // board. The screen must say "no reading" instead. A heap figure means a
        // status HAS arrived, which is what distinguishes "no reading" from
        // "nothing has been heard from the device yet".
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(lastTempC = null, lastHeapFree = 900L),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule.onNodeWithText("Board temp: no reading").assertIsDisplayed()
        composeRule.onNodeWithText("Board temp: 0.0 C").assertDoesNotExist()
    }
}
