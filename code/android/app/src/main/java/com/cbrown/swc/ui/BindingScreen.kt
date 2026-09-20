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
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.oetsolutions.swc.contract.ActionKind
import com.oetsolutions.swc.model.Action

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
 */
@Composable
fun BindingScreen(
    state: BindingUiState,
    onEdit: (BindingCell) -> Unit,
    onSave: () -> Unit,
    modifier: Modifier = Modifier,
) {
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
            items(state.cells, key = { "${it.buttonId}/${it.gesture}" }) { cell ->
                Card(Modifier.fillMaxWidth()) {
                    Row(
                        Modifier.fillMaxWidth().padding(12.dp),
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Column {
                            Text(cell.buttonName, style = MaterialTheme.typography.bodyLarge)
                            Text(cell.gesture, style = MaterialTheme.typography.labelMedium)
                        }
                        TextButton(
                            onClick = { onEdit(cell) },
                            modifier = Modifier.semantics {
                                contentDescription =
                                    "${cell.buttonName} ${cell.gesture}: " +
                                        (cell.action?.kind?.wireName ?: "not bound")
                            },
                        ) {
                            Text(cell.action?.kind?.wireName ?: "Not bound")
                        }
                    }
                }
            }
        }

        state.problems.takeIf { it.isNotEmpty() }?.let { problems ->
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp)) {
                    Text("Cannot save yet", style = MaterialTheme.typography.titleSmall)
                    problems.forEach { Text("• $it", style = MaterialTheme.typography.bodySmall) }
                }
            }
        }
        Button(onClick = onSave, enabled = state.problems.isEmpty()) { Text("Save to device") }
    }
}

data class BindingCell(
    val buttonId: String,
    val buttonName: String,
    val gesture: String,
    val action: Action?,
)

data class BindingUiState(
    val cells: List<BindingCell> = emptyList(),
    val problems: List<String> = emptyList(),
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
            enabled = !kind.needsParam || target.isNotEmpty(),
        ) {
            Text("Apply")
        }
        if (kind.needsParam && target.isEmpty()) {
            Text(
                "${kind.wireName} needs a ${kind.paramKey}",
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}
