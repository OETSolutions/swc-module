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
     * Only the app's OWN requests move this: the firmware sends no frame when the
     * window opens or closes, so a window opened by an AUX1 hold is invisible
     * here. It is deliberately not derived from anything else -- the app's
     * maintenance requests are the only ones it can observe, and guessing at the
     * rest would show a state the app has no evidence for.
     */
    val maintenanceOpen: Boolean = false,
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
                        LinkState.Disconnected -> "Not connected"
                    },
                    style = MaterialTheme.typography.headlineSmall,
                )
                state.firmwareVersion?.let {
                    Text("Firmware: $it", style = MaterialTheme.typography.bodyMedium)
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
                        "The device's WiFi is on. Connect to its setup page to " +
                            "configure WiFi or update the firmware. It returns to " +
                            "normal by itself after 5 minutes of no activity."
                    } else {
                        "Turn the device's WiFi on, so it can be configured or " +
                            "updated over the network."
                    },
                    style = MaterialTheme.typography.bodyMedium,
                )
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

    LinkProblem.InMaintenance ->
        "Device is in maintenance mode" to
            "The adapter is serving its WiFi/BLE setup page and is not answering the " +
            "app link. Exit maintenance mode on the device (or let its 5-minute " +
            "timeout expire) and retry."

    LinkProblem.NoDevice ->
        "No device found" to
            "Check the USB-C cable is connected to the adapter and that the adapter " +
            "has power. The adapter keeps working without the app, so a missing link " +
            "does not mean the adapter is broken."
}
