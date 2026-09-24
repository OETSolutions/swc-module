package com.oetsolutions.swc.ui

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import com.oetsolutions.swc.link.LinkProblem
import com.oetsolutions.swc.link.LinkState
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
    fun `the open card names WHY the window opened`() {
        // N-61: the device records the trigger, and until the field was carried to
        // this card a user holding AUX1 at the car saw exactly what a user who
        // tapped "Enter maintenance mode" saw -- the same double-flash. The card is
        // the only place a user can learn which path they took.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(
                    maintenanceOpen = true,
                    maintenanceTrigger = "aux1_hold",
                ),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule
            .onNodeWithText(
                "Opened by: holding AUX1 on the device (no app needed)",
            )
            .assertIsDisplayed()
    }

    @Test
    fun `no trigger line is shown while the window is closed`() {
        // The field means nothing on a closed window, and a line reading "Opened
        // by: …" under a "not in maintenance mode" card would be the exact confusion
        // the trigger exists to remove.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(maintenanceOpen = false, maintenanceTrigger = "none"),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule.onNodeWithText("Opened by:", substring = true).assertDoesNotExist()
    }

    @Test
    fun `a trigger word this build does not know is shown verbatim`() {
        // A firmware that adds a trigger must not have its word silently mapped to
        // a wrong phrase; the unknown word is surfaced so the app is not lying.
        assertEquals("the app, over USB", describeTrigger("usb_command"))
        assertEquals("holding AUX1 on the device (no app needed)", describeTrigger("aux1_hold"))
        assertEquals("a_new_trigger", describeTrigger("a_new_trigger"))
    }

    @Test
    fun `the open card names the configured timeout and not the inactivity story`() {
        // The window is open and the radio has neither come up nor failed yet --
        // the frame that says so arrives a moment before the bring-up finishes, so
        // the card must not promise a page it does not yet have.
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
        composeRule
            .onNodeWithText(
                "The device is in maintenance mode and is bringing its radio up. " +
                    "The mode returns to normal by itself after 20 minutes.",
            )
            .assertIsDisplayed()
    }

    @Test
    fun `a radio failure is stated rather than a setup page promised`() {
        // The window and the radio are separate states: the device opens the window
        // and only THEN brings the radio up, and that bring-up can fail. The frame
        // reports the failure count, and this card is where a user would otherwise
        // be sent hunting for an access point that does not exist -- the N-76 shape
        // (a user-facing surface asserting a capability the device does not have).
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(
                    maintenanceOpen = true,
                    maintenanceBleFailures = 1,
                    // Even WITH a URL present the failure wins: a page address from
                    // a radio that failed is not a page to send a user to.
                    maintenancePageUrl = "http://192.168.4.1/?token=ABCDEF123456",
                ),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule
            .onNodeWithText("its radio did not come up", substring = true)
            .assertIsDisplayed()
        composeRule
            .onNodeWithText("http://192.168.4.1/?token=ABCDEF123456", substring = true)
            .assertDoesNotExist()
    }

    @Test
    fun `an open window with a live radio shows the page url and the ble secrets`() {
        // Spec 8.3 option 1: the board has no display, so the Proof-of-Possession
        // and the page token reach the user THROUGH THIS SCREEN or not at all. A
        // frame that arrives and is never rendered is the "produced, consumed by
        // nobody" shape (N-22/N-24/N-45) with the user unable to provision.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(
                    maintenanceOpen = true,
                    maintenancePop = "A1B2C3",
                    maintenanceToken = "ABCDEF123456",
                    maintenancePageUrl = "http://192.168.4.1/?token=ABCDEF123456",
                    maintenanceBleName = "A1B2",
                ),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule.onNodeWithText("A1B2C3").assertIsDisplayed()
        composeRule.onNodeWithText("SWC-A1B2").assertIsDisplayed()
        composeRule
            .onNodeWithText("http://192.168.4.1/?token=ABCDEF123456", substring = true)
            .assertIsDisplayed()
    }

    @Test
    fun `no secret is shown for a window whose radio is not up`() {
        // A credential rendered for a session that is not listening is worse than
        // no credential: the user types it into the Espressif app and gets a
        // timeout that looks like a bug in the app.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(
                    maintenanceOpen = true,
                    maintenanceBleFailures = 2,
                    maintenancePop = "A1B2C3",
                    maintenanceBleName = "A1B2",
                ),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule.onNodeWithText("A1B2C3").assertDoesNotExist()
        composeRule.onNodeWithText("SWC-A1B2").assertDoesNotExist()
    }

    @Test
    fun `the maintenance card does not claim the device's radio is up`() {
        // The CLOSED branch must not promise a radio, and neither may the OPEN one
        // when the device has not reported it up. The card used to read "The
        // device's WiFi is on. Connect to its setup page…" in the open state and
        // "Turn the device's WiFi on" when closed -- both asserting a radio no code
        // started, which is the N-35/N-39 shape: user-facing copy promising a
        // behaviour the device does not have. The strings themselves stay banned;
        // what the card may now say is the device's OWN report (see the tests above).
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
    fun `a failed earlier window does not make a later working one claim its radio is down`() {
        // N-81, the app-visible half. The card branches on `ble_failures > 0` FIRST,
        // so a count that outlived its window made EVERY later window say "its radio
        // did not come up, so there is no setup page to open" -- while the same frame
        // carried a live `page_url` and the working PoP. The device now clears the
        // count when a window closes (MaintenanceRadioStop); this asserts the card is
        // correct for the state that produces: radio up, count zero, secrets present.
        //
        // A count of 0 with a page_url MUST render the URL. If the device ever
        // regresses to a sticky count, this is the frame it would send for a working
        // window after any earlier failure, and it would fail here.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(
                    maintenanceOpen = true,
                    maintenanceBleFailures = 0,
                    maintenancePageUrl = "http://192.168.4.1/?token=ABCDEF123456",
                    maintenancePop = "A1B2C3",
                    maintenanceBleName = "A1B2",
                ),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule
            .onNodeWithText("http://192.168.4.1/?token=ABCDEF123456", substring = true)
            .assertIsDisplayed()
        composeRule.onNodeWithText("its radio did not come up", substring = true)
            .assertDoesNotExist()
        composeRule.onNodeWithText("A1B2C3").assertIsDisplayed()
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

    @Test
    fun `a silent link is shown as not responding, not as connected`() {
        // Spec §4.4's liveness, the app's own half (open item N-27). The whole
        // point of the state is that "Connected" was the last thing the app had
        // been told while the cable was already out, so the headline must change.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(link = LinkState.SilenceExpired),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule.onNodeWithText("Not responding").assertIsDisplayed()
        composeRule.onNodeWithText("Connected").assertDoesNotExist()
    }

    @Test
    fun `a silent link gets physical guidance, not a retry`() {
        // Its own message. Folded onto "The link failed" the user is told to retry
        // a conversation that did not fail; the actual cause is almost always the
        // cable, so the copy says so and reminds them the adapter still works.
        composeRule.setContent {
            LinkScreen(
                state = LinkUiState(
                    link = LinkState.SilenceExpired,
                    problem = LinkProblem.SilenceExpired,
                ),
                onRetry = {},
                onEnterMaintenance = {},
                onExitMaintenance = {},
            )
        }
        composeRule.onNodeWithText("The adapter stopped responding").assertIsDisplayed()
        composeRule.onNodeWithText("10 seconds", substring = true).assertIsDisplayed()
    }
}
