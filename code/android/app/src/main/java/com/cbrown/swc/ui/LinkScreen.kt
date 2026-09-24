package com.oetsolutions.swc.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.oetsolutions.swc.link.LinkProblem
import com.oetsolutions.swc.link.LinkState

data class LinkUiState(
    val link: LinkState = LinkState.Disconnected,
    val firmwareVersion: String? = null,
    val problem: LinkProblem? = null,
    /**
     * Warnings the device raised (spec 4.3's `log` frame), newest last.
     *
     * The firmware's one producer is FR-18's clamp warning: a stored `key_mv` the
     * device refused to drive as written and clamped instead. Without a place to
     * show it, that warning reaches a host and is dropped on the floor, which is
     * indistinguishable from the device having nothing to say -- exactly the
     * "detected but not reported" defect the warning was added to close.
     */
    val logs: List<String> = emptyList(),
    /**
     * Whether the device is in a maintenance window, as far as the app knows.
     *
     * **The device's `maintenance` frame is the authority** (spec 4.3). It is
     * emitted on connect and on every window transition, so a window opened by an
     * AUX1 hold -- which the app's own requests cannot see -- IS visible here. The
     * app's own enter/exit request still sets it optimistically, so the button
     * reflects the press before the frame comes back; the frame then corrects it
     * if the device disagreed.
     *
     * It used to be moved ONLY by the app's own requests, on the reasoning that
     * the firmware sent no frame. That stopped being true when the radio made the
     * window's state something the app must display: the PoP and the page URL are
     * only useful if the app knows the window is open, and a user who held AUX1
     * saw a card that said nothing was happening.
     */
    val maintenanceOpen: Boolean = false,
    /**
     * The BLE Proof-of-Possession the device is advertising for its current
     * maintenance window (spec 8.3 option 1), or empty.
     *
     * Derived per device from its MAC and carried to the app over the trusted USB
     * link, because the board has no display and no printed label to show it. The
     * user types this into the Espressif provisioning app. Empty when the window
     * is closed or the session is Sec0 (which has no PoP).
     */
    val maintenancePop: String = "",
    /**
     * The web page's `X-SWC-Token` (spec 8.4), or empty. The user needs it (or the
     * `pageUrl`, which carries it) to open the device's setup page.
     */
    val maintenanceToken: String = "",
    /** The setup page's URL, WITH the token in its query string, or empty. */
    val maintenancePageUrl: String = "",
    /** The BLE name to look for in the Espressif app's scan list, or empty. */
    val maintenanceBleName: String = "",
    /**
     * Whether THIS window's radio bring-up failed (spec 4.3's `ble_failures`):
     * 0 means the radio is up, nonzero means it is not.
     *
     * The window and the radio are separate: the orchestrator can open the window
     * while the radio fails to come up (no heap, a driver error). Without this the
     * app would show a setup page that does not exist -- the N-76 shape, a screen
     * promising a capability the device does not have -- so a nonzero value is
     * what lets the card say the radio is not up instead.
     *
     * **It describes the current window, not a running total.** The device clears
     * it when a window closes (N-81), which is what makes it safe to branch on
     * FIRST in the card: a sticky count would make every window after a single
     * failure claim its radio was down, while the same frame carried a live
     * `pageUrl`. Treat any nonzero value as "this window's radio is not up".
     */
    val maintenanceBleFailures: Int = 0,
    /**
     * WHY the maintenance window is open (spec 4.3's `trigger`, spec N-61): one of
     * `usb_command`, `config_flag`, `aux1_hold`, `no_config_at_boot`, or `none`.
     *
     * The device records the trigger, but until N-61 nothing carried it to a
     * user, so a user holding AUX1 at the car saw the same double-flash as a user
     * who opened the window from this app and could not tell which had happened.
     * The card names it, which is the one thing a user standing at the car with no
     * phone needs to know.
     */
    val maintenanceTrigger: String = "",
    /**
     * How long the device's maintenance window lasts, from the config's own
     * `maintenance_timeout_ms` (spec 8.2 bounds it to `(0, K_MAINTENANCE_TIMEOUT_MAX_MS]`).
     *
     * Carried rather than written into the card's copy as a literal: the window is a
     * SETTING, so a device configured for 20 minutes must not be described to the
     * user as 5. Defaulted to the firmware's own 300000 so a screen rendered before
     * the first config arrives still says something true.
     */
    val maintenanceTimeoutMs: Long = 300_000L,
    /** Set while the enter/exit request is in flight, so the button cannot double-fire. */
    val maintenanceBusy: Boolean = false,
    /** Why the last maintenance request did not take effect. Null when it did. */
    val maintenanceProblem: String? = null,
    /**
     * The device's config state from its `status` frame (spec 4.3): one of `ok`,
     * `none`, `recovered` or `defaults`, or null before the first `status`.
     *
     * The firmware already reports this, and spec 4.3 says the field exists so a
     * config fault has "a name the app could read" -- §6.8's corrupt-config
     * response is `config_state: defaults`. Until this was read the app had no way
     * to tell a device running the user's config from one that silently fell back
     * to defaults, which is the exact fault the named field was added to surface.
     *
     * `none` is NOT a fault: it is FR-25's supported pass-through device (one with
     * no config at all, which still serves presses). `defaults` and `recovered`
     * are the two the user must be told about.
     */
    val configState: String? = null,
    /**
     * How many frames the DEVICE reported missing on the link (spec 4.3's
     * `link_gap`), or 0 if none.
     *
     * The firmware tracks the app's outgoing `seq` and emits a `link_gap` frame
     * when it skips ahead, because a lost command is otherwise indistinguishable
     * from a command the device ignored. The app defined the frame constant and
     * then dropped the frame: it was the ONE inbound type with no handler, so a
     * dropped `config_chunk` or `learn_commit` left the user with a transfer that
     * went nowhere and nothing anywhere saying why. Counting them makes a flaky
     * cable visible, which is the only way the user can tell "the device refused"
     * from "the bytes never arrived".
     */
    val lostFrames: Int = 0,
    /**
     * Frames the DEVICE's transport refused: outbound (`tx_dropped`, its TX
     * buffer was full) and inbound (`rx_overflows`, its staging ring overflowed).
     *
     * The other direction from [lostFrames]. Both counters existed on the device
     * and were read by nothing but a unit test -- `DroppedFrames` even documents
     * itself as "the failure this class exists to prevent, so it must be
     * observable", while no user could observe it. An inbound overflow means a
     * command the app sent was DISCARDED before the firmware parsed it, so the
     * operation fails with no nack and no error: from the app's side it is
     * indistinguishable from a device that ignored the request, which is exactly
     * what `link_gap` was added for at the app-to-device boundary.
     *
     * Cumulative since the device BOOTED -- the transport increments them and
     * never clears them, not even across a disconnect (spec 4.4 makes reconnect
     * stateless for what is IN FLIGHT, and a loss count is a history, not flight
     * state). So a non-zero value means "this cable has lost frames", not
     * "something just broke", and the count keeps rising over a long session
     * rather than resetting each time the app reconnects.
     */
    val deviceTxDropped: Int = 0,
    val deviceRxOverflows: Int = 0,
    /**
     * The device's last reported NTC temperature in degrees C, or null when no
     * reading has been measured (spec 4.3's `status.temp_c`).
     *
     * Null is a MEANINGFUL value, not "not yet arrived": the firmware reports JSON
     * null until a conversion is good, because 0 C is a legal temperature and a
     * bare `0.0` would be indistinguishable from a measured freezing board. Kept
     * as null here so the renderer can say "no reading" rather than show a false
     * zero.
     */
    val lastTempC: Double? = null,
    /**
     * The device's free heap in bytes at the last `status`, or null before one.
     *
     * A `Long` because the figure is a byte count that exceeds `Int` on the S3's
     * total RAM budget in the general case, and truncating it would be a wrong
     * number rather than a missing one.
     */
    val lastHeapFree: Long? = null,
    /**
     * App-side actions the device's press asked for and this app could not run.
     *
     * Spec 3.6 splits the library: the `OUT_` family is the firmware's and
     * everything else is Android's. The device confirms a recognized press with
     * `event` and releases the line for an app-side kind rather than hold a key
     * with no action behind it, so when the app cannot run its half the press is
     * silent on BOTH sides -- nothing on the wire and nothing on the phone.
     *
     * `AppViewModel.actionOutcomes` computed exactly this and NO screen rendered
     * it, so the user's only evidence was a button that did nothing. The most
     * recent press's outcome is what is shown: `runAppSideAction` replaces the
     * list rather than appending, so the message describes the last press rather
     * than accumulating one line per press for the rest of the session.
     */
    val actionProblems: List<String> = emptyList(),
) {
    /**
     * The config fault to show, or null when there is nothing to say.
     *
     * Deliberately a plain `when` rather than a message built in the view model:
     * this is the wording, and wording belongs beside the screen that shows it.
     */
    val configWarning: String?
        get() = when (configState) {
            "defaults" ->
                "The device lost its configuration and is running factory defaults. " +
                    "Your learned buttons and bindings are gone. Program it again from " +
                    "the Bindings screen."
            "recovered" ->
                "The device's saved configuration was damaged and it restored a backup. " +
                    "Check the Bindings screen to confirm your buttons are right."
            else -> null
        }
}

/**
 * The maintenance window's length, in words.
 *
 * **It deliberately says how long the window lasts rather than spelling out the
 * close policy.** Spec 8.2 and FR-38 describe the close as "5 minutes of
 * INACTIVITY", and the device now implements exactly that (N-35, resolved with
 * the radio: the maintenance HTTP server's requests are the activity, and the
 * window is measured from the last one) — but the user-facing sentence is the
 * same either way, because "the mode returns to normal by itself after N
 * minutes" is true of both readings and is not a promise about what extends it.
 * An earlier revision of this comment described a fixed-deadline firmware; that
 * was accurate when it was written and is not any more.
 *
 * **Rounded UP, never down.** Telling a user the window is shorter than it is
 * makes them rush; telling them it is longer is how they get cut off mid-task. A
 * sub-minute timeout (legal: the codec accepts any value in the range) is
 * therefore "under a minute" rather than "0 minutes".
 */
internal fun describeTimeout(timeoutMs: Long): String {
    if (timeoutMs <= 0) return "after a short delay"
    if (timeoutMs < 60_000L) return "after under a minute"
    val minutes = (timeoutMs + 59_999L) / 60_000L
    return if (minutes == 1L) "after 1 minute" else "after $minutes minutes"
}

/**
 * A maintenance trigger's wire word as a phrase a user can act on (spec N-61).
 *
 * The point is to distinguish the paths a user cannot otherwise tell apart -- the
 * AUX1 hold from the app, especially. An unrecognised word is shown verbatim
 * rather than mapped to "unknown": a firmware that adds a trigger should have its
 * word visible so the app is not silently lying about it, the same rule the config
 * decoder follows for unknown enum names.
 */
internal fun describeTrigger(trigger: String): String = when (trigger) {
    "usb_command" -> "the app, over USB"
    "config_flag" -> "the config's maintenance-on-boot flag"
    "aux1_hold" -> "holding AUX1 on the device (no app needed)"
    "no_config_at_boot" -> "booting with no saved config"
    "none" -> "an unrecorded reason"
    else -> trigger
}

/**
 * One per-device secret, labelled, for the maintenance card (spec 8.3 option 1).
 *
 * A small helper rather than two inline `Text`s because the pair must render
 * IDENTICALLY -- a user reads one off the screen and types it into the Espressif
 * app, and a differently-styled second row is where a copy-paste error starts.
 * The value is monospace-free and on its own line so it is selectable and
 * unmistakable against the surrounding prose.
 */
@Composable
private fun SecretRow(label: String, value: String) {
    Column {
        Text(label, style = MaterialTheme.typography.labelSmall)
        Text(value, style = MaterialTheme.typography.titleMedium)
    }
}

/**
 * The link screen: is a device connected, what is it running, and if not, why not.
 */
@Composable
fun LinkScreen(
    state: LinkUiState,
    onRetry: () -> Unit,
    onEnterMaintenance: () -> Unit,
    onExitMaintenance: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("Device", style = MaterialTheme.typography.titleMedium)
                Text(
                    when (state.link) {
                        is LinkState.Connected -> "Connected"
                        is LinkState.VersionMismatch -> "Version mismatch"
                        is LinkState.Failed -> "Failed"
                        // Not "Connected" and not "Failed": the app has heard
                        // nothing for 10 s (spec §4.4, N-27). Saying "Connected"
                        // over a dead cable is the exact lie this state exists to
                        // stop showing.
                        LinkState.SilenceExpired -> "Not responding"
                        LinkState.Disconnected -> "Not connected"
                    },
                    style = MaterialTheme.typography.headlineSmall,
                )
                state.firmwareVersion?.let {
                    Text("Firmware: $it", style = MaterialTheme.typography.bodyMedium)
                }
                // The device's own health figures (spec 4.3's `status`, N-22).
                // Shown only once a `status` has arrived, and the temperature says
                // "no reading" rather than a false 0 C when the device has measured
                // nothing. These are diagnostics, so they sit with "Firmware:"
                // rather than in a warning card -- a non-zero temperature is not a
                // problem, and neither is a heap figure on its own.
                state.lastTempC?.let {
                    Text("Board temp: $it C", style = MaterialTheme.typography.bodyMedium)
                } ?: run {
                    // Only claim "no reading" once the link is up and a status has
                    // been seen at all; before the first frame, saying so would be
                    // noise. `lastHeapFree` shares the first-status signal.
                    if (state.lastHeapFree != null) {
                        Text("Board temp: no reading", style = MaterialTheme.typography.bodyMedium)
                    }
                }
                state.lastHeapFree?.let {
                    Text("Free heap: $it bytes", style = MaterialTheme.typography.bodyMedium)
                }
            }
        }

        // Every problem is its own card with its own guidance. The `when` is
        // exhaustive on purpose: a new LinkProblem is a compile error here rather
        // than a silently generic message.
        state.problem?.let { problem ->
            val (title, detail) = describe(problem)
            Card(
                Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = Color(0xFFFFF3E0)),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text(title, style = MaterialTheme.typography.titleMedium)
                    Text(detail, style = MaterialTheme.typography.bodyMedium)
                }
            }
        }

        state.configWarning?.let { warning ->
            Card(
                Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = Color(0xFFFFEBEE)),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text("Configuration problem", style = MaterialTheme.typography.titleMedium)
                    Text(warning, style = MaterialTheme.typography.bodyMedium)
                }
            }
        }

        val deviceLoss = state.deviceTxDropped + state.deviceRxOverflows
        if (deviceLoss > 0) {
            Card(
                Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = Color(0xFFFFF3E0)),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text("The adapter lost frames", style = MaterialTheme.typography.titleMedium)
                    Text(
                        buildString {
                            append("The adapter's own link dropped ")
                            append(deviceLoss)
                            append(if (deviceLoss == 1) " frame" else " frames")
                            append(":\n")
                            if (state.deviceTxDropped > 0) {
                                append("  \u2022 ")
                                append(state.deviceTxDropped)
                                append(" it could not send (USB buffer full)")
                                append("\n")
                            }
                            if (state.deviceRxOverflows > 0) {
                                append("  \u2022 ")
                                append(state.deviceRxOverflows)
                                append(" it could not receive (came in faster than it ")
                                append("could parse)")
                            }
                        }.trimEnd(),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    Text(
                        "A frame the adapter could not receive was discarded BEFORE it " +
                            "was parsed, so the command may have failed with no error " +
                            "at all. Re-seat the USB cable and try the operation again.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }

        if (state.lostFrames > 0) {
            Card(
                Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = Color(0xFFFFF3E0)),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text("Lost frames", style = MaterialTheme.typography.titleMedium)
                    Text(
                        "The adapter reported ${state.lostFrames} frame" +
                            (if (state.lostFrames == 1) "" else "s") +
                            " missing on the link. A command may not have reached the " +
                            "device. Check the USB cable and try the operation again.",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        }

        /*
         * An app-side action the device asked this phone to run and the phone
         * refused. Its own card, and NOT folded into the device-warning list
         * above: those come from the device over `log`, while this happened HERE,
         * and the fix is on the phone (a permission, another app) rather than on
         * the adapter. Merging them would send the user to check the wrong end.
         */
        if (state.actionProblems.isNotEmpty()) {
            Card(
                Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = Color(0xFFFFEBEE)),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text("A button's action did not run", style = MaterialTheme.typography.titleMedium)
                    state.actionProblems.forEach {
                        Text(it, style = MaterialTheme.typography.bodyMedium)
                    }
                    Text(
                        "The adapter still sent the key press. This is the phone's half " +
                            "of the action (spec 3.6) — the button itself is configured " +
                            "correctly.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }

        if (state.logs.isNotEmpty()) {
            Card(
                Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = Color(0xFFFFF3E0)),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text("Device warnings", style = MaterialTheme.typography.titleMedium)
                    // Newest LAST, so the most recent warning is the one nearest
                    // the bottom edge and the user does not have to scroll to find
                    // what just happened.
                    state.logs.forEach {
                        Text(it, style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }

        /*
         * Maintenance (spec 8.2). Spec 8.2 calls the USB command the PRIMARY way
         * in and warns that the car "may have no WiFi" -- so this is the control
         * that turns the device's radio on, and without it a user has no path to
         * the provisioning page from the app at all.
         *
         * One button that flips, rather than two: the window has exactly two
         * states and the app knows which one it asked for, so two permanent
         * buttons would leave one of them always wrong to press.
         */
        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("Maintenance mode", style = MaterialTheme.typography.titleMedium)
                Text(
                    if (state.maintenanceOpen) {
                        // **It must describe the radio the DEVICE reports, not the
                        // one we hope for.** The window and the radio are separate
                        // states: the orchestrator opens the window and the device
                        // then brings up NimBLE + the page. `ble_failures` is how
                        // the app learns the second half did not happen, and this
                        // is the branch that must not promise a setup page that is
                        // not there (the N-76 shape: a user-facing surface
                        // asserting a capability the device does not have).
                        when {
                            state.maintenanceBleFailures > 0 ->
                                "The device is in maintenance mode, but its radio did " +
                                    "not come up, so there is no setup page to open " +
                                    "yet. The mode returns to normal by itself " +
                                    "${describeTimeout(state.maintenanceTimeoutMs)}."
                            state.maintenancePageUrl.isNotEmpty() ->
                                "The device's setup page is at ${state.maintenancePageUrl}. " +
                                    "Its radio is up: join the \"SWC-…\" network it is " +
                                    "broadcasting with the Espressif provisioning app, " +
                                    "or open the page in a browser. The mode returns to " +
                                    "normal by itself " +
                                    "${describeTimeout(state.maintenanceTimeoutMs)}."
                            else ->
                                // The window is open and the radio has neither
                                // succeeded nor failed yet -- the frame arrives a
                                // moment before the bring-up finishes. Saying
                                // nothing about the page yet is the honest answer.
                                "The device is in maintenance mode and is bringing " +
                                    "its radio up. The mode returns to normal by " +
                                    "itself ${describeTimeout(state.maintenanceTimeoutMs)}."
                        }
                    } else {
                        "Put the device into maintenance mode to set up the device's " +
                            "WiFi and update its firmware over the network. Holding " +
                            "AUX1 for 3 seconds does the same from the device itself, " +
                            "with no app."
                    },
                    style = MaterialTheme.typography.bodyMedium,
                )
                // WHY the window opened (spec N-61). Rendered only while it IS open:
                // a line reading "Opened by: …" under a "not in maintenance mode"
                // card would be the confusion the field exists to remove.
                if (state.maintenanceOpen && state.maintenanceTrigger.isNotEmpty() &&
                    state.maintenanceTrigger != "none") {
                    Text(
                        "Opened by: ${describeTrigger(state.maintenanceTrigger)}",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                // The two per-device secrets, shown over this already-trusted link
                // because the board has no display (spec 8.3 option 1). They are
                // rendered only when the device says the radio is actually up, so
                // a user is never handed a credential for a session that is not
                // listening.
                if (state.maintenanceOpen && state.maintenanceBleFailures == 0) {
                    if (state.maintenancePop.isNotEmpty()) {
                        SecretRow("BLE passcode", state.maintenancePop)
                    }
                    if (state.maintenanceBleName.isNotEmpty()) {
                        SecretRow("Bluetooth name", "SWC-${state.maintenanceBleName}")
                    }
                }
                Button(
                    onClick = if (state.maintenanceOpen) onExitMaintenance else onEnterMaintenance,
                    enabled = !state.maintenanceBusy &&
                        state.link is com.oetsolutions.swc.link.LinkState.Connected,
                ) {
                    Text(
                        when {
                            state.maintenanceBusy -> "Working…"
                            state.maintenanceOpen -> "Leave maintenance mode"
                            else -> "Enter maintenance mode"
                        },
                    )
                }
                state.maintenanceProblem?.let {
                    Text(it, style = MaterialTheme.typography.bodySmall)
                }
            }
        }

        Button(onClick = onRetry) { Text("Try again") }
    }
}

/** The message and the fix, per problem. */
internal fun describe(problem: LinkProblem): Pair<String, String> = when (problem) {
    is LinkProblem.NoUsbPermission ->
        "USB permission needed" to
            "Android has not been granted access to ${problem.deviceName}. Tap " +
            "\"Try again\", then allow the USB access prompt. If no prompt appears, " +
            "replug the adapter — the prompt only shows on a fresh attach."

    is LinkProblem.NotOurDevice ->
        "That is not the adapter" to
            "A USB device was found (${problem.found}) but it does not speak the SWC " +
            "protocol. Another USB device may be sharing the hub — unplug it and retry."

    is LinkProblem.VersionMismatch ->
        "Firmware and app versions differ" to
            "The device speaks protocol ${problem.firmware}; this app speaks " +
            "${problem.app}. Nothing will be sent, because a partial match is how a " +
            "config gets corrupted. Update the app or the firmware so both match."

    is LinkProblem.LinkFailed ->
        "The link failed" to
            "${problem.reason}. The adapter is reachable — this was the conversation, " +
            "not the cable. Retry, and if it repeats, replug the adapter so the " +
            "transfer starts clean."

    LinkProblem.NoDevice ->
        "No device found" to
            "Check the USB-C cable is connected to the adapter and that the adapter " +
            "has power. The adapter keeps working without the app, so a missing link " +
            "does not mean the adapter is broken."

    LinkProblem.SilenceExpired ->
        "The adapter stopped responding" to
            "It answered normally and then went quiet for 10 seconds. Check the " +
            "USB-C cable is still connected and that the adapter still has power — " +
            "a button press still works even while the app cannot see it. Reopen " +
            "this app or tap \"Try again\" once it is back."
}
