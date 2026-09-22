package com.oetsolutions.swc.ui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.runtime.collectAsState
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.lifecycleScope
import com.oetsolutions.swc.action.ActionRunner
import com.oetsolutions.swc.app.AppViewModel
import com.oetsolutions.swc.link.SwcClient
import com.oetsolutions.swc.link.UsbSerialTransport
import com.oetsolutions.swc.model.Action
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** The four top-level screens. */
enum class Screen(val label: String) {
    LINK("Link"),
    LADDER("Ladder"),
    BINDINGS("Bindings"),
    UPDATE("Update"),
}

/**
 * The app's single activity.
 *
 * It owns only navigation and the state each screen renders; the protocol client and
 * the action runner are dependencies it will be given once the real USB transport
 * exists. Keeping the screens pure functions of their state is what lets every one of
 * them be tested on the JVM without a device — including under Robolectric, which is
 * how the live-ladder test runs in CI.
 */
class MainActivity : ComponentActivity() {
    private var vm: AppViewModel? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The transport is created here, not in the view model, because opening it
        // needs an Activity (USB permission is requested with one). Constructing
        // the client around it keeps every decision ABOVE the bytes testable: the
        // view model takes an `SwcClient`, which takes an `SwcTransport`, so the
        // whole state machine runs on the JVM against a fake.
        val transport = UsbSerialTransport(applicationContext)
        val model = AppViewModel(
            SwcClient(transport),
            // Spec 3.6: the app executes the non-OUT_ kinds. Passing the runner is
            // what makes an app-side binding (launch an app, send an intent)
            // actually fire when the wheel reports a press.
            runAppAction = ActionRunner(applicationContext)::run,
        )
        vm = model
        lifecycleScope.launch {
            // The transport's enumeration result is the only source for the
            // permission and wrong-device problems, so it must reach the screen.
            model.reportOpenProblem(transport.open())
            model.connect()
        }
        setContent {
            MaterialTheme {
                Surface(Modifier.fillMaxSize()) { AppRoot(model) }
            }
        }
    }

    override fun onDestroy() {
        vm?.close()
        vm = null
        super.onDestroy()
    }
}

/**
 * The app's navigation and the state each screen renders.
 *
 * `model` is a parameter rather than created inside so the screens can be driven
 * with a fake on the JVM. The no-argument overload below is the preview/empty
 * case: it renders the same screens against default state, which is what the
 * Robolectric tests use.
 */
@Composable
fun AppRoot(model: AppViewModel) {
    val link by model.link.collectAsState()
    val ladder by model.ladder.collectAsState()
    val bindings by model.bindings.collectAsState()
    val update by model.update.collectAsState()
    // The USB-OTA file picker (spec §9.3). The bytes are read on the UI thread
    // activity's resolver and handed straight to the view model, which owns the
    // chunked push. A firmware image is ~1.5 MB, well inside what a one-shot read
    // can hold (there is no PSRAM on the device, but this is the phone).
    val picker = rememberFirmwarePicker { bytes -> model.pushFirmwareOverUsb(bytes) }
    AppScaffold(
        link = link,
        ladder = ladder,
        bindings = bindings,
        update = update,
        onRetry = model::retry,
        onEdit = model::editBinding,
        onSave = model::save,
        onCheck = model::checkForUpdates,
        onEnterMaintenance = model::enterMaintenance,
        onExitMaintenance = model::exitMaintenance,
        onPushOverUsb = picker::launch,
    )
}

/** The screens against default state, for a preview or a test that wants no device. */
@Composable
fun AppRoot() = AppScaffold(
    link = LinkUiState(),
    ladder = LadderUiState(idleMv = 0, buttons = emptyList()),
    bindings = BindingUiState(),
    update = UpdateUiState(),
    onRetry = {},
    onEdit = { _, _ -> },
    onSave = {},
    onCheck = {},
    onEnterMaintenance = {},
    onExitMaintenance = {},
    onPushOverUsb = {},
)

@Composable
private fun AppScaffold(
    link: LinkUiState,
    ladder: LadderUiState,
    bindings: BindingUiState,
    update: UpdateUiState,
    onRetry: () -> Unit,
    onEdit: (BindingCell, Action?) -> Unit,
    onSave: () -> Unit,
    onCheck: () -> Unit,
    onEnterMaintenance: () -> Unit,
    onExitMaintenance: () -> Unit,
    onPushOverUsb: () -> Unit,
) {
    var screen by remember { mutableStateOf(Screen.LINK) }
    Scaffold(
        bottomBar = {
            NavigationBar {
                Screen.entries.forEach { s ->
                    NavigationBarItem(
                        selected = screen == s,
                        onClick = { screen = s },
                        icon = {},
                        label = { Text(s.label) },
                    )
                }
            }
        },
    ) { padding ->
        // NO `verticalScroll` here, and that is a correctness requirement rather
        // than a style choice. `BindingScreen` scrolls with a `LazyColumn`, and a
        // `LazyColumn` inside a `Column(verticalScroll)` is measured with an
        // INFINITE maximum height, which Compose refuses: the app dies with
        // "Vertically scrollable component was measured with an infinity maximum
        // height constraints". Each screen owns its own scrolling instead — the
        // bindings grid gives its `LazyColumn` a weight, so the Save button stays
        // pinned and visible rather than scrolling off the bottom.
        Column(Modifier.padding(padding).fillMaxSize()) {
            when (screen) {
                Screen.LINK -> LinkScreen(
                    state = link,
                    onRetry = onRetry,
                    onEnterMaintenance = onEnterMaintenance,
                    onExitMaintenance = onExitMaintenance,
                )
                Screen.LADDER -> LadderScreen(state = ladder)
                Screen.BINDINGS -> BindingScreen(
                    state = bindings,
                    onEdit = onEdit,
                    onSave = onSave,
                )
                Screen.UPDATE -> UpdateScreen(
                    state = update,
                    onCheck = onCheck,
                    // The USB push is live (N-14); the WiFi path still opens the
                    // device's maintenance page, and the radio is not started yet
                    // (open item N-15), so the screen keeps that button disabled.
                    onPushOverUsb = onPushOverUsb,
                    onUpdateOverWifi = {},
                )
            }
        }
    }
}

/**
 * A launcher for the firmware-file picker, with the read done off the UI thread.
 *
 * `ActivityResultContracts.GetContent` with the wildcard MIME keeps the file
 * provider in charge of what is selectable, so a user can hand us a `.bin` from
 * anywhere (Downloads, Drive, a USB stick). The bytes are read on
 * `Dispatchers.IO` — a firmware image is over a megabyte, and reading it inline
 * would jank the frame that started the picker.
 *
 * A null URI (the user backed out) or a failed read is dropped silently rather
 * than surfaced: it is a cancellation, not an error the user needs to be told
 * about. A file that is READ but the wrong size is caught downstream by the
 * client's local refusal, which names the real problem.
 */
@Composable
internal fun rememberFirmwarePicker(onPicked: (ByteArray) -> Unit): FirmwarePicker {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.GetContent()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val bytes = withContext(Dispatchers.IO) {
                runCatching {
                    context.contentResolver.openInputStream(uri)?.use { it.readBytes() }
                }.getOrNull()
            }
            if (bytes != null) onPicked(bytes)
        }
    }
    return remember(launcher) { FirmwarePicker { launcher.launch("*/*") } }
}

/** A one-method handle to the firmware picker, so screens can hold a stable callback. */
internal fun interface FirmwarePicker {
    fun launch()
}
