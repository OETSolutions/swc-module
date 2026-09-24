package com.oetsolutions.swc.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
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

    /**
     * The running version is AHEAD of the release (spec §9.5 step 2).
     *
     * Its own state, not folded into [UpToDate]: they are different facts, and a
     * device running a newer build than the published one is usually a device that
     * was bench-flashed, which the user should be told rather than shown "up to
     * date". A downgrade is never offered — it is a rollback an attacker could
     * induce to put a known-vulnerable image back on the device.
     */
    data class NotNewer(val current: String, val available: String) : UpdateStatus

    /**
     * The image IS newer, but the running version is below `min_from_version`
     * (spec §9.5 step 4): applying it directly is unsupported and would need a
     * staged migration, so the user is told WHY it is not offered.
     */
    data class TooOldToUpgradeFrom(
        val current: String,
        val available: String,
        val minFrom: String,
    ) : UpdateStatus

    /** The available image is for different hardware; flashing it would brick the device. */
    data class WrongBoard(val available: String, val deviceBoard: String) : UpdateStatus
    data class Failed(val reason: String) : UpdateStatus
}

data class UpdateUiState(
    val currentVersion: String = "—",
    val status: UpdateStatus = UpdateStatus.Unknown,
    /**
     * True while a check is running. It gates the "Check for updates" button, so a
     * second tap cannot start a second fetch.
     *
     * It is a LIVE gate now (open item N-12, resolved 2026-09-24): `checkForUpdates`
     * fetches the manifest off-thread, so there is a real in-progress window to
     * represent. Before that it was set `true` and then immediately `false` inside
     * one `launch` body with no suspension between, so a collector could only ever
     * observe `false` — a gate that never fired.
     */
    val inProgress: Boolean = false,

    /**
     * The USB push's own state (spec §9.3), kept separate from [status] because the
     * two are different questions: [status] is "is there a newer release", this is
     * "is an image being written right now". Only these drive the push button.
     */
    val pushInProgress: Boolean = false,
    /** Bytes sent and total, for a real progress bar rather than a spinner. */
    val pushSent: Int = 0,
    val pushTotal: Int = 0,
    /** The outcome of the last push, stated plainly. Null until one has run. */
    val pushResult: PushResult? = null,
)

/** The outcome of a USB firmware push, as the user needs to read it. */
sealed interface PushResult {
    /** The image verified and committed; the device offers a reboot. */
    data object Installed : PushResult
    /**
     * The build has no partitions (a host/dev image), or the device refused the
     * image. `reason` names which.
     */
    data class Refused(val reason: String) : PushResult
    /** The transfer started but did not finish. The device kept its old image. */
    data class Failed(val reason: String) : PushResult
}

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
    onInstallAvailable: () -> Unit,
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
        // The install path spec §9.5 describes: the check found a newer release, so
        // offer to download it (over the phone's connection) and push it (over USB).
        // Shown ONLY for a newer release -- there is nothing to install otherwise,
        // and a button that looks live with no release behind it is how a user taps
        // it and gets a confusing failure. Disabled during a check or a push so a
        // double tap cannot start two transfers.
        if (state.status is UpdateStatus.Newer) {
            Button(
                onClick = onInstallAvailable,
                enabled = !state.pushInProgress && !state.inProgress,
                modifier = Modifier.testTag("install-update"),
            ) {
                Text(if (state.pushInProgress) "Installing…" else "Download and install")
            }
        }
        // The USB push is LIVE now that the firmware serves the `ota_*` frames
        // (N-14). It is disabled only while a push is already running, so a second
        // tap cannot open a second run the device would refuse as `run_open`.
        //
        // The WiFi path stays disabled: it opens the device's own maintenance page,
        // and the radio is not started yet (open item N-15). A button that looks live
        // and does nothing is indistinguishable from one that is broken, so the state
        // is shown rather than implied -- but the USB button IS live and says so.
        OutlinedButton(
            onClick = onPushOverUsb,
            enabled = !state.pushInProgress && !state.inProgress,
            modifier = Modifier.testTag("push-over-usb"),
        ) {
            Text(if (state.pushInProgress) "Sending…" else "Push a file over USB")
        }
        if (state.pushInProgress && state.pushTotal > 0) {
            val fraction = state.pushSent.toFloat() / state.pushTotal.toFloat()
            LinearProgressIndicator(
                progress = { fraction },
                modifier = Modifier.fillMaxWidth().testTag("push-progress"),
            )
            Text(
                "${state.pushSent} / ${state.pushTotal} bytes",
                style = MaterialTheme.typography.bodySmall,
            )
        }
        state.pushResult?.let { result ->
            val (ok, text) = describePush(result)
            Card(
                Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(
                    containerColor = if (ok) Color(0xFFE8F5E9) else Color(0xFFFFEBEE)
                ),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text(text, style = MaterialTheme.typography.bodyMedium)
                }
            }
        }
        OutlinedButton(onClick = onUpdateOverWifi, enabled = false) {
            Text("Update over WiFi (not yet)")
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

    is UpdateStatus.NotNewer -> "No update offered" to
        "You are running ${status.current}, which is newer than the released " +
        "${status.available}. Your device is ahead of the published release, so " +
        "nothing is offered — a downgrade is never installed."

    is UpdateStatus.TooOldToUpgradeFrom -> "Update not offered for this version" to
        "This device is running ${status.current}, and ${status.available} can only " +
        "be installed from ${status.minFrom} or later — it needs an intermediate " +
        "version first. Update through that version rather than straight to " +
        "${status.available}."

    is UpdateStatus.WrongBoard -> "That image is for different hardware" to
        "The available release targets ${status.available}, but this device is " +
        "${status.deviceBoard}. Flashing it would not run. This is refused rather " +
        "than offered, because there is no safe way to try it."

    is UpdateStatus.Failed -> "Could not check" to
        "${status.reason} — the device is unaffected and still running " +
        "whatever it was running before."
}

/** A push outcome as (is-good-news, one sentence). */
internal fun describePush(result: PushResult): Pair<Boolean, String> = when (result) {
    PushResult.Installed ->
        true to ("The update is installed. Reboot the adapter to run it — it will " +
            "only become active after the device proves it can still drive the output.")

    is PushResult.Refused ->
        false to ("The device refused the image: ${result.reason}. Nothing was " +
            "changed and it is still running the version it had.")

    is PushResult.Failed ->
        false to ("The update did not finish: ${result.reason}. The device kept its " +
            "current version and is working normally.")
}
