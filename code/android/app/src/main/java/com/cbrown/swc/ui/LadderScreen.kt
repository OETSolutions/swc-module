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
    /**
     * The +3V3 rail the button was LEARNED at (spec 3.4 `learned_at_rail_mv`), in
     * millivolts. Shown because FR-30 promises the app can "display absolute
     * millivolts" -- a reading is only interpretable against the rail it was taken
     * on, and a user comparing two learns needs to know the rail did not move.
     */
    val learnedAtRailMv: Int = 3300,
)

data class LadderUiState(
    /**
     * The LEARN-TIME idle ("rail"), from the config's `learned_idle_mv`.
     *
     * This is the denominator the bands are PLACED against: the stored
     * `mv_center` values were measured at this rail, so their ratio against it is
     * rail-invariant and every band stays in the same screen position as the real
     * rail moves.
     */
    val idleMv: Int,
    val buttons: List<LearnedButton>,
    /** The current reading, or null when no samples have arrived yet. */
    val liveMv: Int? = null,
    /**
     * The LIVE idle the last `event` was normalized against (spec 6.3's
     * `V_ADC_idle`), or null when none has arrived.
     *
     * This is the denominator the device's `LadderClassify` actually used, so it
     * is what the MATCH must run against -- not [idleMv], which is the learn-time
     * rail. Keeping the two separate is the whole point: the bands are placed by
     * the invariant ratio, the match is decided by the live one.
     */
    val liveIdleMv: Int? = null,
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
     * The firmware's own ratio: level/idle x 1000 (`LadderRatioPermille`),
     * including its rounding (`(mv*1000 + idle/2)/idle`).
     *
     * Note the direction -- "at idle" is ~1000, NOT ~0. A view that assumed the
     * usual "0 is idle" convention would render every band mirrored.
     */
    fun ratioPermille(mv: Int): Int = ratioOf(mv, idleMv)

    /**
     * The button whose window contains the live reading, if any.
     *
     * **This reproduces the firmware's decision** (`LadderClassify`,
     * LadderDecode.cpp): normalize the reading and each button's centre/tolerance
     * against the LEARNED idle to permille, then take the nearest centre within
     * its half-width. The device normalizes the reading against the LIVE idle and
     * the centres against the LEARNED idle, and so does this -- [liveIdleMv] is
     * the denominator for the reading, [idleMv] for the windows. Running both on
     * the same denominator was the earlier bug: the device cancels the rail by
     * dividing a live reading by a live idle, while comparing the stored centre
     * to the learned rail, so a view that compared absolute millivolts (or used
     * one idle for both) disagreed with the device the moment the rail moved
     * (spec N-25).
     *
     * The idle-band and fault guards are deliberately NOT reproduced here: this
     * screen explains a press the device already classified, and re-deriving
     * "would the device have called this a fault" from a frame that arrived is a
     * second classifier that can drift. The frame's own `button` field already
     * says what the device decided; this is the picture behind that answer.
     */
    fun matched(): LearnedButton? {
        val live = liveMv ?: return null
        // The live idle when the event carried one; otherwise fall back to the
        // learn-time idle, which is the best available denominator and no worse
        // than the old absolute-millivolt test.
        val denom = liveIdleMv?.takeIf { it > 0 } ?: idleMv
        if (denom <= 0 || idleMv <= 0) return null
        val ratio = ratioOf(live, denom)
        var best: LearnedButton? = null
        var bestDistance = 0
        for (b in buttons) {
            val centre = ratioOf(b.mvCenter, idleMv)
            val half = ratioOf(b.mvTolerance, idleMv)
            val distance = abs(ratio - centre)
            if (distance > half) continue
            if (best == null || distance < bestDistance) {
                best = b
                bestDistance = distance
            }
        }
        return best
    }

    /** The firmware's own rounding: `(mv * 1000 + idle/2) / idle` (`LadderRatioPermille`). */
    private fun ratioOf(mv: Int, idle: Int): Int =
        if (idle <= 0) 0 else ((mv.toLong() * 1000 + idle / 2) / idle).toInt()

    /**
     * FR-30's sag detection: true when the LIVE idle has collapsed toward zero
     * relative to the learned one -- a regulator fault, not a press.
     *
     * **The threshold is the firmware's, not a new one.** `LadderClassify`
     * (`LadderDecode.cpp`) rejects a profile whose `idle_mv` falls below
     * `learned_idle_mv * [kRailHealthFloorPermille]/1000` and raises `kFault`. The
     * app mirrors that test so the screen and the device agree about what a
     * collapsed rail is; a second, independently-chosen threshold would let the UI
     * say "healthy" while the device is faulting, which is the exact
     * disagreement N-77 exists to remove. The floor is a PERMILLE figure applied to
     * the idle, not the 20 % of the reading the earlier spec prose (wrongly) named.
     *
     * Null when there is nothing to compare -- no live idle yet, an unlearned
     * channel, or a frame whose denom is the AUX nominal rather than a real wheel
     * idle -- so the screen shows nothing rather than a false alarm.
     */
    fun railSagging(): Boolean? {
        val live = liveIdleMv ?: return null
        if (idleMv <= 0 || live <= 0) return null
        return live < (idleMv.toLong() * kRailHealthFloorPermille / 1000).toInt()
    }
}

/**
 * FR-30's rail-health floor, in permille of the learned idle.
 *
 * One home, and it mirrors `kRailHealthFloorPermille` in
 * `lib/Analog/LadderDecode.cpp` (200, i.e. a collapse to <=20 %). Kept as a named
 * app constant rather than inlined so the gate that pins the app's numbers against
 * the firmware's can find it -- `tools/check_app_limits.py` reads the firmware's
 * limits, and this is the same "the app must not invent its own threshold" rule the
 * config limits follow.
 */
internal const val kRailHealthFloorPermille = 200

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
 * Each band is placed by the DERIVED ratio against [LadderUiState.idleMv]: the
 * centre and the denominator are BOTH learn-era millivolts measured at the same
 * rail, so their ratio is rail-invariant and the band sits in the same place
 * whether the rail is 3.14 V or 3.47 V.
 *
 * **The placement is invariant; the MATCH is not.** [LadderUiState.matched] compares
 * absolute millivolts, so it can disagree with the device on a moved rail — see its
 * comment and spec N-25.
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
                // FR-30's display clause: the +3V3 rail each button was LEARNED at.
                // Shown per-button because a re-learn can move it, and a user with
                // two buttons learned on different rails needs to see that.
                state.buttons.map { it.learnedAtRailMv }.distinct().forEach { rail ->
                    Text(
                        "Learned at +3V3 rail: $rail mV",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                Text(
                    state.liveMv?.let { "Reading: $it mV" } ?: "Reading: —",
                    style = MaterialTheme.typography.headlineSmall,
                )
                Text(
                    matched?.let { "Classified as: ${it.name}" } ?: "Classified as: nothing",
                    style = MaterialTheme.typography.bodyLarge,
                )
                // FR-30's detectability clause: a live idle collapsed toward zero is
                // a regulator fault, and saying so is the difference between "your
                // adapter is broken" and "your car's 3V3 is failing".
                if (state.railSagging() == true) {
                    Text(
                        "Rail fault: the +3V3 supply has sagged to ${state.liveIdleMv} mV " +
                            "(was ${state.idleMv} mV at learn). This is a regulator fault, " +
                            "not a button press.",
                        style = MaterialTheme.typography.bodyLarge,
                        color = Color(0xFFD32F2F),
                        modifier = Modifier.semantics {
                            contentDescription = "rail fault, +3V3 sagged"
                        }.testTag("rail-fault"),
                    )
                }
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
            BoxWithConstraints(Modifier.fillMaxWidth().height(120.dp).testTag("ladder-scale")) {
                // `maxWidth` (the scope's Dp), NOT `constraints.maxWidth` (px):
                // every consumer below applies the result through `.dp`, so the
                // scale must be measured in dp or it is scaled by the density a
                // second time. At density 1 the two agree, which is why a JVM test
                // at the default density cannot see the difference.
                val widthDp = maxWidth.value
                // Position a millivolt reading on the rail. The scale runs 0..idle,
                // so a lower voltage is further LEFT -- which is the same axis the
                // firmware's ratio uses.
                fun xOf(mv: Int): Float = (mv.toFloat() / state.idleMv) * widthDp

                // The rail marker at the right edge, so the scale is legible.
                Box(
                    Modifier
                        .offset(x = (widthDp - 2f).dp)
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
