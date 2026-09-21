package com.oetsolutions.swc.ui

import androidx.compose.ui.test.assertIsDisplayed
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
        // 1500 is 70 mV from vol_up's 1430 and inside its ±120 window.
        assertEquals("vol_up", state(liveMv = 1500).matched()?.id)
    }

    @Test
    fun `a reading just outside the band matches nothing`() {
        // 1430 + 121 is one millivolt outside the window. The boundary is asserted
        // rather than a far-away value, because an off-by-one here is exactly the
        // kind of gap the firmware would disagree about.
        assertNull(state(liveMv = 1430 + 121).matched())
        assertEquals("vol_up", state(liveMv = 1430 + 120).matched()?.id)
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
            UpdateScreen(state = state, onCheck = {}, onPushOverUsb = {}, onUpdateOverWifi = {})
        }
    }

    @Test
    fun `the two update paths are visibly unavailable, not silently inert`() {
        // Both are unimplemented: the USB path needs the `ota_*` frames the firmware
        // answers with `not_implemented`, and the WiFi path opens a maintenance page
        // that does not exist. A live-looking button that does nothing cannot be
        // told from a broken one, so the screen says so.
        show(UpdateUiState())
        rule.onNodeWithText("Push a file over USB (not yet)").assertIsNotEnabled()
        rule.onNodeWithText("Update over WiFi (not yet)").assertIsNotEnabled()
    }

    @Test
    fun `an unchecked build says so instead of claiming to be current`() {
        show(UpdateUiState(status = UpdateStatus.Unknown))
        rule.onNodeWithText("Not checked yet").assertExists()
    }
}
