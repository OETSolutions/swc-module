package com.oetsolutions.swc.ui

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
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
