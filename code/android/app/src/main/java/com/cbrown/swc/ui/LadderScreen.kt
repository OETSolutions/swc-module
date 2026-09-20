package com.oetsolutions.swc.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.oetsolutions.swc.model.Gesture
import kotlin.math.abs

/**
 * One learned button as the live view needs it.
 *
 * **The window is stored in MILLIVOLTS, and the ratio is DERIVED for display.**
 * Spec 3.4 stores `mv_center`/`mv_tolerance` and the firmware computes the ratio at
 * classify time. An app that kept its own storable permille copy would be a second
 * home for the same window, and the two would drift -- which is how the app and the
 * firmware come to disagree about where a button is.
 */
data class LearnedButton(
    val id: String,
    val name: String,
    val mvCenter: Int,
    val mvTolerance: Int,
)

data class LadderUiState(
    /** The idle ("rail") reading the ratios are taken against. */
    val idleMv: Int,
    val buttons: List<LearnedButton>,
    /** The current reading, or null when no samples have arrived yet. */
    val liveMv: Int? = null,
    val channelName: String = "SWC1",
    /**
     * The gesture of the most recent `event` (spec 4.3), and the button it was on.
     *
     * **This is what makes the live view a DIAGNOSTIC rather than a voltmeter.**
     * A live millivolt reading that merely matches a band tells the user where the
     * line is; it does not tell them what the DEVICE decided. Those differ exactly
     * when something is wrong — a reading inside the window the classifier rejected
     * as undecided, or a press that resolved to a different button than the one the
     * user thought they held. Showing the reported gesture closes that gap: "the
     * device saw vol_up, and called it a LONG" is actionable, and "1430 mV" is not.
     */
    val lastGesture: Gesture? = null,
    val lastGestureButton: String? = null,
) {
    /**
     * The firmware's own ratio: level/idle x 1000 (`LadderRatioPermille`).
     *
     * Note the direction -- "at idle" is ~1000, NOT ~0. A view that assumed the
     * usual "0 is idle" convention would render every band mirrored.
     */
    fun ratioPermille(mv: Int): Int =
        if (idleMv <= 0) 0 else (mv.toLong() * 1000 / idleMv).toInt()

    /** The button whose window contains the live reading, if any. */
    fun matched(): LearnedButton? {
        val live = liveMv ?: return null
        return buttons.firstOrNull { abs(live - it.mvCenter) <= it.mvTolerance }
    }
}

/**
 * The live ladder: the diagnostic that separates an adapter fault from a head-unit
 * fault.
 *
 * **The user must be able to see WHICH button the device currently classifies.**
 * That is the screen's whole value: if the device thinks "vol_up" is held while the
 * user presses "next", the adapter is the problem; if it agrees and the stereo still
 * does nothing, the head unit is. Without the match marked, the user has a voltage
 * number and no way to act on it.
 *
 * Each band is placed by the DERIVED ratio against [LadderUiState.idleMv], which is
 * what makes the view rail-invariant (spec 6.3): the same button sits in the same
 * place whether the rail is 3.14 V or 3.47 V, because the firmware classifies by
 * ratio too.
 */
@Composable
fun LadderScreen(state: LadderUiState, modifier: Modifier = Modifier) {
    val matched = state.matched()
    Column(
        modifier = modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text(
                    "Live ladder — ${state.channelName}",
                    style = MaterialTheme.typography.titleMedium,
                )
                Text("Rail (idle): ${state.idleMv} mV", style = MaterialTheme.typography.bodyMedium)
                Text(
                    state.liveMv?.let { "Reading: $it mV" } ?: "Reading: —",
                    style = MaterialTheme.typography.headlineSmall,
                )
                Text(
                    matched?.let { "Classified as: ${it.name}" } ?: "Classified as: nothing",
                    style = MaterialTheme.typography.bodyLarge,
                )
                // The gesture the DEVICE reported, which is not derivable from the
                // reading: the classifier answers "which window", and the gesture
                // machine answers "single, double or long". Showing both is what
                // lets a user tell an adapter fault from a head-unit fault.
                state.lastGesture?.let { g ->
                    Text(
                        "Last gesture: ${state.lastGestureButton ?: "?"} ${g.wireName}",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        }

        if (state.idleMv > 0) {
            BoxWithConstraints(Modifier.fillMaxWidth().height(120.dp)) {
                val widthPx = constraints.maxWidth.toFloat()
                // Position a millivolt reading on the rail. The scale runs 0..idle,
                // so a lower voltage is further LEFT -- which is the same axis the
                // firmware's ratio uses.
                fun xOf(mv: Int): Float = (mv.toFloat() / state.idleMv) * widthPx

                // The rail marker at the right edge, so the scale is legible.
                Box(
                    Modifier
                        .offset(x = (widthPx - 2f).dp)
                        .width(2.dp)
                        .fillMaxHeight()
                        .background(Color(0xFF424242))
                )

                state.buttons.forEach { b ->
                    val isMatched = matched?.id == b.id
                    val centre = xOf(b.mvCenter)
                    val bandW = xOf(b.mvTolerance) * 2f
                    Box(
                        Modifier
                            .offset(x = (centre - bandW / 2f).dp)
                            .width(bandW.coerceAtLeast(1f).dp)
                            .fillMaxHeight()
                            .background(
                                bandColor(isMatched),
                                RoundedCornerShape(4.dp),
                            )
                            .semantics {
                                contentDescription =
                                    "${b.name}, ${if (isMatched) "matched" else "not matched"}"
                            }
                            .testTag("band-${b.id}")
                    )
                }

                state.liveMv?.let { live ->
                    Box(
                        Modifier
                            .offset(x = (xOf(live) - 1f).dp)
                            .width(3.dp)
                            .fillMaxHeight()
                            .background(Color(0xFFD32F2F))
                            .testTag("live-marker")
                    )
                }
            }

            // A legend, because a band colour with no key is a decoration.
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                state.buttons.forEach { b ->
                    val isMatched = matched?.id == b.id
                    Text(
                        "${b.name}: ${b.mvCenter} mV ±${b.mvTolerance} " +
                            "(ratio ${state.ratioPermille(b.mvCenter)})" +
                            if (isMatched) "  ← device classifies this" else "",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        } else {
            Text(
                "This channel has no learned rail yet. Learn a button first.",
                style = MaterialTheme.typography.bodyMedium,
            )
        }
    }
}

/**
 * The colour a band is drawn in. Matched is the accent, unmatched is muted.
 *
 * Colour alone is never the signal -- the content description says "matched" in
 * words -- because a colour-blind user must get the same diagnostic.
 */
internal fun bandColor(matched: Boolean): Color =
    if (matched) Color(0xFF1565C0) else Color(0xFF9E9E9E)
