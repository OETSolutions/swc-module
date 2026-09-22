package com.oetsolutions.swc.ui

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.getUnclippedBoundsInRoot
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * The live-ladder test, run on the JVM under Robolectric.
 *
 * The plan specified `connectedDebugAndroidTest`, which needs an emulator or a board
 * attached — so the one test that guards the screen's entire diagnostic value would
 * have been a gate nobody could run. Robolectric runs the same Compose semantics
 * assertions on the JVM, so this executes in CI and on any checkout.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class LadderScreenTest {

    @get:Rule
    val composeRule = createComposeRule()

    private fun state(liveMv: Int?) = LadderUiState(
        idleMv = 2835,
        buttons = listOf(
            LearnedButton("vol_up", "vol_up", mvCenter = 1430, mvTolerance = 120),
            LearnedButton("vol_dn", "vol_dn", mvCenter = 1785, mvTolerance = 120),
        ),
        liveMv = liveMv,
    )

    @Test
    fun `liveLadderView_showsEachLearnedButtonAtItsMeasuredLevel`() {
        composeRule.setContent { LadderScreen(state = state(liveMv = 1430)) }
        // The point of the live view is that the user can see WHICH button the
        // device currently thinks is pressed, so the matched one must be marked.
        composeRule.onNodeWithContentDescription("vol_up, matched").assertExists()
        composeRule.onNodeWithContentDescription("vol_dn, not matched").assertExists()
    }

    @Test
    fun `a reading inside the tolerance band matches that button`() {
        // 1500 is 70 mV from vol_up's 1430 and well inside its ±120 window.
        assertEquals("vol_up", state(liveMv = 1500).matched()?.id)
    }

    @Test
    fun `the match runs on the devices permille ratio, not on absolute millivolts`() {
        // The window is now compared in PERMILLE, exactly as `LadderClassify`
        // does: the centre and half-width are derived from millivolts against the
        // learned idle, then the reading's ratio is tested against them. The
        // tolerance is therefore QUANTIZED -- vol_up's ±120 mV becomes ±42 ‰ of
        // 2835, which is ~±119 mV -- so the boundary is one millivolt tighter than
        // the raw window. That is the device's boundary, and matching it is the
        // whole point of open item N-25: the app must not put the edge of a band
        // somewhere the device does not.
        assertEquals("vol_up", state(liveMv = 1549).matched()?.id)
        assertNull(state(liveMv = 1550).matched())
    }

    @Test
    fun `the ratio is level over idle times 1000, so idle is about 1000 not zero`() {
        // The firmware's `LadderRatioPermille` returns level/idle x 1000, so "at
        // idle" is ~1000. A view assuming the usual "0 is idle" convention would
        // render every band mirrored, which is why this is asserted by name.
        assertEquals(1000, state(null).ratioPermille(2835))
        assertEquals(504, state(null).ratioPermille(1430))
    }

    @Test
    fun `a live idle lets the match reproduce the device on a moved rail`() {
        // Open item N-25. The device classifies the reading against the LIVE idle
        // spec 6.3 maintains, while the stored windows are ratios against the
        // LEARNED idle. When the rail moves, an absolute-millivolt test (or one
        // that used a single idle for both) marks the wrong thing -- here the
        // rail is 5% high, so a real press reads ~5% higher in millivolts than its
        // stored centre and falls OUTSIDE the raw window, yet the device fires it
        // because the RATIO is unchanged. With the live idle on the wire the app
        // now agrees with the device instead of calling it broken.
        val raisedRail = (2835 * 3465) / 3300          // 2977 mV, the +5% band edge
        val press = (1430L * raisedRail / 2835L).toInt()   // vol_up's ratio, at the new rail
        // A tight window is what makes the disagreement observable: vol_up's
        // centre is 1430 ±40 mV. The rail's +5% moves the reading to 1501 mV --
        // 71 mV from the stored centre, OUTSIDE the raw window -- while its RATIO
        // is unchanged, so the device still fires it. This is exactly the "screen
        // says broken, device is working" failure N-25 describes.
        val tight = listOf(LearnedButton("vol_up", "vol_up", mvCenter = 1430, mvTolerance = 40))
        // Absent a live idle the app falls back to the learned rail and misses it.
        assertNull(
            LadderUiState(idleMv = 2835, buttons = tight, liveMv = press).matched(),
        )
        // With the device's live idle the same press matches, as the device says.
        assertEquals(
            "vol_up",
            LadderUiState(
                idleMv = 2835,
                buttons = tight,
                liveMv = press,
                liveIdleMv = raisedRail,
            ).matched()?.id,
        )
    }

    @Test
    fun `no live reading shows no match and no crash`() {
        composeRule.setContent { LadderScreen(state = state(liveMv = null)) }
        composeRule.onNodeWithContentDescription("vol_up, not matched").assertExists()
        composeRule.onNodeWithText("Classified as: nothing").assertExists()
    }

    @Test
    @Config(sdk = [34], qualifiers = "xhdpi")
    fun `the ladder bands are placed in density-independent units`() {
        // `BoxWithConstraints` exposes BOTH `constraints.maxWidth` (pixels) and the
        // scope's `maxWidth` (dp). Reading the pixel one and then applying it with
        // `.dp` scales every band by the density a second time, so on a real phone
        // (~2.75x) the whole scale overflows. At the JVM default density of 1.0 the
        // two units coincide, which is why every other test here passes either way.
        // Run at xhdpi (density 2.0) so the mix-up is observable.
        composeRule.setContent { LadderScreen(state = state(liveMv = 1430)) }
        val scale = composeRule.onNodeWithTag("ladder-scale").getUnclippedBoundsInRoot()
        val band = composeRule.onNodeWithTag("band-vol_up").getUnclippedBoundsInRoot()
        val scaleWidth = (scale.right - scale.left).value
        // The scale runs 0..idle left-to-right, so vol_up's band starts at
        // (centre - tolerance)/idle of the way across, measured from the scale's left.
        val expected = (1430 - 120) / 2835f * scaleWidth
        assertEquals(expected, (band.left - scale.left).value, 1.0f)
    }

    @Test
    fun `an unlearned channel explains itself rather than rendering an empty scale`() {
        composeRule.setContent {
            LadderScreen(LadderUiState(idleMv = 0, buttons = emptyList(), liveMv = 100))
        }
        // A scale divided by a zero rail would be a crash or a nonsense view; the
        // screen says what to do instead.
        composeRule.onNodeWithText(
            "This channel has no learned rail yet. Learn a button first."
        ).assertIsDisplayed()
    }
}

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class UpdateScreenHonestyTest {

    @get:Rule
    val rule = createComposeRule()

    private fun show(state: UpdateUiState) {
        rule.setContent {
            UpdateScreen(state = state, onCheck = {}, onInstallAvailable = {}, onPushOverUsb = {}, onUpdateOverWifi = {})
        }
    }

    @Test
    fun `the USB push is live and the WiFi path still says it is not`() {
        // The USB path is IMPLEMENTED now (the firmware serves the `ota_*` frames,
        // N-14), so its button is live. The WiFi path still opens a maintenance page
        // whose radio is not started (N-15), so that one stays disabled and says so.
        // A live-looking button that does nothing cannot be told from a broken one,
        // which is why the distinction is asserted rather than left implicit.
        show(UpdateUiState())
        rule.onNodeWithText("Push a file over USB").assertIsEnabled()
        rule.onNodeWithText("Update over WiFi (not yet)").assertIsNotEnabled()
    }

    @Test
    fun `a push in progress disables the button and shows how far it has got`() {
        show(UpdateUiState(pushInProgress = true, pushSent = 512, pushTotal = 1024))
        rule.onNodeWithText("Sending…").assertIsNotEnabled()
        rule.onNodeWithText("512 / 1024 bytes").assertExists()
    }

    @Test
    fun `a completed push tells the user nothing was bricked`() {
        show(UpdateUiState(pushResult = PushResult.Installed))
        rule.onNodeWithText("Reboot", substring = true).assertExists()
    }

    @Test
    fun `a refused push says the device is unaffected`() {
        show(UpdateUiState(pushResult = PushResult.Refused("bad size")))
        rule.onNodeWithText("still running", substring = true).assertExists()
    }

    @Test
    fun `an unchecked build says so instead of claiming to be current`() {
        show(UpdateUiState(status = UpdateStatus.Unknown))
        rule.onNodeWithText("Not checked yet").assertExists()
    }

    @Test
    fun `the install button appears only when a newer release was found`() {
        // Spec §9.5's install path. A button offered with no release behind it is
        // how a user taps it and gets a confusing failure, so it is gated on the
        // NEWER status rather than always present.
        show(UpdateUiState(status = UpdateStatus.Newer(current = "1.0.0", available = "9.9.9")))
        rule.onNodeWithText("Download and install").assertIsEnabled()
    }

    @Test
    fun `with no newer release there is no install button`() {
        // The other half of the gate, in its own test because a Compose test rule
        // allows one `setContent` per test.
        show(UpdateUiState(status = UpdateStatus.Unknown))
        rule.onNodeWithText("Download and install").assertDoesNotExist()
    }

    @Test
    fun `an ahead-of-release device is told, not shown as up to date`() {
        // `NotNewer` is its own state (spec §9.5 step 2): a device ahead of the
        // published release is usually bench-flashed, and saying "up to date" hides
        // that. No install button either -- a downgrade is never offered.
        show(UpdateUiState(status = UpdateStatus.NotNewer(current = "2.0.0", available = "1.0.0")))
        rule.onNodeWithText("No update offered").assertExists()
        rule.onNodeWithText("Download and install").assertDoesNotExist()
    }

    @Test
    fun `a below-floor device is told why the update is not offered`() {
        // Spec §9.5 step 4: `min_from_version` gates an update needing a staged
        // migration, and the user must be told the reason rather than shown nothing.
        show(
            UpdateUiState(
                status = UpdateStatus.TooOldToUpgradeFrom(
                    current = "0.4.0", available = "9.9.9", minFrom = "5.0.0",
                )
            )
        )
        rule.onNodeWithText("Update not offered for this version").assertExists()
        rule.onNodeWithText("5.0.0", substring = true).assertExists()
    }
}
