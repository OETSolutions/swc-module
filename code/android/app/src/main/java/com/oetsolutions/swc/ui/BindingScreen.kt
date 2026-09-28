package com.oetsolutions.swc.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.Switch
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.oetsolutions.swc.contract.ActionKind
import com.oetsolutions.swc.model.Action
import com.oetsolutions.swc.model.BindingChannel
import com.oetsolutions.swc.model.K_KEY_MV_MAX
import com.oetsolutions.swc.model.K_KEY_MV_MIN

/**
 * The bindings grid: buttons × gestures, each cell a bound action.
 *
 * **The action picker is driven by the generated `ActionKind` enum.**
 * `ActionKind.entries` comes from the contract that also generates the firmware's
 * header, so the app cannot offer a kind the firmware does not implement — the
 * picker's options and the firmware's behavior are one list with one source.
 *
 * **There is deliberately no action-id picker.** Spec 3.6 defines no numeric action
 * ids, and the generated contract carries none, so there is nothing to offer.
 *
 * A kind whose `needsParam` is true shows its target field, labelled with the KIND's
 * own parameter name from the contract (`package`, `action`, …), and the screen
 * refuses to save an empty one — the same rule the firmware enforces, from the same
 * generated table, so the two cannot disagree about which kinds need a parameter.
 *
 * **A cell tap opens [ActionPicker] for that cell.** It did not, once: the tap went
 * straight to `onEdit(cell, null)`, which CLEARS the binding, and `ActionPicker` —
 * written in the same commit as this screen — was composed nowhere. The only thing
 * the Bindings screen could do was delete, and the user's route to binding a button
 * to anything did not exist. Tapping now opens the picker; clearing is its own
 * explicit button inside it, so "unbind" can no longer be the accidental result of
 * a stray tap.
 */
@Composable
fun BindingScreen(
    state: BindingUiState,
    onEdit: (BindingCell, Action?) -> Unit,
    onSave: () -> Unit,
    modifier: Modifier = Modifier,
) {
    // Which cell's picker is open, if any. Keyed by the same id the grid keys on,
    // so a config refresh cannot leave the dialog pointed at a stale cell.
    var editing by remember { mutableStateOf<BindingCell?>(null) }

    Column(
        modifier = modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Bindings", style = MaterialTheme.typography.titleMedium)
        Text(
            "Each cell binds one gesture on one button. An empty cell means the " +
                "gesture is passed to the head unit unchanged.",
            style = MaterialTheme.typography.bodySmall,
        )

        // `weight(1f)` so the list takes the space that is LEFT after the header
        // and the Save button, rather than measuring itself unbounded. Without it
        // the list would push Save off the bottom of a long grid, and the user
        // could edit a binding with no way to store it.
        LazyColumn(
            Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items(state.cells, key = { "${it.channel.wireName}/${it.buttonId}/${it.gesture}" }) { cell ->
                Card(Modifier.fillMaxWidth()) {
                    Row(
                        Modifier.fillMaxWidth().padding(12.dp),
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Column {
                            // The channel is shown because the SAME button name is
                            // usually on both ladders: two rows reading "Volume Up /
                            // SINGLE" with no channel would be indistinguishable, and
                            // the user could not tell which binding they were editing.
                            Text(
                                "${cell.channel.wireName} · ${cell.buttonName}",
                                style = MaterialTheme.typography.bodyLarge,
                            )
                            Text(cell.gesture, style = MaterialTheme.typography.labelMedium)
                        }
                        TextButton(
                            onClick = { editing = cell },
                            modifier = Modifier.semantics {
                                contentDescription =
                                    "${cell.channel.wireName} ${cell.buttonName} ${cell.gesture}: " +
                                        (cell.action?.kind?.wireName ?: "not bound")
                            },
                        ) {
                            Text(cell.action?.kind?.wireName ?: "Not bound")
                        }
                    }
                }
            }
        }

        editing?.let { cell ->
            ActionEditorDialog(
                cell = cell,
                onPick = { action ->
                    onEdit(cell, action)
                    editing = null
                },
                onClear = {
                    onEdit(cell, null)
                    editing = null
                },
                onDismiss = { editing = null },
            )
        }

        state.problems.takeIf { it.isNotEmpty() }?.let { problems ->
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp)) {
                    Text("Cannot save yet", style = MaterialTheme.typography.titleSmall)
                    problems.forEach { Text("• $it", style = MaterialTheme.typography.bodySmall) }
                }
            }
        }
        // A save FAILURE, rendered separately from the validation problems above
        // and deliberately NOT gating the button below: the user must be able to
        // retry. See BindingUiState.saveError.
        state.saveError?.let { err ->
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp)) {
                    Text("Save failed", style = MaterialTheme.typography.titleSmall)
                    Text(err, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        Button(onClick = onSave, enabled = state.problems.isEmpty()) { Text("Save to device") }

        // The per-press click, OPTIONAL and off by default: "normal switch operation
        // should not cause a beep, but the beep for it can be enabled optionally in
        // the app if used." Toggling persists immediately (this is a device setting,
        // not a pending edit), so a user hears the difference without a Save.
        Row(
            Modifier.fillMaxWidth().padding(top = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text("Beep on each press", style = MaterialTheme.typography.bodyMedium)
                Text(
                    "Off by default. When on, the adapter beeps once for every " +
                        "button press it recognises.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Switch(checked = state.keyClickEnabled, onCheckedChange = state.onSetKeyClick)
        }
    }
}

/** The picker for one cell, with an explicit way to clear the binding. */
@Composable
private fun ActionEditorDialog(
    cell: BindingCell,
    onPick: (Action) -> Unit,
    onClear: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("${cell.buttonName} · ${cell.gesture}") },
        // The picker's own Apply button commits the action; this dialog's buttons
        // are the two exits that do NOT commit. `Clear binding` is here and not on
        // the cell, so unbinding is deliberate rather than what a stray tap does.
        text = { ActionPicker(current = cell.action, onPick = onPick) },
        confirmButton = {},
        dismissButton = {
            Row {
                TextButton(onClick = onClear) { Text("Clear binding") }
                TextButton(onClick = onDismiss) { Text("Cancel") }
            }
        },
    )
}


/**
 * One cell: a (channel, button, gesture) triple and its bound action.
 *
 * **The CHANNEL is part of the cell's identity, not decoration.** A binding is
 * `(channel, button, gesture)` on the wire (spec 3.5), and the firmware's
 * `BindingResolve` matches all three. A cell that carried only the button and
 * gesture could not say WHICH channel it edits, and `vol_up` is the same id on
 * both ladders -- the common case, since both wheels have volume. Keying edits by
 * `button/gesture` alone then made an edit to SWC1's `vol_up` indistinguishable
 * from one to SWC2's, so saving the SWC1 edit DELETED the device's SWC2 binding.
 */
data class BindingCell(
    val channel: BindingChannel,
    val buttonId: String,
    val buttonName: String,
    val gesture: String,
    val action: Action?,
)

data class BindingUiState(
    val cells: List<BindingCell> = emptyList(),
    /**
     * VALIDATION problems: the local config cannot legally be sent. These are what
     * gate [BindingScreen]'s Save button, because they are things the user can fix
     * by editing a cell.
     */
    val problems: List<String> = emptyList(),
    /**
     * The result of the last SAVE ATTEMPT: a nack, a timeout, a refused link.
     *
     * **Keep this separate from [problems], and never gate Save on it.** A runtime
     * failure is not a validation failure, but it used to be written into the same
     * `problems` list the button reads as `enabled = problems.isEmpty()`. A device
     * nack or a timeout therefore DISABLED Save: the pending edit was still shown
     * in the cell, so the user's change looked live, but they could not retry --
     * "Try again" was a button that no longer existed. The only recovery was to
     * edit an unrelated cell, which re-sent the same config.
     *
     * A save failure must be *shown* and must leave Save *enabled*.
     */
    val saveError: String? = null,
    /**
     * The per-press "click" — a short beep on every recognised switch press.
     * OFF by default; this is where the user opts in.
     */
    val keyClickEnabled: Boolean = false,
    /** Toggle the per-press click and persist it to the device. */
    val onSetKeyClick: (Boolean) -> Unit = {},
)

/**
 * The action picker.
 *
 * Options are [ActionKind.entries] — the generated contract's list. The parameter
 * field appears only for kinds that take one, and its LABEL is the kind's own wire
 * key, so the screen shows the user the same word the firmware will read.
 */
@Composable
fun ActionPicker(
    current: Action?,
    onPick: (Action) -> Unit,
    modifier: Modifier = Modifier,
) {
    var kind by remember { mutableStateOf(current?.kind ?: ActionKind.NONE) }
    var target by remember { mutableStateOf(current?.target ?: "") }
    var payload by remember { mutableStateOf(current?.payload ?: "") }
    var keyMv by remember { mutableStateOf(current?.keyMv?.toString() ?: "") }
    var expanded by remember { mutableStateOf(false) }

    // OUT_VOLTAGE carries its parameter as a NUMBER (`key_mv`) rather than in the
    // `target` string, so `needsParam` alone does not describe what it needs and
    // the Apply gate below has to ask for it separately. Without this the gate
    // read `!needsParam || target.isNotEmpty()`, which for OUT_VOLTAGE is
    // `!false || ...` -- always true. Apply was therefore enabled on an empty
    // field and committed `keyMv = 0`, which `ConfigJson.problems()` refuses
    // ("OUT_VOLTAGE needs a key_mv") -- disabling Save for the WHOLE config with a
    // message naming an ordinal binding id the grid never shows, so the user could
    // not tell which cell to fix.
    val keyMvValue = keyMv.trim().toIntOrNull()
    val keyMvOk = kind != ActionKind.OUT_VOLTAGE ||
        (keyMvValue != null && keyMvValue in K_KEY_MV_MIN..K_KEY_MV_MAX)

    Column(modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Box {
            TextButton(onClick = { expanded = true }, modifier = Modifier.testTag("kind-picker")) {
                Text("Kind: ${kind.wireName}")
            }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                ActionKind.entries.forEach { k ->
                    DropdownMenuItem(
                        text = { Text(k.wireName) },
                        onClick = { kind = k; expanded = false },
                    )
                }
            }
        }

        // The target field exists only for kinds that HAVE one. `needsParam` is the
        // contract's copy of the firmware's `ActionTakesPayload`, so an unused field
        // is not shown at all rather than shown-and-ignored.
        kind.paramKey?.let { key ->
            OutlinedTextField(
                value = target,
                onValueChange = { target = it },
                label = { Text(key) },
                modifier = Modifier.fillMaxWidth().testTag("target-field"),
            )
        }
        kind.payloadKey?.let { key ->
            OutlinedTextField(
                value = payload,
                onValueChange = { payload = it },
                label = { Text(key) },
                modifier = Modifier.fillMaxWidth().testTag("payload-field"),
            )
        }
        // OUT_VOLTAGE is the exception: its parameter is a NUMBER (key_mv), not one
        // of the two string slots (spec 3.6).
        if (kind == ActionKind.OUT_VOLTAGE) {
            OutlinedTextField(
                value = keyMv,
                onValueChange = { keyMv = it },
                label = { Text("key_mv (millivolts)") },
                modifier = Modifier.fillMaxWidth().testTag("key-mv-field"),
            )
        }

        Button(
            onClick = {
                onPick(
                    Action(
                        kind = kind,
                        target = target,
                        payload = payload,
                        keyMv = keyMv.toIntOrNull() ?: 0,
                    )
                )
            },
            // The gate must ask for whatever the KIND actually needs: a target
            // string for the `needsParam` kinds, and a usable key_mv for
            // OUT_VOLTAGE, whose parameter is a number. See `keyMvOk`.
            enabled = (!kind.needsParam || target.isNotEmpty()) && keyMvOk,
        ) {
            Text("Apply")
        }
        if (kind.needsParam && target.isEmpty()) {
            Text(
                "${kind.wireName} needs a ${kind.paramKey}",
                style = MaterialTheme.typography.bodySmall,
            )
        }
        if (!keyMvOk) {
            Text(
                "key_mv must be a whole number between $K_KEY_MV_MIN and $K_KEY_MV_MAX mV",
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}
