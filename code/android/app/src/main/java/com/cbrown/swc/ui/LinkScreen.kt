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
import com.oetsolutions.swc.link.LinkState

/**
 * Why the app cannot talk to the device, stated so the user can ACT.
 *
 * The plan is explicit that a generic "connection error" is not acceptable, and the
 * reason is practical: the four failures below have four different fixes, and
 * "connection error" tells the user none of them. A version mismatch is not fixed by
 * reseating the cable; a maintenance-mode device is not fixed by reinstalling.
 */
sealed interface LinkProblem {
    /** Android has not granted USB permission for this device. */
    data class NoUsbPermission(val deviceName: String) : LinkProblem

    /** A USB device is attached but it is not the adapter. */
    data class NotOurDevice(val found: String) : LinkProblem

    /** The firmware speaks a different protocol version (spec 4.5). */
    data class VersionMismatch(val firmware: Int, val app: Int) : LinkProblem

    /** The device is in maintenance mode and is not serving the app link. */
    data object InMaintenance : LinkProblem

    /** The cable is not connected, or nothing enumerated. */
    data object NoDevice : LinkProblem
}

data class LinkUiState(
    val link: LinkState = LinkState.Disconnected,
    val firmwareVersion: String? = null,
    val problem: LinkProblem? = null,
)

/**
 * The link screen: is a device connected, what is it running, and if not, why not.
 */
@Composable
fun LinkScreen(
    state: LinkUiState,
    onRetry: () -> Unit,
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
