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
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp

/** The result of asking about updates, stated plainly. */
sealed interface UpdateStatus {
    data object Unknown : UpdateStatus
    data class UpToDate(val version: String) : UpdateStatus
    data class Newer(val current: String, val available: String) : UpdateStatus

    /** The available image is for different hardware; flashing it would brick the device. */
    data class WrongBoard(val available: String, val deviceBoard: String) : UpdateStatus
    data class Failed(val reason: String) : UpdateStatus
}

data class UpdateUiState(
    val currentVersion: String = "—",
    val status: UpdateStatus = UpdateStatus.Unknown,
    val inProgress: Boolean = false,
)

/**
 * The update screen.
 *
 * **The screen must state that the device keeps working if the update fails.** The
 * user's real question is "will this brick my stereo", and an update UI that is
 * silent about it invites the user to guess — usually by not updating at all. The
 * firmware's guarantee is real and specific (spec 9: a failed update leaves the
 * running image in its slot and rolls back), so the screen says it.
 *
 * Two explicit paths, because they have different requirements: pushing a file over
 * USB needs the cable and the app, and updating over WiFi needs the device on a
 * network but not the phone.
 */
@Composable
fun UpdateScreen(
    state: UpdateUiState,
    onCheck: () -> Unit,
    onPushOverUsb: () -> Unit,
    onUpdateOverWifi: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("Firmware", style = MaterialTheme.typography.titleMedium)
                Text(state.currentVersion, style = MaterialTheme.typography.headlineSmall)
            }
        }

        // The status, stated as one of four plain outcomes. "Newer" is only offered
        // when the image is for THIS board -- a wrong-board image is its own state,
        // not a "newer" the user can tap.
        val (title, detail) = describeStatus(state.status)
        Card(
            Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(
                containerColor = when (state.status) {
                    is UpdateStatus.WrongBoard, is UpdateStatus.Failed -> Color(0xFFFFEBEE)
                    else -> Color(0xFFF5F5F5)
                }
            ),
        ) {
            Column(Modifier.padding(16.dp)) {
                Text(title, style = MaterialTheme.typography.titleMedium)
                Text(detail, style = MaterialTheme.typography.bodyMedium)
            }
        }

        Button(onClick = onCheck, enabled = !state.inProgress, modifier = Modifier.testTag("check-updates")) {
            Text(if (state.inProgress) "Checking…" else "Check for updates")
        }
        OutlinedButton(onClick = onPushOverUsb, enabled = !state.inProgress) {
            Text("Push a file over USB")
        }
        OutlinedButton(onClick = onUpdateOverWifi, enabled = !state.inProgress) {
            Text("Update over WiFi")
        }

        Card(
            Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = Color(0xFFE8F5E9)),
        ) {
            Column(Modifier.padding(16.dp)) {
                Text("If an update fails", style = MaterialTheme.typography.titleSmall)
                Text(
                    "Your adapter keeps working. The new image is written to the " +
                        "unused slot and only becomes active once the device proves it " +
                        "can still drive the output; if it cannot, the device reboots " +
                        "back into the version you are running now. A failed or " +
                        "interrupted update does not leave you with a dead adapter.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

internal fun describeStatus(status: UpdateStatus): Pair<String, String> = when (status) {
    UpdateStatus.Unknown -> "Not checked yet" to
        "Tap \"Check for updates\" to compare this device against the released version."

    // "Up to date" is only reachable after a REAL comparison against a release
    // manifest. Nothing sets it today, because nothing can check -- see
    // `checkForUpdates`. The copy stays honest about what was compared.
    is UpdateStatus.UpToDate -> "Up to date" to
        "This device is running ${status.version}, which matched the released version."

    is UpdateStatus.Newer -> "A newer version is available" to
        "You are running ${status.current}; ${status.available} is available. " +
        "It is built for this board."

    is UpdateStatus.WrongBoard -> "That image is for different hardware" to
        "The available release targets ${status.available}, but this device is " +
        "${status.deviceBoard}. Flashing it would not run. This is refused rather " +
        "than offered, because there is no safe way to try it."

    is UpdateStatus.Failed -> "Could not check" to
        "${status.reason} — the device is unaffected and still running " +
        "whatever it was running before."
}
