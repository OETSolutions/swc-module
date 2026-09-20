package com.oetsolutions.swc.ui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
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
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier

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
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                Surface(Modifier.fillMaxSize()) { AppRoot() }
            }
        }
    }
}

@Composable
fun AppRoot() {
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
        Column(Modifier.padding(padding)) {
            when (screen) {
                Screen.LINK -> LinkScreen(state = LinkUiState(), onRetry = {})
                Screen.LADDER -> LadderScreen(state = LadderUiState(idleMv = 0, buttons = emptyList()))
                Screen.BINDINGS -> BindingScreen(
                    state = BindingUiState(),
                    onEdit = {},
                    onSave = {},
                )
                Screen.UPDATE -> UpdateScreen(
                    state = UpdateUiState(),
                    onCheck = {},
                    onPushOverUsb = {},
                    onUpdateOverWifi = {},
                )
            }
        }
    }
}
