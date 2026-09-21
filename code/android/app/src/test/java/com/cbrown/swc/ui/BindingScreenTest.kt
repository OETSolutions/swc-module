package com.oetsolutions.swc.ui

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextInput
import com.oetsolutions.swc.contract.ActionKind
import com.oetsolutions.swc.model.Action
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * The Bindings screen's edit path, run on the JVM under Robolectric.
 *
 * **This test exists because the screen could only DELETE.** Each cell's tap went
 * straight to `onEdit(cell, null)`, which clears the binding, and `ActionPicker` —
 * written in the same commit as the screen — was composed nowhere in the app. So
 * there was no way to bind a button to anything; the one tap the screen offered
 * silently removed whatever was there. The suite was green over it because no test
 * rendered this screen at all: only `LadderScreen` had one.
 *
 * The assertions below are about the tap's MEANING: it must open a picker, must
 * offer the kind list, and must commit a chosen action rather than a null.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class BindingScreenTest {

    @get:Rule
    val rule = createComposeRule()

    private fun cell(action: Action? = null) = BindingCell(
        channel = com.oetsolutions.swc.model.BindingChannel.SWC1,
        buttonId = "vol_up",
        buttonName = "Volume Up",
        gesture = "SINGLE",
        action = action,
    )

    @Test
    fun `tapping a cell opens the picker rather than clearing the binding`() {
        val edits = mutableListOf<Action?>()
        rule.setContent {
            BindingScreen(
                state = BindingUiState(cells = listOf(cell())),
                onEdit = { _, a -> edits += a },
                onSave = {},
            )
        }

        rule.onNodeWithText("Not bound").performClick()

        // The picker is open — its kind selector is present — and NOTHING has been
        // sent to onEdit. The previous wiring cleared the binding on this exact tap.
        rule.onNodeWithTag("kind-picker").assertIsDisplayed()
        assertEquals("a bare tap must not edit anything", emptyList<Action?>(), edits)
    }

    @Test
    fun `picking a kind commits that action, not a null`() {
        val edits = mutableListOf<Action?>()
        rule.setContent {
            BindingScreen(
                state = BindingUiState(cells = listOf(cell())),
                onEdit = { _, a -> edits += a },
                onSave = {},
            )
        }

        rule.onNodeWithText("Not bound").performClick()
        rule.onNodeWithTag("kind-picker").performClick()
        // OUT_VOLTAGE is bound to the wheel and needs its key_mv; it is the kind a
        // user reaching for "make this button send a key" would choose.
        rule.onNodeWithText(ActionKind.OUT_VOLTAGE.wireName).performClick()
        rule.onNodeWithTag("key-mv-field").performTextInput("2400")
        rule.onNodeWithText("Apply").performClick()

        assertEquals(1, edits.size)
        assertEquals(ActionKind.OUT_VOLTAGE, edits.single()?.kind)
        assertEquals(2400, edits.single()?.keyMv)
    }

    @Test
    fun `clearing is an explicit choice inside the picker, and only there`() {
        val edits = mutableListOf<Action?>()
        rule.setContent {
            BindingScreen(
                state = BindingUiState(cells = listOf(cell(Action(ActionKind.OUT_VOLTAGE, keyMv = 2400)))),
                onEdit = { _, a -> edits += a },
                onSave = {},
            )
        }

        // The bound cell shows its kind, and its tap opens the picker -- it does
        // NOT clear. Only the picker's own button produces the null.
        rule.onNodeWithText("OUT_VOLTAGE").performClick()
        assertEquals("opening a bound cell must not clear it", emptyList<Action?>(), edits)

        rule.onNodeWithText("Clear binding").performClick()
        assertEquals(1, edits.size)
        assertNull("Clear binding is what produces the null", edits.single())
    }

    @Test
    fun `the picker refuses to apply a kind that needs a parameter until it is given one`() {
        val edits = mutableListOf<Action?>()
        rule.setContent {
            BindingScreen(
                state = BindingUiState(cells = listOf(cell())),
                onEdit = { _, a -> edits += a },
                onSave = {},
            )
        }

        rule.onNodeWithText("Not bound").performClick()
        rule.onNodeWithTag("kind-picker").performClick()
        // APP_LAUNCH takes a `package`; the firmware refuses an empty one, so the
        // screen must not let the user send it.
        rule.onNodeWithText(ActionKind.APP_LAUNCH.wireName).performClick()
        rule.onNodeWithText("Apply").performClick()

        assertEquals("an empty required parameter must not be committed", emptyList<Action?>(), edits)
    }
}
