package com.oetsolutions.swc.app

import com.oetsolutions.swc.link.Frame
import com.oetsolutions.swc.link.LinkState
import com.oetsolutions.swc.link.SwcClient
import com.oetsolutions.swc.link.SwcTransport
import com.oetsolutions.swc.action.ActionOutcome
import com.oetsolutions.swc.contract.ActionKind
import com.oetsolutions.swc.model.ConfigJson
import com.oetsolutions.swc.model.Gesture
import com.oetsolutions.swc.model.sampleConfig
import com.oetsolutions.swc.ui.UpdateStatus
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The composition of client and screens.
 *
 * **This test exists because nothing composed them.** The plan built the model and
 * client (Task 20), the screens (Task 21) and the CI gates (Task 22), and no task
 * joined them: `SwcClient` had no production caller and `MainActivity` rendered
 * default state with no-op callbacks, so the app could not do anything. A green
 * suite of client tests and screen tests did not notice, because the defect was the
 * missing edge between them.
 *
 * So the assertions here are all about that edge: a frame from the device changes
 * what a screen would render; a config reply populates the ladder and the bindings
 * grid; a save is refused locally when the config is invalid and the local model is
 * not adopted when the device nacks.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class AppViewModelTest {

    private class FakeTransport : SwcTransport {
        val written = mutableListOf<String>()
        private val flow = MutableSharedFlow<ByteArray>(extraBufferCapacity = 64)
        override suspend fun write(bytes: ByteArray) {
            written += String(bytes)
        }
        override val incoming: Flow<ByteArray> = flow
        override fun close() {}
        suspend fun emit(text: String) = flow.emit(text.toByteArray())
        fun lastType(): String = written.lastOrNull()
            ?.let { Regex("\"type\":\"([^\"]+)\"").find(it)?.groupValues?.get(1) } ?: ""
    }

    /**
     * Start the receive loop and let it SUBSCRIBE before emitting.
     *
     * `incoming` is a `SharedFlow` with no replay and `runTest` does not start a
     * `launch` until it is advanced, so a frame emitted first is dropped. The same
     * race exists in production, which is why `MainActivity` opens the transport
     * and connects inside one `lifecycleScope.launch` after the view model — whose
     * `init` starts the collector — has been constructed.
     */
    /**
     * A scope the test can DRIVE but does not have to wait for.
     *
     * This is the one non-obvious piece of the harness, and it was measured rather
     * than guessed. `scope = this` (the TestScope itself) works, but then `runTest`
     * refuses to finish: the view model's four collectors are never cancelled, so
     * it reports active child jobs and fails a test whose assertions all passed.
     * `backgroundScope` fixes that but the collectors never subscribe (measured via
     * `SharedFlow.subscriptionCount`: it stayed 0), so every emitted frame went
     * nowhere.
     *
     * A scope on the TEST SCHEDULER but not parented to the test's job gets both:
     * `advanceUntilIdle()` drives its coroutines, and `runTest` does not wait for
     * them to complete.
     */
    private fun TestScope.vmScope() = CoroutineScope(StandardTestDispatcher(testScheduler))

    private fun TestScope.started(vm: AppViewModel) = advanceUntilIdle()

    private fun frame(type: String, vararg fields: Pair<String, String>): String {
        val body = fields.joinToString(",") { (k, v) -> "\"$k\":$v" }
        val sep = if (body.isEmpty()) "" else ","
        return "{\"v\":1,\"seq\":1,\"type\":\"$type\"$sep$body}\n"
    }

    /**
     * The device's config reply, as a legal chunked run.
     *
     * **The chunking is not incidental.** The protocol's line cap is 1024 bytes
     * (`kNdjsonMaxFrame`), and `kConfigWireChunkBytes` is 512 for exactly that
     * reason: 512 decoded bytes become 684 base64 characters, which fits with the
     * envelope around it. An earlier version of this helper emitted the whole
     * 1409-byte config as ONE chunk, which base64s to 1940 characters — the client
     * correctly rejected the over-long line as malformed, and the test looked like
     * a client bug. It was the test asserting a frame the wire cannot carry.
     */
    private fun configRun(c: com.oetsolutions.swc.model.Config): List<String> {
        val bytes = ConfigJson.encode(c).toByteArray()
        val out = mutableListOf<String>()
        out += frame("config_begin", "total_len" to "${bytes.size}", "crc32" to "${crcOf(bytes)}")
        var off = 0
        while (off < bytes.size) {
            val end = minOf(off + 512, bytes.size)
            val chunk = java.util.Base64.getEncoder()
                .encodeToString(bytes.copyOfRange(off, end))
            out += frame("config_chunk", "offset" to "$off", "data_b64" to "\"$chunk\"")
            off = end
        }
        out += frame("config_end", "sha256" to "\"${shaOf(bytes)}\"")
        return out
    }

    @Test
    fun `an event frame sets the live reading and the reported gesture`() = runTest {
        // Spec 4.3's `event` is "the core event", and it is the ONLY frame carrying
        // a level outside a learn run -- so without this path the live ladder view
        // has nothing to draw when the user is not learning.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("event", "channel" to "0", "button" to "\"vol_up\"",
            "gesture" to "\"LONG\"", "t_ms" to "1234", "level_mv" to "1430"))
        advanceUntilIdle()

        assertEquals(1430, vm.ladder.value.liveMv)
        assertEquals(Gesture.LONG, vm.ladder.value.lastGesture)
        assertEquals("vol_up", vm.ladder.value.lastGestureButton)
        // 1430 matches vol_up's window, so the derived match must find it once the
        // config has arrived; before that there are no buttons to match against.
        assertNull(vm.ladder.value.matched())
    }

    @Test
    fun `a config reply populates the ladder window and the bindings grid`() = runTest {
        // The screens render from the config. Before this wiring the ladder showed
        // no buttons and the bindings grid showed no cells, which is exactly what a
        // user saw on a device that was working.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        val c = sampleConfig()
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        val ladder = vm.ladder.value
        assertEquals(2835, ladder.idleMv)
        assertEquals(2, ladder.buttons.size)
        assertEquals("vol_up", ladder.buttons[0].id)
        assertEquals(1430, ladder.buttons[0].mvCenter)

        // The grid is one cell per button x gesture, with the config's own binding
        // on the pairs it binds and null on the pairs it does not.
        val cells = vm.bindings.value.cells
        assertEquals(2 * 3, cells.size)
        val volUpSingle = cells.first { it.buttonId == "vol_up" && it.gesture == "SINGLE" }
        assertEquals(ActionKind.OUT_VOLTAGE, volUpSingle.action?.kind)
        val volDn = cells.filter { it.buttonId == "next" }
        assertTrue("a button with no SINGLE binding must show an empty cell",
            volDn.first { it.gesture == "SINGLE" }.action == null)
    }

    @Test
    fun `a version mismatch from the device becomes a link problem the screen can render`() = runTest {
        // Spec 4.5: a mismatch must be explicit, and the link screen has a distinct
        // message for it. A mismatch reaching only `LinkState` would render as
        // "Failed" and lose both version numbers, which are the whole point of the
        // message -- the user has to know WHICH side to update.
        //
        // Note there is no `hello` payload here, and that is the finding: the
        // firmware writes `hello.protocol_v` from the same constant as the
        // envelope's `v`, so a mismatched frame never reaches the `HELLO` branch at
        // all. The version lives in exactly one place, and this is it.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit("{\"v\":99,\"seq\":1,\"type\":\"hello\",\"fw_version\":\"0.1.0\",\"protocol_v\":99}\n")
        advanceUntilIdle()

        assertEquals(LinkState.VersionMismatch(firmware = 99, app = 1), vm.link.value.link)
        assertEquals(
            com.oetsolutions.swc.link.LinkProblem.VersionMismatch(99, 1),
            vm.link.value.problem,
        )
        // The version is NOT adopted, because the frame was never dispatched: a
        // device speaking an unknown protocol is not one whose self-description the
        // app should trust.
        assertNull(vm.link.value.firmwareVersion)
    }

    @Test
    fun `a firmware version on hello is adopted and clears a stale problem`() = runTest {
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        vm.reportOpenProblem(com.oetsolutions.swc.link.LinkProblem.NoDevice)

        t.emit(frame("hello", "fw_version" to "\"0.2.0\"", "protocol_v" to "1"))
        advanceUntilIdle()

        assertEquals("0.2.0", vm.link.value.firmwareVersion)
        assertEquals(LinkState.Connected, vm.link.value.link)
        assertNull("a successful hello must clear the enumeration problem", vm.link.value.problem)
    }

    @Test
    fun `a transport enumeration problem is what the link screen shows`() = runTest {
        // The four-state LinkProblem design is unreachable without this: permission
        // and wrong-device are known before any frame is exchanged, so no frame
        // could ever produce them.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        vm.reportOpenProblem(com.oetsolutions.swc.link.LinkProblem.NoUsbPermission("SWC adapter"))
        advanceUntilIdle()
        assertEquals(
            com.oetsolutions.swc.link.LinkProblem.NoUsbPermission("SWC adapter"),
            vm.link.value.problem,
        )
    }

    @Test
    fun `an edit is held locally and only sent on save`() = runTest {
        val t = FakeTransport()
        var saved: com.oetsolutions.swc.model.Config? = null
        // The save path is injected, so this test exercises the view model's
        // decision -- what it sends and what it adopts -- without a device.
        val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { c -> saved = c; true })
        started(vm)

        val c = sampleConfig()
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        val cell = vm.bindings.value.cells.first { it.buttonId == "next" && it.gesture == "SINGLE" }
        vm.editBinding(cell, com.oetsolutions.swc.model.Action(ActionKind.OUT_VOLTAGE, keyMv = 2000))
        advanceUntilIdle()

        assertNull("nothing may be sent before Save is pressed", saved)
        // The edit is visible immediately, so the user can see what they changed.
        assertEquals(ActionKind.OUT_VOLTAGE,
            vm.bindings.value.cells.first { it.buttonId == "next" && it.gesture == "SINGLE" }
                .action?.kind)

        vm.save()
        advanceUntilIdle()

        assertNotNull(saved)
        val added = saved!!.bindings.firstOrNull { it.button == "next" && it.gesture == Gesture.SINGLE }
        assertNotNull("the edited cell must appear as a binding in what is sent", added)
        assertEquals(2000, added!!.actions.first().keyMv)
    }

    @Test
    fun `a save preserves every channel the device reported`() = runTest {
        // The board is two-channel (FR-9) and `ConfigDefault` enables both, so a
        // real device answers `config_get` with two `ChannelConfig`s. A save that
        // rebuilt the config around `channels.firstOrNull()` would send back only
        // SWC1 -- replacing the device's whole SWC2 ladder (its learned idle and
        // every button centre) with nothing, silently, while telling the user the
        // edit was saved.
        val t = FakeTransport()
        var saved: com.oetsolutions.swc.model.Config? = null
        val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { c -> saved = c; true })
        started(vm)

        val base = sampleConfig()
        val second = base.channels[0].copy(
            name = "SWC2",
            ladder = base.channels[0].ladder.copy(learnedIdleMv = 2801),
        )
        val c = base.copy(channels = base.channels + second)
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        val cell = vm.bindings.value.cells.first { it.buttonId == "next" && it.gesture == "SINGLE" }
        vm.editBinding(cell, com.oetsolutions.swc.model.Action(ActionKind.OUT_VOLTAGE, keyMv = 2000))
        advanceUntilIdle()
        vm.save()
        advanceUntilIdle()

        assertNotNull(saved)
        assertEquals("both channels must survive the save", 2, saved!!.channels.size)
        assertEquals("SWC2's learned ladder must not be dropped",
            2801, saved!!.channels[1].ladder.learnedIdleMv)
    }

    @Test
    fun `a refused save is reported and the pending edit is retained`() = runTest {
        // The failure path, and it matters more than the success one: `setConfig`
        // refuses to adopt the local model on a nack precisely so the app never
        // displays a config the device is not running. A view model that dropped
        // the pending edits on failure would show the user their change as saved
        // while the device still held the old bindings -- and the user's next
        // action would be based on that false state.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { false })
        started(vm)
        configRun(sampleConfig()).forEach { t.emit(it) }
        advanceUntilIdle()

        val cell = vm.bindings.value.cells.first { it.buttonId == "next" && it.gesture == "SINGLE" }
        vm.editBinding(cell, com.oetsolutions.swc.model.Action(ActionKind.OUT_VOLTAGE, keyMv = 2000))
        advanceUntilIdle()
        vm.save()
        advanceUntilIdle()

        // The edit is still what the screen shows...
        assertEquals(ActionKind.OUT_VOLTAGE,
            vm.bindings.value.cells.first { it.buttonId == "next" && it.gesture == "SINGLE" }
                .action?.kind)
        // ...and the failure is stated rather than swallowed.
        assertTrue("a refused save must report a problem, got ${vm.bindings.value.problems}",
            vm.bindings.value.problems.isNotEmpty())
    }

    @Test
    fun `a new binding takes its channel from the button, not from an unrelated binding`() =
        runTest {
            // The channel for a NEW binding must come from the BUTTON's own ladder.
            // It came from `config.bindings.firstOrNull()?.channel` -- whatever the
            // first existing binding happened to name. A device whose first binding
            // is `ANY` (or SWC2) therefore wrote every newly authored SWC1 binding
            // with THAT channel: an `ANY` binding fires on BOTH channels, so a
            // binding the user made for one specific button also fired on the other
            // wheel's same-named button -- a wrong command from a button the user
            // never touched.
            val t = FakeTransport()
            var saved: com.oetsolutions.swc.model.Config? = null
            val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { c -> saved = c; true })
            started(vm)

            val base = sampleConfig()
            // Make the FIRST binding name ANY, so the old derivation would copy
            // `ANY` onto the edited SWC1 binding. The grid itself shows channel 0's
            // buttons (the app's bindings screen is single-channel in v1), and
            // `vol_up`/`next` are SWC1's.
            val anyFirst = base.bindings[0].copy(channel = com.oetsolutions.swc.model.BindingChannel.ANY)
            val c = base.copy(bindings = listOf(anyFirst) + base.bindings.drop(1))
            configRun(c).forEach { t.emit(it) }
            advanceUntilIdle()

            val cell = vm.bindings.value.cells.first { it.buttonId == "next" && it.gesture == "SINGLE" }
            vm.editBinding(cell, com.oetsolutions.swc.model.Action(ActionKind.OUT_VOLTAGE, keyMv = 2000))
            advanceUntilIdle()
            vm.save()
            advanceUntilIdle()

            assertNotNull(saved)
            val added = saved!!.bindings.firstOrNull { it.button == "next" && it.gesture == Gesture.SINGLE }
            assertNotNull("the edited cell must appear as a binding", added)
            assertEquals(
                "an SWC1 button must bind to SWC1, not to whatever channel the first binding named",
                com.oetsolutions.swc.model.BindingChannel.SWC1,
                added!!.channel,
            )
        }

    @Test
    fun `a locally invalid config is refused before it is sent`() = runTest {
        // The same rule the firmware enforces, checked locally so the user gets the
        // FIELD named rather than a nack that can only name a check (SwcClient does
        // this too; this asserts the view model does not bypass it).
        val t = FakeTransport()
        var calls = 0
        val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { calls++; true })

        // No config has arrived, so the local model is the codec's default -- which
        // is not valid (no channel name), and the refusal must happen before the
        // injected save path is reached.
        vm.save()
        advanceUntilIdle()

        assertTrue("the injected save path must not be reached for an invalid config", calls == 0)
        assertTrue(vm.bindings.value.problems.isNotEmpty())
    }

    @Test
    fun `a recognized event runs the app-side action bound to that button and gesture`() = runTest {
        // Spec 3.6 splits the library: the firmware does the OUT_ family and the APP
        // does everything else. The firmware releases the line for an app-side kind
        // rather than hold a key with no action behind it, so if the app does not
        // resolve the binding here, the user's binding does nothing at all --
        // which is exactly what happened while ActionRunner had no caller.
        val t = FakeTransport()
        val ran = mutableListOf<String>()
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            runAppAction = { kind, target, _ -> ran += "$kind:$target"; ActionOutcome.Ran },
        )
        started(vm)
        configRun(sampleConfig()).forEach { t.emit(it) }
        advanceUntilIdle()

        // The fixture binds `next` DOUBLE to APP_LAUNCH of com.spotify.music.
        t.emit(frame("event", "channel" to "0", "button" to "\"next\"",
            "gesture" to "\"DOUBLE\"", "t_ms" to "10", "level_mv" to "2145"))
        advanceUntilIdle()

        assertEquals(listOf("APP_LAUNCH:com.spotify.music"), ran)
        assertTrue("a successful action reports no problem", vm.actionOutcomes.value.isEmpty())
    }

    @Test
    fun `an app-side action that cannot run is reported, not swallowed`() = runTest {
        // "The button did nothing" is the outcome ActionOutcome exists to prevent.
        val t = FakeTransport()
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            runAppAction = { _, _, _ -> ActionOutcome.Blocked("Android refused from the background") },
        )
        started(vm)
        configRun(sampleConfig()).forEach { t.emit(it) }
        advanceUntilIdle()

        t.emit(frame("event", "channel" to "0", "button" to "\"next\"",
            "gesture" to "\"DOUBLE\"", "t_ms" to "10", "level_mv" to "2145"))
        advanceUntilIdle()

        assertTrue("a blocked action must be reported",
            vm.actionOutcomes.value.any { it.contains("refused") })
    }

    @Test
    fun `an event with no binding for that pair runs nothing`() = runTest {
        // vol_up SINGLE is bound to OUT_VOLTAGE, which is the FIRMWARE's to execute.
        // The app must not treat it as its own and must not report it as a failure.
        val t = FakeTransport()
        val ran = mutableListOf<String>()
        val vm = AppViewModel(SwcClient(t), scope = vmScope(),
            runAppAction = { kind, target, _ -> ran += "$kind:$target"; ActionOutcome.Ran })
        started(vm)
        configRun(sampleConfig()).forEach { t.emit(it) }
        advanceUntilIdle()

        t.emit(frame("event", "channel" to "0", "button" to "\"vol_up\"",
            "gesture" to "\"SINGLE\"", "t_ms" to "10", "level_mv" to "1430"))
        advanceUntilIdle()

        assertTrue("an OUT_ binding is the firmware's, not the app's", ran.isEmpty())
        assertTrue("and it is not an app-side failure", vm.actionOutcomes.value.isEmpty())
    }

    @Test
    fun `an unrecognized press still moves the live ladder, with no button highlighted`() = runTest {
        // FR-12: the device reports a press it cannot recognise as
        // `event{button: null}`. Treating a null button as "nothing to do" is the
        // tempting simplification and it is wrong twice over: the ladder would
        // freeze at its last value while the device is in fact seeing every press,
        // and the user's only symptom would be "the app is stuck", which points at
        // the app rather than at the mis-learned button.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("event", "channel" to "0", "button" to "null",
            "gesture" to "\"NONE\"", "t_ms" to "77", "level_mv" to "2400"))
        advanceUntilIdle()

        assertEquals("the reading is reported even with no button", 2400, vm.ladder.value.liveMv)
        assertEquals(Gesture.NONE, vm.ladder.value.lastGesture)
        assertNull("no button may be highlighted for an unrecognised press",
            vm.ladder.value.lastGestureButton)
    }

    @Test
    fun `an unrecognized press runs no app-side action`() = runTest {
        // There is no button, so there is no (button, gesture) pair to resolve. A
        // lookup with a null key must not fall through to some default action --
        // the radio doing something the driver did not ask for is the exact
        // failure FR-12 is written to prevent.
        val t = FakeTransport()
        val ran = mutableListOf<String>()
        val vm = AppViewModel(SwcClient(t), scope = vmScope(),
            runAppAction = { kind, target, _ -> ran += "$kind:$target"; ActionOutcome.Ran })
        started(vm)
        configRun(sampleConfig()).forEach { t.emit(it) }
        advanceUntilIdle()

        t.emit(frame("event", "channel" to "0", "button" to "null",
            "gesture" to "\"NONE\"", "t_ms" to "10", "level_mv" to "2400"))
        advanceUntilIdle()

        assertTrue("an unrecognised press must run nothing", ran.isEmpty())
        assertTrue("and it is not an app-side failure", vm.actionOutcomes.value.isEmpty())
    }

    @Test
    fun `a log frame from the device is surfaced on the link screen`() = runTest {
        // FR-18's clamp warning reaches the app as a `log` frame. Dropping it
        // would leave the warning's only consumer with nothing to show -- the
        // "detected but not reported" defect it was added to close, one layer up.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("log", "level" to "\"WARN\"", "msg" to "\"key_mv 9000 clamped\""))
        advanceUntilIdle()

        assertEquals(1, vm.link.value.logs.size)
        assertTrue("the level must be kept, so a warning reads as one",
            vm.link.value.logs[0].startsWith("WARN"))
        assertTrue(vm.link.value.logs[0].contains("9000"))
    }

    @Test
    fun `device logs are bounded so a chatty device cannot grow the state`() = runTest {
        // A device that logs on every poll tick would otherwise grow this list
        // without limit for as long as the app is connected.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        repeat(50) { t.emit(frame("log", "level" to "\"INFO\"", "msg" to "\"line $it\"")) }
        advanceUntilIdle()

        assertEquals(20, vm.link.value.logs.size)
        assertTrue("the NEWEST are the useful ones", vm.link.value.logs.last().contains("line 49"))
    }

    @Test
    fun `an app-side kind this build cannot run is reported as unimplemented, not as a bad binding`() =
        runTest {
            // KEYCODE, MEDIA, VOLUME and SYSTEM are the APP's to execute per spec
            // 3.6, and ActionRunner implements none of them. The bindings screen
            // offers every kind in the generated enum, so a user can bind one and
            // will see nothing happen. The message matters: "not an app-side action"
            // would tell them to go fix a binding that is already correct, whereas
            // "not implemented yet" is the true reason.
            val t = FakeTransport()
            val ran = mutableListOf<String>()
            val vm = AppViewModel(
                SwcClient(t),
                scope = vmScope(),
                runAppAction = { kind, _, _ -> ran += kind; ActionOutcome.NotImplemented(kind) },
            )
            started(vm)

            val c = sampleConfig().let { cfg ->
                cfg.copy(bindings = cfg.bindings + com.oetsolutions.swc.model.Binding(
                    "b9", com.oetsolutions.swc.model.BindingChannel.SWC1, "next",
                    com.oetsolutions.swc.model.Gesture.SINGLE, true,
                    listOf(com.oetsolutions.swc.model.Action(
                        com.oetsolutions.swc.contract.ActionKind.KEYCODE, "KEYCODE_MEDIA_NEXT", "")),
                ))
            }
            configRun(c).forEach { t.emit(it) }
            advanceUntilIdle()

            t.emit(frame("event", "channel" to "0", "button" to "\"next\"",
                "gesture" to "\"SINGLE\"", "t_ms" to "10", "level_mv" to "2145"))
            advanceUntilIdle()

            assertEquals("the app must attempt it, not skip it", listOf("KEYCODE"), ran)
            val msg = vm.actionOutcomes.value.single()
            assertTrue("the message must say it is unimplemented, not mis-bound: $msg",
                msg.contains("not implement"))
            assertFalse("and must NOT claim the binding is not app-side: $msg",
                msg.contains("not an app-side action"))
        }

    @Test
    fun `checking for updates never claims to be up to date without checking`() = runTest {
        // The app has no release-manifest client and no INTERNET permission, so it
        // has never seen a manifest. Reporting UpToDate would be a claim about the
        // world that no code verified -- and a green "up to date" is precisely what
        // stops a user looking for an update that does exist.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "hw_id" to "\"swc\"",
            "protocol_v" to "1", "caps" to "[]"))
        advanceUntilIdle()

        vm.checkForUpdates()
        advanceUntilIdle()

        val status = vm.update.value.status
        assertFalse("an unchecked build must not report UpToDate: $status",
            status is UpdateStatus.UpToDate)
        assertTrue("and it must say why, not silently do nothing: $status",
            status is UpdateStatus.Failed)
        assertTrue("the message must name the real reason: ${(status as UpdateStatus.Failed).reason}",
            status.reason.contains("cannot check") || status.reason.contains("manifest"))
    }

    // --- spec 8.2: the app's maintenance control --------------------------

    @Test
    fun `entering maintenance is shown as open only once the device acks`() = runTest {
        // Spec 8.2 makes the USB command the PRIMARY trigger and warns the car
        // "may have no WiFi", so this control is the app's only path to turning
        // the radio on. Showing the window open before the device confirmed it
        // would send the user hunting for an access point that does not exist.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "hw_id" to "\"swc\"",
            "protocol_v" to "1", "caps" to "[]"))
        advanceUntilIdle()

        vm.enterMaintenance()
        // `runCurrent`, NOT `advanceUntilIdle`: the latter advances virtual time
        // past the request's own 5 s wait and the request times out before the
        // reply below can be delivered, which is what the first version of this
        // test measured. Every other test in this suite sidesteps a live ack by
        // injecting the operation; this one is the first that must observe a real
        // reply, so it has to hold the clock still.
        runCurrent()

        val sent = t.written.last()
        assertTrue("the request must go out as maintenance_enter: $sent",
            sent.contains("\"type\":\"maintenance_enter\""))
        assertFalse("and must not claim the window is open before the ack",
            vm.link.value.maintenanceOpen)

        val seq = Regex("\"seq\":(\\d+)").find(sent)!!.groupValues[1]

        // A NACK means the device refused, which is not the same as it being open.
        t.emit("{\"v\":1,\"seq\":2,\"type\":\"nack\",\"for_seq\":$seq," +
            "\"err\":\"unavailable\",\"detail\":\"no orchestrator\"}\n")
        advanceUntilIdle()
        assertFalse("a refusal must not read as open", vm.link.value.maintenanceOpen)
        assertTrue("and it must say why: ${vm.link.value.maintenanceProblem}",
            vm.link.value.maintenanceProblem?.contains("unavailable") == true)
    }

    @Test
    fun `an acked maintenance enter opens the window and a later exit closes it`() = runTest {
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "hw_id" to "\"swc\"",
            "protocol_v" to "1", "caps" to "[]"))
        advanceUntilIdle()

        vm.enterMaintenance()
        runCurrent()
        val enterSeq = Regex("\"seq\":(\\d+)").find(t.written.last())!!.groupValues[1]
        t.emit("{\"v\":1,\"seq\":2,\"type\":\"ack\",\"for_seq\":$enterSeq,\"ok\":true}\n")
        advanceUntilIdle()
        assertTrue("an ack means the window is open", vm.link.value.maintenanceOpen)

        vm.exitMaintenance()
        runCurrent()
        assertTrue("leaving must send its own frame, not _enter",
            t.written.last().contains("\"type\":\"maintenance_exit\""))
        val exitSeq = Regex("\"seq\":(\\d+)").find(t.written.last())!!.groupValues[1]
        t.emit("{\"v\":1,\"seq\":3,\"type\":\"ack\",\"for_seq\":$exitSeq,\"ok\":true}\n")
        advanceUntilIdle()
        assertFalse("an acked exit closes it", vm.link.value.maintenanceOpen)
    }

    // --- helpers -----------------------------------------------------------

    private fun crcOf(data: ByteArray): Long {
        var crc = 0xFFFFFFFFL
        for (b in data) {
            crc = crc xor (b.toLong() and 0xFF)
            for (i in 0 until 8) {
                crc = if (crc and 1L != 0L) (crc ushr 1) xor 0xEDB88320L else crc ushr 1
            }
        }
        return (crc xor 0xFFFFFFFFL) and 0xFFFFFFFFL
    }

    private fun shaOf(data: ByteArray): String =
        java.security.MessageDigest.getInstance("SHA-256")
            .digest(data).joinToString("") { "%02x".format(it) }
}
