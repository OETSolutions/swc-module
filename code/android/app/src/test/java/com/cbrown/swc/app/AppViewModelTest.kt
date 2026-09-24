package com.oetsolutions.swc.app

import com.oetsolutions.swc.link.Frame
import com.oetsolutions.swc.link.LinkProblem
import com.oetsolutions.swc.link.LinkState
import com.oetsolutions.swc.link.SwcClient
import com.oetsolutions.swc.link.SwcTransport
import com.oetsolutions.swc.action.ActionOutcome
import com.oetsolutions.swc.contract.ActionKind
import com.oetsolutions.swc.model.Action
import com.oetsolutions.swc.model.ConfigJson
import com.oetsolutions.swc.model.Gesture
import com.oetsolutions.swc.model.sampleConfig
import com.oetsolutions.swc.ui.PushResult
import com.oetsolutions.swc.ui.UpdateStatus
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.isActive
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

        /**
         * When set, every written frame is answered with an `ack` naming its `seq`.
         *
         * Off by default so a test that depends on an unanswered request still sees
         * silence; on for the OTA install tests, whose push needs the `ota_*` acks a
         * real device sends. Mirrors `SwcClientTest`'s fake, since the view model's
         * push goes through the same chunked transport.
         */
        var autoAck = false
        override suspend fun write(bytes: ByteArray) {
            val s = String(bytes)
            written += s
            if (!autoAck) return
            val seq = Regex("\"seq\":(\\d+)").find(s)?.groupValues?.get(1) ?: return
            flow.tryEmit(
                "{\"v\":1,\"seq\":$seq,\"type\":\"ack\",\"for_seq\":$seq}\n".toByteArray()
            )
        }
        override val incoming: Flow<ByteArray> = flow
        override fun close() {}
        suspend fun emit(text: String) = flow.emit(text.toByteArray())
        fun lastType(): String = written.lastOrNull()
            ?.let { Regex("\"type\":\"([^\"]+)\"").find(it)?.groupValues?.get(1) } ?: ""

        /** Counts re-enumerations, so the retry test can prove one happened. */
        var reopened = 0
        var reopenProblem: LinkProblem? = null
        override suspend fun reopen(): LinkProblem? {
            ++reopened
            return reopenProblem
        }
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
    fun `an event frame carries the live idle the device classified against`() = runTest {
        // Open item N-25's wire half. `matched()` reproduces `LadderClassify` by
        // normalizing the reading against the LIVE idle and the windows against the
        // LEARNED rail, so the event's `idle_mv` has to reach the state. Without it
        // the view falls back to the learned rail and disagrees with the device the
        // moment the rail moves.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("event", "channel" to "0", "button" to "\"vol_up\"",
            "gesture" to "\"SINGLE\"", "t_ms" to "5", "level_mv" to "1500", "idle_mv" to "2977"))
        advanceUntilIdle()

        assertEquals(2977, vm.ladder.value.liveIdleMv)
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
    fun `a maintenance frame reports an open window with its secrets`() = runTest {
        // Spec 8.3 option 1. The board has no display, so the BLE Proof-of-
        // Possession and the page token reach the user through THIS app or not at
        // all -- a frame that arrives and is dropped makes the provisioning path
        // unusable, which is the "produced, consumed by nobody" shape (N-22/N-24/
        // N-45) with a user who cannot provision.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(
            frame(
                "maintenance",
                "active" to "true",
                "pop" to "\"A1B2C3\"",
                "token" to "\"ABCDEF123456\"",
                "page_url" to "\"http://192.168.4.1/?token=ABCDEF123456\"",
                "ble_name" to "\"A1B2\"",
                "ble_failures" to "0",
            ),
        )
        advanceUntilIdle()

        val s = vm.link.value
        assertTrue("the device's window must open in the app too", s.maintenanceOpen)
        assertEquals("A1B2C3", s.maintenancePop)
        assertEquals("ABCDEF123456", s.maintenanceToken)
        assertEquals("http://192.168.4.1/?token=ABCDEF123456", s.maintenancePageUrl)
        assertEquals("A1B2", s.maintenanceBleName)
        assertEquals(0, s.maintenanceBleFailures)
    }

    @Test
    fun `a maintenance frame reporting the window closed clears every secret`() = runTest {
        // A stale PoP left on screen after the window shut is a credential for a
        // session that is no longer listening: the user types it into the
        // Espressif app and waits out a timeout that looks like an app bug. The
        // fields are ASSIGNED from the frame, so an empty payload clears them.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(
            frame(
                "maintenance", "active" to "true", "pop" to "\"A1B2C3\"",
                "token" to "\"ABCDEF123456\"", "ble_name" to "\"A1B2\"",
            ),
        )
        advanceUntilIdle()
        assertEquals("A1B2C3", vm.link.value.maintenancePop)

        t.emit(
            frame(
                "maintenance", "active" to "false", "pop" to "\"\"",
                "token" to "\"\"", "page_url" to "\"\"", "ble_name" to "\"\"",
            ),
        )
        advanceUntilIdle()

        val s = vm.link.value
        assertFalse("the device closed the window and the app must follow", s.maintenanceOpen)
        assertEquals("", s.maintenancePop)
        assertEquals("", s.maintenanceToken)
        assertEquals("", s.maintenancePageUrl)
        assertFalse(
            "a closed window must not render an 'it did not work' message",
            s.maintenanceProblem != null,
        )
    }

    @Test
    fun `a failed radio reaches the screen so it does not promise a setup page`() = runTest {
        // The window and the radio are separate: the device opens the window and
        // only then brings the radio up, and that can fail. `ble_failures` is the
        // signal, and it must reach the state the card renders -- otherwise the
        // card sends a user hunting for an access point that does not exist (the
        // N-76 shape: copy promising a capability the device lacks).
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("maintenance", "active" to "true", "ble_failures" to "1"))
        advanceUntilIdle()

        val s = vm.link.value
        assertTrue(s.maintenanceOpen)
        assertEquals(1, s.maintenanceBleFailures)
        assertEquals("a failed radio leaves no page to offer", "", s.maintenancePageUrl)
    }

    @Test
    fun `an AUX1-opened window is visible without the app having asked`() = runTest {
        // The device opens the window on a 3 s AUX1 hold (spec 8.2), which no app
        // request produced. Before the `maintenance` frame existed the app could
        // only ever show a window IT had opened, so a no-app user's window was
        // invisible to the app -- and with it the PoP and the page URL.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        assertFalse("nothing has opened a window yet", vm.link.value.maintenanceOpen)

        t.emit(frame("maintenance", "active" to "true", "pop" to "\"A1B2C3\""))
        advanceUntilIdle()

        assertTrue(
            "a window the app did not open must still be shown",
            vm.link.value.maintenanceOpen,
        )
    }

    @Test
    fun `a status frame reports the config state the device sends`() = runTest {
        // Spec 4.3: `config_state` exists so a config fault has "a name the app
        // could read" -- §6.8's corrupt-config response is `config_state: defaults`.
        // The firmware emits it every 2 s; before this the app read none of it.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("status", "vbus_present" to "true", "config_state" to "\"defaults\""))
        advanceUntilIdle()

        assertEquals("defaults", vm.link.value.configState)
        assertNotNull(
            "a config fallback must reach the user, not just the state object",
            vm.link.value.configWarning,
        )
    }

    @Test
    fun `a status frame carries the board temperature and free heap to the screen`() = runTest {
        // Open item N-22's last two fields. Both were declared in spec 4.3 and
        // produced by nothing, so the app could not show them. Now they arrive and
        // must reach the state the Link screen renders -- a value the transport
        // delivers and the app forgets is the same "produced, consumed by nobody"
        // shape the loss counters were.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(
            frame(
                "status",
                "config_state" to "\"ok\"",
                "temp_c" to "23.5",
                "heap_free" to "123456",
            ),
        )
        advanceUntilIdle()

        assertEquals(23.5, vm.link.value.lastTempC!!, 1e-6)
        assertEquals(123456L, vm.link.value.lastHeapFree)
    }

    @Test
    fun `a null temperature reads as no reading, never as zero degrees`() = runTest {
        // The firmware reports JSON null until an NTC conversion is good, because
        // 0 C is a LEGAL temperature. The app must keep that distinction: folding
        // null to 0.0 would show a measured freezing board for a device that has
        // measured nothing.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("status", "config_state" to "\"ok\"", "temp_c" to "null", "heap_free" to "9"))
        advanceUntilIdle()

        assertNull("a JSON-null temperature is `no reading`", vm.link.value.lastTempC)
        assertEquals(9L, vm.link.value.lastHeapFree)
    }

    @Test
    fun `a status frame does NOT overwrite the ladder's idle with the rail`() = runTest {
        // The STATUS branch used to read `rail_mv` into the ladder's idle. That
        // field has no producer (spec N-22), and it is the +3V3 RAIL (~3300), not
        // the wheel's idle KEY level (~2835) -- so had it ever been sent, every
        // band in LadderScreen would have been divided by the wrong number. The
        // idle comes from the config; a status frame must leave it alone.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        val c = sampleConfig()
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()
        assertEquals(2835, vm.ladder.value.idleMv)

        t.emit(frame("status", "rail_mv" to "3300", "config_state" to "\"ok\""))
        advanceUntilIdle()

        assertEquals("the rail must never become the ladder's idle", 2835, vm.ladder.value.idleMv)
        assertNull("a healthy config has nothing to warn about", vm.link.value.configWarning)
    }

    @Test
    fun `a pass-through device with no config is not reported as a fault`() = runTest {
        // FR-25's supported pass-through device reports `config_state: none`, which
        // is deliberately NOT `defaults`: a device with no config still serves
        // presses, so telling the user "your configuration is gone" would be false.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("status", "config_state" to "\"none\""))
        advanceUntilIdle()

        assertEquals("none", vm.link.value.configState)
        assertNull(vm.link.value.configWarning)
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
    fun `an expired link silence becomes a problem the screen can render`() = runTest {
        // Spec §4.4's liveness, the app's own half (open item N-27). The state
        // `SwcClient` raises on 10 s of silence must reach the screen as its OWN
        // problem type: folded onto `LinkFailed` the user is told "retry", and onto
        // `NoDevice` they are told the cable was never there -- neither is what
        // happened, which is an adapter that answered and then stopped.
        //
        // This is the mapping half: a real client, driven past its own threshold.
        // The clock half -- that 10 s of silence IS raised, once -- is pinned in
        // `SwcClientTest` against a controlled clock.
        val t = FakeTransport()
        val clock = java.util.concurrent.atomic.AtomicLong(1_000_000L)
        val client = SwcClient(t, nowMs = { clock.get() })
        val vm = AppViewModel(client, scope = vmScope())
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()
        assertEquals(LinkState.Connected, vm.link.value.link)

        clock.addAndGet(10_000)
        // The periodic loop belongs to the activity's lifecycle and this test owns
        // no activity, so the tick is driven directly. `started(vm)` has already let
        // the client's `state` collector subscribe, so the mapping runs.
        assertTrue(client.SilenceTick())
        advanceUntilIdle()

        assertEquals(LinkState.SilenceExpired, vm.link.value.link)
        assertEquals(
            "the screen must show its own message for a silent link, not a retry " +
                "or a missing cable: was ${vm.link.value.problem}",
            com.oetsolutions.swc.link.LinkProblem.SilenceExpired,
            vm.link.value.problem,
        )
    }

    @Test
    fun `a failed config run reaches the screen with its reason, not as no-device`() = runTest {
        // `SwcClient` raises `LinkState.Failed` with a NAMED reason for a torn run,
        // a digest mismatch or an over-long line. The view model used to collapse
        // every one onto `LinkProblem.NoDevice`, so the user was told to check a
        // cable that was working, and the reason -- the only thing that says which
        // of those actually happened -- was discarded and rendered nowhere.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        // A config run whose declared length the chunks do not fill: `endInboundConfig`
        // reports "config run ended early" and leaves the model alone.
        t.emit(frame("config_begin", "total_len" to "40", "crc32" to "0"))
        t.emit(frame("config_end", "sha256" to "\"\""))
        advanceUntilIdle()

        assertTrue(
            "the state must be Failed, was ${vm.link.value.link}",
            vm.link.value.link is LinkState.Failed,
        )
        val problem = vm.link.value.problem
        assertTrue(
            "the failure must reach the screen with its reason, not as NoDevice: $problem",
            problem is com.oetsolutions.swc.link.LinkProblem.LinkFailed,
        )
        assertEquals(
            "config run ended early",
            (problem as com.oetsolutions.swc.link.LinkProblem.LinkFailed).reason,
        )
    }

    @Test
    fun `a save stamps updated_at_ms with the app's own wall clock`() = runTest {
        // Spec N-28: `updated_at_ms` was declared, decoded and re-encoded while
        // nothing ever assigned it, so an edited config still reported the timestamp
        // it arrived with (or 0). The firmware cannot stamp it -- it has no RTC --
        // so the app is the only writer that can make the field mean what its name
        // promises. This pins that a save the user makes carries the app's clock.
        val t = FakeTransport()
        var saved: com.oetsolutions.swc.model.Config? = null
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            saveConfig = { c -> saved = c; true },
            nowMs = { 1_700_000_000_000L },
        )
        started(vm)
        val c = sampleConfig()
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        val cell = vm.bindings.value.cells.first { it.buttonId == "next" && it.gesture == "SINGLE" }
        vm.editBinding(cell, com.oetsolutions.swc.model.Action(ActionKind.OUT_VOLTAGE, keyMv = 2000))
        vm.save()
        advanceUntilIdle()

        assertEquals(1_700_000_000_000L, saved!!.updatedAtMs)
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
        assertNotNull("a refused save must state the failure",
            vm.bindings.value.saveError)
        // **And it must NOT gate the Save button.** A runtime nack is not a
        // validation problem, but it used to be written into `problems` -- the very
        // field the screen reads as `enabled = problems.isEmpty()` -- so a timeout
        // disabled Save while the pending edit still showed in its cell. The user
        // could not retry, and the only recovery was to edit an unrelated cell.
        assertTrue("a save failure must leave the button usable for a retry, " +
            "got problems=${vm.bindings.value.problems}",
            vm.bindings.value.problems.isEmpty())
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
            // `ANY` onto the edited SWC1 binding. `vol_up`/`next` are SWC1's, and
            // the cell edited below is SWC1's `next SINGLE`.
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
    fun `a stale edit for a button that no longer exists is dropped, not sent`() = runTest {
        // The edit key comes from a grid built against an EARLIER config, and a
        // later refresh (connect() re-reads) can re-learn the button away. The old
        // code emitted the binding anyway, under a SWC1 fallback, which the
        // firmware refuses (`BindingNamesARealInput`) -- and since the app's
        // `problems()` does not mirror that rule, the whole save was nacked and
        // every VALID edit lost behind the generic message. The stale one must be
        // dropped instead.
        val t = FakeTransport()
        var saved: com.oetsolutions.swc.model.Config? = null
        val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { c -> saved = c; true })
        started(vm)

        configRun(sampleConfig()).forEach { t.emit(it) }
        advanceUntilIdle()

        // Edit a button the current config HAS.
        val stale = vm.bindings.value.cells.first { it.buttonId == "next" && it.gesture == "DOUBLE" }
        vm.editBinding(stale, com.oetsolutions.swc.model.Action(ActionKind.APP_LAUNCH, "com.stale.app"))
        // And edit one a refreshed config will still have, so the save carries a
        // legitimate edit alongside the stale one.
        val live = vm.bindings.value.cells.first { it.buttonId == "vol_up" && it.gesture == "DOUBLE" }
        vm.editBinding(live, com.oetsolutions.swc.model.Action(ActionKind.APP_LAUNCH, "com.live.app"))
        advanceUntilIdle()

        // The device now reports a config where `next` is gone (re-learned away).
        val refreshed = sampleConfig().let { base ->
            base.copy(
                channels = listOf(
                    base.channels[0].copy(
                        ladder = base.channels[0].ladder.copy(
                            buttons = base.channels[0].ladder.buttons.filter { it.id != "next" },
                        ),
                    ),
                ),
                bindings = emptyList(),
            )
        }
        configRun(refreshed).forEach { t.emit(it) }
        advanceUntilIdle()

        vm.save()
        advanceUntilIdle()

        assertNotNull("the save must reach the device", saved)
        val buttons = saved!!.bindings.map { it.button }
        assertFalse("a binding to a button that no longer exists must not be sent",
            buttons.contains("next"))
        assertTrue("and the still-valid edit must survive: $buttons",
            buttons.contains("vol_up"))
    }

    @Test
    fun `an edit on a headless-learned button produces an id the device accepts`() = runTest {
        // The app DERIVES a binding id as `"${button}-${gesture}"`. A button id
        // that came from the headless learn (the ONLY production learn path -- no
        // app, no host) is `swc1_bt<n>`, so slot 10 and up derive
        // `swc1_bt10-SINGLE`: EXACTLY 16 characters, which is `ConfigModel.h`'s
        // `kBindingIdLen` and therefore refused by `ConfigJson.problems()`
        // ("id must be under 16 chars"). The refusal is not a warning -- it
        // disables the Save button, so the user's edit to that button can never be
        // sent at all. The app was generating an id its own validator rejects.
        val t = FakeTransport()
        var saved: com.oetsolutions.swc.model.Config? = null
        val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { c -> saved = c; true })
        started(vm)

        val base = sampleConfig()
        // A ladder the AUX1 wizard built: slot 10's generated slug, a real id the
        // device stores and the app's grid shows.
        val learned = base.copy(
            channels = listOf(
                base.channels[0].copy(
                    ladder = base.channels[0].ladder.copy(
                        buttons = base.channels[0].ladder.buttons +
                            com.oetsolutions.swc.model.LadderButton(
                                "swc1_bt10", "Button 10", 1700, 110, 3300, 235, 200, 98,
                            ),
                    ),
                ),
            ),
            bindings = emptyList(),
        )
        configRun(learned).forEach { t.emit(it) }
        advanceUntilIdle()

        val cell = vm.bindings.value.cells
            .first { it.buttonId == "swc1_bt10" && it.gesture == "SINGLE" }
        vm.editBinding(cell, com.oetsolutions.swc.model.Action(ActionKind.OUT_VOLTAGE, keyMv = 2000))
        advanceUntilIdle()

        assertTrue(
            "an edit on a headless-learned button must not be refused by the app's own " +
                "id-width check: ${vm.bindings.value.problems}",
            vm.bindings.value.problems.isEmpty(),
        )

        vm.save()
        advanceUntilIdle()

        assertNotNull("the save must reach the device", saved)
        val added = saved!!.bindings
            .firstOrNull { it.button == "swc1_bt10" && it.gesture == Gesture.SINGLE }
        assertNotNull("the edited cell must appear as a binding in what is sent", added)
        assertTrue(
            "the derived binding id must fit kBindingIdLen: '${added!!.id}'",
            added.id.length < 16,
        )
    }

    @Test
    fun `editing a button on one channel does not destroy the other channel's binding`() = runTest {
        // The board is two-channel and `vol_up` is the SAME id on both ladders --
        // the common case, since both wheels have volume. The edit key is
        // `button/gesture`, which a channel-blind `kept` filter then uses to drop
        // every binding whose pair was "edited": an edit to SWC1's vol_up SINGLE
        // also removes SWC2's vol_up SINGLE, because the two keys are identical.
        // The device's SWC2 binding is gone after a save the user was told worked.
        val t = FakeTransport()
        var saved: com.oetsolutions.swc.model.Config? = null
        val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { c -> saved = c; true })
        started(vm)

        val base = sampleConfig()
        val second = base.channels[0].copy(
            name = "SWC2",
            ladder = base.channels[0].ladder.copy(learnedIdleMv = 2801),
        )
        // Both channels carry vol_up; SWC2's SINGLE binding is the one at risk.
        val swc2Binding = com.oetsolutions.swc.model.Binding(
            "b9", com.oetsolutions.swc.model.BindingChannel.SWC2, "vol_up", Gesture.SINGLE, true,
            listOf(Action(ActionKind.OUT_VOLTAGE, "", "", 2600)),
        )
        val c = base.copy(channels = base.channels + second, bindings = base.bindings + swc2Binding)
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        // Edit SWC1's vol_up SINGLE (the cell the grid shows).
        val cell = vm.bindings.value.cells.first { it.buttonId == "vol_up" && it.gesture == "SINGLE" }
        vm.editBinding(cell, Action(ActionKind.OUT_VOLTAGE, keyMv = 2000))
        advanceUntilIdle()
        vm.save()
        advanceUntilIdle()

        assertNotNull(saved)
        val swc2Survived = saved!!.bindings.firstOrNull {
            it.button == "vol_up" && it.gesture == Gesture.SINGLE &&
                it.channel == com.oetsolutions.swc.model.BindingChannel.SWC2
        }
        assertNotNull(
            "editing SWC1's vol_up must not delete SWC2's vol_up (bindings: ${saved!!.bindings})",
            swc2Survived,
        )
        assertEquals(2600, swc2Survived!!.actions.first().keyMv)
    }

    @Test
    fun `the grid shows a binding for every channel that has a ladder`() = runTest {
        // The board is two-channel (FR-9) and a binding names its channel (spec
        // 3.5). A grid built from `channels.firstOrNull()` showed SWC2's buttons
        // NOWHERE, so SWC2's bindings could not be seen or edited at all.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        val base = sampleConfig()
        val second = base.channels[0].copy(
            name = "SWC2",
            ladder = base.channels[0].ladder.copy(learnedIdleMv = 2801),
        )
        configRun(base.copy(channels = base.channels + second)).forEach { t.emit(it) }
        advanceUntilIdle()

        val channels = vm.bindings.value.cells.map { it.channel }.distinct()
        assertEquals(
            "both channels' buttons must appear in the grid",
            listOf(com.oetsolutions.swc.model.BindingChannel.SWC1, com.oetsolutions.swc.model.BindingChannel.SWC2),
            channels,
        )
    }

    @Test
    fun `an edit is dropped when the button is on the OTHER channel's ladder, not this one`() =
        runTest {
            // `BindingResolve` looks the id up in `channels[channel_index].ladder`,
            // so a binding whose channel's ladder does not hold the id never fires.
            // The firmware would ACCEPT the save anyway -- `BindingNamesARealInput`
            // checks whether the id is on ANY channel's ladder, not the named
            // one -- so the binding would be silently DEAD on the device. The
            // check must therefore be "on THIS channel's ladder", not "on SOME
            // ladder": the two differ exactly when a button is re-learned onto one
            // channel only, which is what a second-channel re-learn does. The
            // mutation that weakened the check to "on any ladder" survived every
            // other test in this file.
            val t = FakeTransport()
            var saved: com.oetsolutions.swc.model.Config? = null
            val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { c -> saved = c; true })
            started(vm)

            val base = sampleConfig()
            // BOTH ladders hold `next` to begin with, so an SWC2 cell exists to edit.
            val second = base.channels[0].copy(name = "SWC2")
            configRun(base.copy(channels = base.channels + second)).forEach { t.emit(it) }
            advanceUntilIdle()

            // Edit SWC2's `next` -- legitimate NOW.
            val cell = vm.bindings.value.cells.first {
                it.channel == com.oetsolutions.swc.model.BindingChannel.SWC2 &&
                    it.buttonId == "next" && it.gesture == "SINGLE"
            }
            vm.editBinding(cell, Action(ActionKind.OUT_VOLTAGE, keyMv = 2000))
            advanceUntilIdle()

            // The device now reports SWC2 re-learned WITHOUT `next`, while SWC1
            // still has it. The pending edit names (SWC2, next): `next` is on SOME
            // ladder (SWC1's) but not SWC2's, so it must be dropped.
            val relearned = sampleConfig().let { b ->
                val swc2NoNext = base.channels[0].copy(
                    name = "SWC2",
                    ladder = base.channels[0].ladder.copy(
                        buttons = base.channels[0].ladder.buttons.filter { it.id != "next" },
                    ),
                )
                b.copy(channels = listOf(b.channels[0], swc2NoNext))
            }
            configRun(relearned).forEach { t.emit(it) }
            advanceUntilIdle()
            vm.save()
            advanceUntilIdle()

            assertNotNull(saved)
            assertFalse(
                "a binding on SWC2 for a button only SWC1's ladder holds must not be sent: " +
                    "${saved!!.bindings}",
                saved!!.bindings.any {
                    it.button == "next" && it.channel == com.oetsolutions.swc.model.BindingChannel.SWC2
                },
            )
        }

    @Test
    fun `a cell shows the ANY binding the device would actually fire`() = runTest {
        // `ANY` fires from EITHER channel (spec 3.5), and `BindingResolve` matches
        // it. A cell that looked for a channel-exact binding would draw SWC2's
        // `vol_up SINGLE` empty even though the device acts on it -- and the user
        // would "bind" a gesture the device already fires, seeing the wrong action
        // as a result.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        val base = sampleConfig()
        val second = base.channels[0].copy(name = "SWC2")
        // b4 is `next LONG` on ANY; give SWC2 a vol_up ANY SINGLE to check.
        val anyBinding = com.oetsolutions.swc.model.Binding(
            "b7", com.oetsolutions.swc.model.BindingChannel.ANY, "vol_up", Gesture.SINGLE, true,
            listOf(Action(ActionKind.OUT_VOLTAGE, "", "", 2700)),
        )
        val c = base.copy(channels = base.channels + second, bindings = base.bindings + anyBinding)
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        val swc2Cell = vm.bindings.value.cells.first {
            it.channel == com.oetsolutions.swc.model.BindingChannel.SWC2 &&
                it.buttonId == "vol_up" && it.gesture == "SINGLE"
        }
        assertEquals(
            "an ANY binding fires from either channel, so the SWC2 cell must show it",
            ActionKind.OUT_VOLTAGE, swc2Cell.action?.kind,
        )
    }

    @Test
    fun `editing one channel's cell wins over a surviving ANY binding for that gesture`() = runTest {
        // `BindingResolve` returns the FIRST match, so binding ORDER is
        // precedence. An `ANY` binding is kept by `withEdits`, and if it sat ahead
        // of the edit the channel-specific binding the user just made would never
        // be reached -- the edit would save, be reported as applied, and do nothing
        // on the device.
        val t = FakeTransport()
        var saved: com.oetsolutions.swc.model.Config? = null
        val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { c -> saved = c; true })
        started(vm)

        val base = sampleConfig()
        // Drop the fixture's SWC1 vol_up SINGLE (b1) so only an ANY binding holds
        // that triple.
        val anyBinding = com.oetsolutions.swc.model.Binding(
            "b7", com.oetsolutions.swc.model.BindingChannel.ANY, "vol_up", Gesture.SINGLE, true,
            listOf(Action(ActionKind.OUT_VOLTAGE, "", "", 2700)),
        )
        val c = base.copy(bindings = listOf(anyBinding))
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        val cell = vm.bindings.value.cells.first {
            it.channel == com.oetsolutions.swc.model.BindingChannel.SWC1 &&
                it.buttonId == "vol_up" && it.gesture == "SINGLE"
        }
        vm.editBinding(cell, Action(ActionKind.OUT_VOLTAGE, keyMv = 2000))
        advanceUntilIdle()
        vm.save()
        advanceUntilIdle()

        assertNotNull(saved)
        // The FIRST binding for the triple is what the device fires. It must be the
        // edit, not the surviving ANY wildcard.
        val first = saved!!.bindings.firstOrNull {
            it.button == "vol_up" && it.gesture == Gesture.SINGLE &&
                (it.channel == com.oetsolutions.swc.model.BindingChannel.SWC1 ||
                    it.channel == com.oetsolutions.swc.model.BindingChannel.ANY)
        }
        assertNotNull(first)
        assertEquals(
            "the edited binding must precede the ANY wildcard, or the device ignores it",
            com.oetsolutions.swc.model.BindingChannel.SWC1, first!!.channel,
        )
        assertEquals(2000, first.actions.first().keyMv)
    }


    @Test
    fun `an edit on SWC2 of a button also on SWC1 is not dropped`() = runTest {
        val t = FakeTransport()
        var saved: com.oetsolutions.swc.model.Config? = null
        val vm = AppViewModel(SwcClient(t), scope = vmScope(), saveConfig = { c -> saved = c; true })
        started(vm)
        val base = sampleConfig()
        val second = base.channels[0].copy(name = "SWC2")
        configRun(base.copy(channels = base.channels + second)).forEach { t.emit(it) }
        advanceUntilIdle()
        val cell = vm.bindings.value.cells.first {
            it.channel == com.oetsolutions.swc.model.BindingChannel.SWC2 &&
                it.buttonId == "next" && it.gesture == "SINGLE"
        }
        vm.editBinding(cell, Action(ActionKind.OUT_VOLTAGE, keyMv = 2500))
        advanceUntilIdle()
        vm.save()
        advanceUntilIdle()
        assertNotNull(saved)
        val added = saved!!.bindings.firstOrNull {
            it.button == "next" && it.gesture == Gesture.SINGLE &&
                it.channel == com.oetsolutions.swc.model.BindingChannel.SWC2
        }
        assertNotNull("an SWC2 edit of a shared button id must not be dropped: ${saved!!.bindings}", added)
    }


    @Test
    fun `only the first matching binding's app-side action fires, as the firmware resolves`() =
        runTest {
            // `BindingResolve` returns the FIRST match and stops. When two bindings
            // match one press -- an `ANY` wildcard plus a channel-specific one on
            // the same triple, which is the common case (`vol_up` SINGLE on both) --
            // the device acts on the first only. Running EVERY match made the app
            // launch TWO apps for one press while the device resolved one binding,
            // so the app's half of spec 3.6 fired more than the device did.
            val t = FakeTransport()
            val ran = mutableListOf<String>()
            val vm = AppViewModel(
                SwcClient(t), scope = vmScope(),
                runAppAction = { kind, target, _ -> ran += "$kind:$target"; ActionOutcome.Ran },
            )
            started(vm)
            val base = sampleConfig()
            val anyDup = com.oetsolutions.swc.model.Binding(
                "b8", com.oetsolutions.swc.model.BindingChannel.ANY, "vol_up", Gesture.SINGLE, true,
                listOf(Action(ActionKind.APP_LAUNCH, "com.second.app")),
            )
            val first = base.bindings[0].copy(
                actions = listOf(Action(ActionKind.APP_LAUNCH, "com.first.app")),
            )
            val c = base.copy(bindings = listOf(first, anyDup) + base.bindings.drop(1))
            configRun(c).forEach { t.emit(it) }
            advanceUntilIdle()

            t.emit(frame("event", "channel" to "0", "button" to "\"vol_up\"",
                "gesture" to "\"SINGLE\"", "t_ms" to "5", "level_mv" to "1430"))
            advanceUntilIdle()

            assertEquals(
                "only the binding the device resolved may fire its app-side action",
                listOf("APP_LAUNCH:com.first.app"), ran,
            )
        }

    @Test
    fun `every action in the resolved binding runs, in order`() = runTest {
        // Spec 3.5: a binding's action list is ordered and best-effort, and the
        // product's core case is ONE binding with "the factory key press AND tell
        // the app". Taking only `actions[0]` would run the OUT_ half and drop the
        // app notification the user paired with it.
        val t = FakeTransport()
        val ran = mutableListOf<String>()
        val vm = AppViewModel(
            SwcClient(t), scope = vmScope(),
            runAppAction = { kind, target, _ -> ran += "$kind:$target"; ActionOutcome.Ran },
        )
        started(vm)
        val base = sampleConfig()
        // (SWC1, next, DOUBLE) -> [OUT_RELEASE (firmware's), APP_INTENT (the app's)].
        val paired = com.oetsolutions.swc.model.Binding(
            "b9", com.oetsolutions.swc.model.BindingChannel.SWC1, "next", Gesture.DOUBLE, true,
            listOf(
                Action(ActionKind.OUT_RELEASE),
                Action(ActionKind.APP_INTENT, "com.oetsolutions.swc.ACTION_NAVIGATE", "geo:1,2"),
            ),
        )
        val c = base.copy(bindings = base.bindings.filter { it.id != "b3" } + paired)
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        t.emit(frame("event", "channel" to "0", "button" to "\"next\"",
            "gesture" to "\"DOUBLE\"", "t_ms" to "7", "level_mv" to "2145"))
        advanceUntilIdle()

        assertEquals(
            "the firmware's OUT_RELEASE is skipped; the app's APP_INTENT runs",
            listOf("APP_INTENT:com.oetsolutions.swc.ACTION_NAVIGATE"), ran,
        )
    }


    @Test
    fun `an event on the second channel moves the bands and rail with the label`() = runTest {
        // The live view is one channel at a time, and an `event` carries ONE
        // channel's reading. The view used to take its LABEL from the pressing
        // channel but its BANDS and RAIL from channel 0: a press on SWC2 relabeled
        // the screen "SWC2" while still plotting the reading against SWC1's scale
        // and matching it against SWC1's windows -- the wrong button, named
        // confidently. The three must move together or not at all.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        val base = sampleConfig()
        // SWC2 differs in every field the view draws: a different rail, a
        // different button set, a different name.
        val swc2 = base.channels[0].copy(
            name = "SWC2",
            ladder = base.channels[0].ladder.copy(
                learnedIdleMv = 2801,
                buttons = listOf(
                    com.oetsolutions.swc.model.LadderButton("mute", "Mute", 1900, 100),
                ),
            ),
        )
        configRun(base.copy(channels = base.channels + swc2)).forEach { t.emit(it) }
        advanceUntilIdle()

        // A press on channel 1.
        t.emit(frame("event", "channel" to "1", "button" to "\"mute\"",
            "gesture" to "\"SINGLE\"", "t_ms" to "9", "level_mv" to "1900"))
        advanceUntilIdle()

        val ladder = vm.ladder.value
        assertEquals("SWC2", ladder.channelName)
        assertEquals("the rail must be SWC2's, not channel 0's", 2801, ladder.idleMv)
        assertEquals("the buttons must be SWC2's", listOf("mute"), ladder.buttons.map { it.id })
        assertEquals("and the reading must match SWC2's window", "mute", ladder.matched()?.id)
    }

    @Test
    fun `an event naming no channel leaves the ladder on the last good one`() = runTest {
        // A frame the firmware could not name a channel for must not blank the
        // view: the reading is still real, and an empty ladder would look like a
        // device that lost its learning.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        configRun(sampleConfig()).forEach { t.emit(it) }
        advanceUntilIdle()
        val before = vm.ladder.value

        t.emit(frame("event", "button" to "\"vol_up\"",
            "gesture" to "\"SINGLE\"", "t_ms" to "9", "level_mv" to "1430"))
        advanceUntilIdle()

        assertEquals("an unnameable channel keeps the last ladder", before.idleMv,
            vm.ladder.value.idleMv)
        assertEquals(before.buttons.map { it.id }, vm.ladder.value.buttons.map { it.id })
        assertEquals("but the reading still updates", 1430, vm.ladder.value.liveMv)
    }


    @Test
    fun `a learn stream sample shows the run's channel scale`() = runTest {
        // `ladder_sample` carries the run's `channel` (spec 4.3), and the learn
        // screen is where a mis-scaled band matters MOST: the user decides whether
        // their press was recognised by watching the reading move against the
        // bands. A learn run on channel 1 streamed into a view still scaled to
        // channel 0's rail and buttons, so a perfectly good SWC2 press looked like
        // it landed on the wrong button.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        val base = sampleConfig()
        val swc2 = base.channels[0].copy(
            name = "SWC2",
            ladder = base.channels[0].ladder.copy(
                learnedIdleMv = 2801,
                buttons = listOf(
                    com.oetsolutions.swc.model.LadderButton("mute", "Mute", 1900, 100),
                ),
            ),
        )
        configRun(base.copy(channels = base.channels + swc2)).forEach { t.emit(it) }
        advanceUntilIdle()

        t.emit(frame("ladder_sample", "channel" to "1", "level_mv" to "1900", "n" to "40"))
        advanceUntilIdle()

        val ladder = vm.ladder.value
        assertEquals("SWC2", ladder.channelName)
        assertEquals("the learn stream's rail must be SWC2's", 2801, ladder.idleMv)
        assertEquals(listOf("mute"), ladder.buttons.map { it.id })
        assertEquals(1900, ladder.liveMv)
    }

    @Test
    fun `a learn stream sample of zero blanks the reading rather than holding the last`() = runTest {
        // `EmitLadderSample` emits `level_mv: 0` as the device's "NO READING" --
        // there is no orchestrator to sample, or the conversion is unreadable or
        // stale -- and its own comment states the contract: "0 mV is unambiguous:
        // it is below the ladder's floor, so the app renders it as 'no reading'
        // rather than as a real level."
        //
        // The app did the opposite: `level > 0` dropped the frame entirely, so the
        // last real millivolt reading stayed on screen. During a learn -- the one
        // screen the user watches to decide whether a press was recognised -- that
        // is a number the device has already abandoned, and it is indistinguishable
        // from a live one. The frame carries no `no reading` flag of its own, so a
        // reading that never blanks is exactly the stale-value lie the producer's
        // comment exists to prevent.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        configRun(sampleConfig()).forEach { t.emit(it) }
        advanceUntilIdle()

        t.emit(frame("ladder_sample", "channel" to "0", "level_mv" to "1900", "n" to "10"))
        advanceUntilIdle()
        assertEquals("the real reading is shown first", 1900, vm.ladder.value.liveMv)

        t.emit(frame("ladder_sample", "channel" to "0", "level_mv" to "0", "n" to "11"))
        advanceUntilIdle()
        assertEquals(
            "a zero must blank the reading, not leave the stale one on screen",
            null, vm.ladder.value.liveMv)
    }

    @Test
    fun `a link_gap frame is counted, not dropped`() = runTest {
        // Spec 4.3: the firmware emits `link_gap` when one of the app's outgoing
        // frames was lost. The app defined `Frames.LINK_GAP` and handled it
        // NOWHERE -- it was the one inbound type with no branch -- so a dropped
        // frame vanished and a failed transfer could not be told from a refused
        // one. The frame existing in the vocabulary and being unhandled is exactly
        // the "detected but not reported" shape the frame was added to close.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("link_gap", "expected_seq" to "4", "got_seq" to "7"))
        advanceUntilIdle()

        assertEquals("a lost frame must be reported to the user", 1, vm.link.value.lostFrames)
    }

    @Test
    fun `successive link gaps accumulate rather than being overwritten`() = runTest {
        // A cable that drops one frame in a thousand drops several over a session,
        // and the COUNT is what tells the user the link is unreliable rather than
        // their device. Overwriting would keep saying "1" while the transfer kept
        // failing.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        repeat(3) { t.emit(frame("link_gap", "expected_seq" to "1", "got_seq" to "2")) }
        advanceUntilIdle()

        assertEquals(3, vm.link.value.lostFrames)
    }

    @Test
    fun `the device's own loss counters reach the screen`() = runTest {
        // N-24 and its inbound twin. Both transport counters existed on the device
        // and were read by nothing but a unit test -- `DroppedFrames` even
        // documented itself as "the failure this class exists to prevent, so it
        // must be observable", while no user could observe it. The router now
        // reports them in `status`; this asserts the other half, that the app
        // STORES them rather than parsing past them.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("status", "tx_dropped" to "2", "rx_overflows" to "1"))
        advanceUntilIdle()

        assertEquals("outbound losses must reach the screen", 2, vm.link.value.deviceTxDropped)
        assertEquals("inbound overflows must reach the screen", 1, vm.link.value.deviceRxOverflows)
    }

    @Test
    fun `a status without the loss counters leaves them alone`() = runTest {
        // The fields are cumulative on the device and absent from an older
        // firmware's `status`. Absent must mean "unchanged", not "reset to zero":
        // zeroing would erase the evidence of a loss the user is looking at, on
        // the very next 2 s keepalive.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        t.emit(frame("status", "tx_dropped" to "2", "rx_overflows" to "1"))
        advanceUntilIdle()
        t.emit(frame("status"))   // a frame carrying no loss fields at all
        advanceUntilIdle()

        assertEquals(2, vm.link.value.deviceTxDropped)
        assertEquals(1, vm.link.value.deviceRxOverflows)
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
        // ...AND REACH THE SCREEN. `actionOutcomes` was produced and read by no
        // screen and not by `MainActivity`, so the failure was computed, worded
        // and dropped: the user's evidence was a button that did nothing, which
        // is the outcome this whole type exists to prevent. The link state is
        // what `LinkScreen` renders, so the message must appear there too.
        assertTrue(
            "the failure must be mirrored into the state the Link screen renders: " +
                "${vm.link.value.actionProblems}",
            vm.link.value.actionProblems.any { it.contains("refused") },
        )
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
    fun `an app-side binding fires only for the channel the press came from`() = runTest {
        // The firmware resolves a binding by CHANNEL and button id (with `ANY` as a
        // wildcard) -- `BindingResolve`/`BindingsForButton`. The app's half of spec
        // 3.6 matched on button id alone, so once both channels carry a button of
        // the same name (the common case: `vol_up` on SWC1 and SWC2) a press on one
        // fired the OTHER channel's app-side action too -- launching an app or
        // sending an intent the second channel never asked for. The app must resolve
        // exactly what the device did, no more.
        val t = FakeTransport()
        val ran = mutableListOf<String>()
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            runAppAction = { kind, target, _ -> ran += "$kind:$target"; ActionOutcome.Ran },
        )
        started(vm)

        // Two channels, the SAME button id `vol_up` on each, each binding DOUBLE to
        // a different app. A press resolves to exactly one of them.
        val c = com.oetsolutions.swc.model.sampleConfig().let { base ->
            base.copy(
                channels = listOf(
                    base.channels[0],
                    base.channels[0].copy(name = "SWC2"),
                ),
                bindings = listOf(
                    com.oetsolutions.swc.model.Binding(
                        "c1", com.oetsolutions.swc.model.BindingChannel.SWC1, "vol_up",
                        Gesture.DOUBLE, true,
                        listOf(Action(ActionKind.APP_LAUNCH, "com.swc1.app")),
                    ),
                    com.oetsolutions.swc.model.Binding(
                        "c2", com.oetsolutions.swc.model.BindingChannel.SWC2, "vol_up",
                        Gesture.DOUBLE, true,
                        listOf(Action(ActionKind.APP_LAUNCH, "com.swc2.app")),
                    ),
                ),
            )
        }
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        t.emit(frame("event", "channel" to "0", "button" to "\"vol_up\"",
            "gesture" to "\"DOUBLE\"", "t_ms" to "10", "level_mv" to "1430"))
        advanceUntilIdle()

        assertEquals(listOf("APP_LAUNCH:com.swc1.app"), ran)
    }

    @Test
    fun `an ANY binding fires for a press on either channel`() = runTest {
        // `ANY` is a real BindingChannel value (spec 3.5), and the firmware treats it
        // as a wildcard (`b.channel != as_swc && b.channel != kAny`). The channel
        // filter must not have narrowed that away.
        val t = FakeTransport()
        val ran = mutableListOf<String>()
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            runAppAction = { kind, target, _ -> ran += "$kind:$target"; ActionOutcome.Ran },
        )
        started(vm)

        val c = com.oetsolutions.swc.model.sampleConfig().let { base ->
            base.copy(
                channels = listOf(base.channels[0], base.channels[0].copy(name = "SWC2")),
                bindings = listOf(
                    com.oetsolutions.swc.model.Binding(
                        "a1", com.oetsolutions.swc.model.BindingChannel.ANY, "vol_up",
                        Gesture.DOUBLE, true,
                        listOf(Action(ActionKind.APP_LAUNCH, "com.any.app")),
                    ),
                ),
            )
        }
        configRun(c).forEach { t.emit(it) }
        advanceUntilIdle()

        t.emit(frame("event", "channel" to "1", "button" to "\"vol_up\"",
            "gesture" to "\"DOUBLE\"", "t_ms" to "10", "level_mv" to "1430"))
        advanceUntilIdle()

        assertEquals(listOf("APP_LAUNCH:com.any.app"), ran)
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

    // --- spec 9.5: the app's release check (open item N-12) ----------------

    /** A manifest fetch that returns a fixed body, or throws, with no network. */
    private class FakeFetcher(
        var body: String = "",
        var fail: Exception? = null,
    ) : com.oetsolutions.swc.update.ManifestFetcher {
        var fetches = 0
        override suspend fun fetch(url: String): String {
            ++fetches
            fail?.let { throw it }
            return body
        }
    }

    private fun releaseManifest(
        latest: String,
        minFrom: String? = null,
        url: String = "https://github.com/oetsolutions/swc-module/releases/download/v1/firmware.bin",
    ): String = buildString {
        append("""{"latest_version":"$latest","channel":"stable",""")
        append(""""firmware":{"version":"$latest","url":"$url","size_bytes":1543210,""")
        append(""""sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}""")
        if (minFrom != null) append(""","min_from_version":"$minFrom"""")
        append("}")
    }

    @Test
    fun `a release check fetches the manifest and offers a newer version`() = runTest {
        // The whole point of N-12: the app checks over its OWN connection (spec
        // 9.5) because the ESP32 may have no WiFi in the car. Before this the app
        // reported "this build cannot check" and the user had no way to know a
        // release existed without standing in the car with a laptop.
        val t = FakeTransport()
        val fetcher = FakeFetcher(body = releaseManifest("9.9.9"))
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()

        vm.checkForUpdates()
        advanceUntilIdle()

        assertEquals("the manifest must actually be fetched", 1, fetcher.fetches)
        val status = vm.update.value.status
        assertTrue("a newer release must be offered, was $status", status is UpdateStatus.Newer)
        assertEquals("1.0.0", (status as UpdateStatus.Newer).current)
        assertEquals("9.9.9", status.available)
        assertFalse("the check finished", vm.update.value.inProgress)
    }

    @Test
    fun `the running version is compared against the manifest, not assumed`() = runTest {
        // Up to date is only reachable after a REAL comparison: the manifest's
        // version equals the device's. This is what the old build could never say.
        val t = FakeTransport()
        val fetcher = FakeFetcher(body = releaseManifest("2.5.0"))
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"2.5.0\"", "protocol_v" to "1"))
        advanceUntilIdle()

        vm.checkForUpdates()
        advanceUntilIdle()

        assertTrue(
            "an equal version is up to date: ${vm.update.value.status}",
            vm.update.value.status is UpdateStatus.UpToDate,
        )
    }

    @Test
    fun `a version below the manifest floor is not offered`() = runTest {
        // Spec 9.5 step 4: `min_from_version` gates an update that needs a staged
        // migration. The device MUST NOT be offered it, and the user must be told
        // why rather than shown nothing.
        val t = FakeTransport()
        val fetcher = FakeFetcher(body = releaseManifest("9.9.9", minFrom = "5.0.0"))
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()

        vm.checkForUpdates()
        advanceUntilIdle()

        val status = vm.update.value.status
        assertTrue(
            "a below-floor device must not be offered the update: $status",
            status is UpdateStatus.TooOldToUpgradeFrom,
        )
        assertEquals("5.0.0", (status as UpdateStatus.TooOldToUpgradeFrom).minFrom)
    }

    @Test
    fun `a failed fetch is reported and the device is declared unaffected`() = runTest {
        // A network failure must be a message, not a crash and not a false "up to
        // date". The device is running whatever it was, and the copy says so.
        val t = FakeTransport()
        val fetcher = FakeFetcher(fail = java.io.IOException("no route to host"))
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()

        vm.checkForUpdates()
        advanceUntilIdle()

        val status = vm.update.value.status
        assertTrue("a transport failure is a Failed, was $status", status is UpdateStatus.Failed)
        assertTrue(
            "the message names the cause: ${(status as UpdateStatus.Failed).reason}",
            status.reason.contains("no route to host"),
        )
        assertFalse("a failure must not leave the button disabled forever",
            vm.update.value.inProgress)
    }

    @Test
    fun `a malformed manifest is a failure, never a false up to date`() = runTest {
        val t = FakeTransport()
        val fetcher = FakeFetcher(body = "{ not a manifest")
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()

        vm.checkForUpdates()
        advanceUntilIdle()

        assertTrue(
            "an unreadable manifest must not read as current: ${vm.update.value.status}",
            vm.update.value.status is UpdateStatus.Failed,
        )
    }

    @Test
    fun `a check with no device connected does not fetch`() = runTest {
        val t = FakeTransport()
        val fetcher = FakeFetcher(body = releaseManifest("9.9.9"))
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        // No hello: firmwareVersion is null.
        vm.checkForUpdates()
        advanceUntilIdle()

        assertEquals("no version to compare: do not hit the network", 0, fetcher.fetches)
        assertTrue(vm.update.value.status is UpdateStatus.Failed)
    }

    // --- spec 9.5 step 5: download, verify, push over USB ------------------

    private class FakeDownloader(var image: ByteArray = ByteArray(0), var fail: Exception? = null) :
        com.oetsolutions.swc.update.ImageDownloader {
        var downloads = 0
        override suspend fun download(url: String): ByteArray {
            ++downloads
            fail?.let { throw it }
            return image
        }
    }

    private fun sha256Hex(data: ByteArray): String =
        java.security.MessageDigest.getInstance("SHA-256")
            .digest(data).joinToString("") { "%02x".format(it) }

    /** A manifest whose declared size and sha256 match `image`. */
    private fun manifestForImage(version: String, image: ByteArray): String =
        """{"latest_version":"$version","channel":"stable",
            "firmware":{"version":"$version",
            "url":"https://github.com/oetsolutions/swc-module/releases/download/v$version/firmware.bin",
            "size_bytes":${image.size},"sha256":"${sha256Hex(image)}"}}"""

    @Test
    fun `installing a found release downloads, verifies and pushes it`() = runTest {
        // Spec §9.5's whole app path: check over the phone's connection, then push
        // the resulting file over USB. Without this the app could tell the user a
        // release existed and then offer no way to get it.
        val t = FakeTransport()
        t.autoAck = true
        val image = ByteArray(1200) { (it % 251).toByte() }
        val fetcher = FakeFetcher(body = manifestForImage("9.9.9", image))
        val downloader = FakeDownloader(image = image)
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            downloadImage = downloader,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()

        vm.checkForUpdates()
        advanceUntilIdle()
        assertTrue(vm.update.value.status is UpdateStatus.Newer)

        vm.installAvailableUpdate()
        advanceUntilIdle()

        assertEquals("the image must be downloaded", 1, downloader.downloads)
        assertEquals(
            "a verified image is pushed",
            PushResult.Installed,
            vm.update.value.pushResult,
        )
        // And the push actually went over the wire.
        val types = t.written.mapNotNull { Regex("\"type\":\"(\\w+)\"").find(it)?.groupValues?.get(1) }
        assertTrue("the image reached the device: $types", "ota_end" in types)
    }

    @Test
    fun `a download that fails its checksum is NOT pushed`() = runTest {
        // Spec §9.5 step 3. A substituted or corrupted image must be discarded whole
        // -- the device's own gate would refuse it too, but refusing here means a bad
        // image never occupies the link, and the message names the cause.
        val t = FakeTransport()
        t.autoAck = true
        val image = ByteArray(1200) { 7 }
        val fetcher = FakeFetcher(body = manifestForImage("9.9.9", image))
        // The downloader hands back DIFFERENT bytes than the manifest hashed.
        val downloader = FakeDownloader(image = ByteArray(1200) { 9 })
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            downloadImage = downloader,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()
        vm.checkForUpdates()
        advanceUntilIdle()

        vm.installAvailableUpdate()
        advanceUntilIdle()

        val result = vm.update.value.pushResult
        assertTrue("a bad checksum must be refused: $result", result is PushResult.Failed)
        assertTrue(
            "the message names the checksum: ${(result as PushResult.Failed).reason}",
            result.reason.contains("checksum"),
        )
        val types = t.written.mapNotNull { Regex("\"type\":\"(\\w+)\"").find(it)?.groupValues?.get(1) }
        assertFalse("nothing must reach the device: $types", "ota_begin" in types)
    }

    @Test
    fun `a download of the wrong size is refused before the checksum`() = runTest {
        // Size first, then digest -- the same order the device's `ImageVerifyEnd`
        // uses, so a truncated image reports the specific cause rather than the
        // vaguer checksum failure.
        val t = FakeTransport()
        t.autoAck = true
        val declared = ByteArray(1200) { 1 }
        val fetcher = FakeFetcher(body = manifestForImage("9.9.9", declared))
        val downloader = FakeDownloader(image = ByteArray(1000) { 1 })  // short
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            downloadImage = downloader,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()
        vm.checkForUpdates()
        advanceUntilIdle()

        vm.installAvailableUpdate()
        advanceUntilIdle()

        val result = vm.update.value.pushResult
        assertTrue(result is PushResult.Failed)
        assertTrue(
            "the message names the size: ${(result as PushResult.Failed).reason}",
            result.reason.contains("1200") || result.reason.contains("size") ||
                result.reason.contains("bytes"),
        )
    }

    @Test
    fun `installing does nothing when no release was found`() = runTest {
        // The state you act on must be the state you last observed: with no
        // available release (an up-to-date check, or none run), install must not
        // download anything.
        val t = FakeTransport()
        t.autoAck = true
        val downloader = FakeDownloader(image = ByteArray(10))
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = FakeFetcher(body = releaseManifest("1.0.0")),
            downloadImage = downloader,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()
        vm.checkForUpdates()   // equal -> UP_TO_DATE
        advanceUntilIdle()

        vm.installAvailableUpdate()
        advanceUntilIdle()

        assertEquals("no release to install: do not download", 0, downloader.downloads)
        assertNull(vm.update.value.pushResult)
    }

    @Test
    fun `a later check that finds no release clears the installable one`() = runTest {
        // A stale release must not survive a later check that found none: otherwise
        // a user who checks, sees nothing to install, then somehow taps install gets
        // the OLD release.
        val t = FakeTransport()
        t.autoAck = true
        val image = ByteArray(600) { 3 }
        val fetcher = FakeFetcher(body = manifestForImage("9.9.9", image))
        val downloader = FakeDownloader(image = image)
        val vm = AppViewModel(
            SwcClient(t),
            scope = vmScope(),
            fetchManifest = fetcher,
            downloadImage = downloader,
            ioDispatcher = kotlinx.coroutines.test.StandardTestDispatcher(testScheduler),
        )
        started(vm)
        t.emit(frame("hello", "fw_version" to "\"1.0.0\"", "protocol_v" to "1"))
        advanceUntilIdle()
        vm.checkForUpdates()
        advanceUntilIdle()
        assertTrue(vm.update.value.status is UpdateStatus.Newer)

        // The release is pulled: the next check finds the device already current.
        fetcher.body = releaseManifest("1.0.0")
        vm.checkForUpdates()
        advanceUntilIdle()
        assertTrue(vm.update.value.status is UpdateStatus.UpToDate)

        vm.installAvailableUpdate()
        advanceUntilIdle()

        assertEquals("the stale release must not be installed", 0, downloader.downloads)
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

    @Test
    fun `retry re-enumerates the bus rather than re-pinging a dead transport`() = runTest {
        // **"Try again" could not recover anything.** It called `connect()`, which
        // sends a `ping`; `UsbSerialTransport.write` returns early when no
        // connection is open, so on the exact failure the button exists for -- the
        // device was not plugged in when the app started, and nothing ever
        // enumerated again -- the retry wrote nothing and re-reported the same
        // problem forever.
        val t = FakeTransport()
        t.reopenProblem = LinkProblem.NoDevice
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)

        vm.retry()
        advanceUntilIdle()

        assertEquals("a retry must re-run enumeration", 1, t.reopened)
        assertEquals(LinkProblem.NoDevice, vm.link.value.problem)

        // Once the device appears, the same button must connect: the enumeration
        // succeeds and a frame goes out.
        t.reopenProblem = null
        vm.retry()
        advanceUntilIdle()
        assertEquals(2, t.reopened)
        assertNull("a successful reopen must clear the stale problem", vm.link.value.problem)
        assertTrue("the retry must actually talk to the device now",
            t.written.isNotEmpty())
    }

    @Test
    fun `close cancels the view model's own scope so its collectors do not leak`() = runTest {
        // The other half of `onDestroy`: it closed the transport but never cancelled
        // the view model's coroutines, so the five collectors and the never-returning
        // `client.run()` kept the whole object graph alive after the activity was
        // gone. `client.run()` cannot stop on its own, so cancelling the scope is the
        // only thing that ends it.
        val t = FakeTransport()
        val owned = CoroutineScope(SupervisorJob() + StandardTestDispatcher(testScheduler))
        val vm = AppViewModel(SwcClient(t), scope = owned, ownsScope = true)
        started(vm)

        assertTrue("the harness scope must be live before close",
            owned.isActive)
        vm.close()
        assertFalse("close must cancel a scope this view model owns", owned.isActive)
    }

    @Test
    fun `close leaves an injected scope alive`() = runTest {
        // The tests pass their own scope on the test scheduler; cancelling it would
        // tear down the harness. Only a scope the view model created is its to end.
        val t = FakeTransport()
        val injected = vmScope()
        val vm = AppViewModel(SwcClient(t), scope = injected, ownsScope = false)
        started(vm)

        vm.close()
        assertTrue("an injected scope is the caller's to cancel", injected.isActive)
        injected.cancel()
    }

    // --- USB OTA push (spec 9.3, N-14) -------------------------------------

    @Test
    fun `a push with no device reports refused, not a false success`() = runTest {
        // The client refuses locally (no `ota_begin` reaches the wire) because no
        // device answers, so the result is the caller-visible outcome after the
        // request times out. What matters is the screen never sees `Installed` for
        // a transfer that did not happen.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        vm.pushFirmwareOverUsb(ByteArray(512) { 1 })
        advanceUntilIdle()
        val r = vm.update.value.pushResult
        assertTrue("a push that never ran must not report Installed", r !is PushResult.Installed)
        assertFalse("the push flag must clear", vm.update.value.pushInProgress)
    }

    @Test
    fun `an empty image is refused locally with the empty reason`() = runTest {
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        vm.pushFirmwareOverUsb(ByteArray(0))
        advanceUntilIdle()
        val r = vm.update.value.pushResult
        assertTrue(r is PushResult.Refused)
        assertTrue(
            "the reason must name the empty file",
            (r as PushResult.Refused).reason.contains("empty"),
        )
        assertTrue("nothing may reach the wire", t.written.isEmpty())
    }

    @Test
    fun `a not_supported refusal reads as a build with no slot, not a generic failure`() = runTest {
        // A host/dev image has no partitions; the device answers `not_supported`.
        // The screen must say what that means rather than surface a code.
        val t = FakeTransport()
        val vm = AppViewModel(SwcClient(t), scope = vmScope())
        started(vm)
        vm.pushFirmwareOverUsb(ByteArray(1_966_080 + 1))   // oversize -> local too_large
        advanceUntilIdle()
        val r = vm.update.value.pushResult
        assertTrue(r is PushResult.Refused)
        assertTrue("nothing may reach the wire for an oversize image", t.written.isEmpty())
    }

    @Test
    fun `describePush says the device is unaffected when an update fails`() {
        // The screen's core promise (spec 9): "a failed update does not leave you
        // with a dead adapter." Every non-install outcome must say so.
        val (okR, refused) = com.oetsolutions.swc.ui.describePush(PushResult.Refused("bad size"))
        assertFalse(okR)
        assertTrue("a refusal must say nothing changed", refused.contains("still running"))

        val (okF, failed) = com.oetsolutions.swc.ui.describePush(PushResult.Failed("timeout"))
        assertFalse(okF)
        assertTrue("a failure must say the old image kept running",
            failed.contains("current version"))

        val (okI, installed) = com.oetsolutions.swc.ui.describePush(PushResult.Installed)
        assertTrue(okI)
        assertTrue("a success must still mention the reboot", installed.contains("Reboot"))
    }

    // --- helpers -----------------------------------------------------------

    private fun crcOf(data: ByteArray): Long {        var crc = 0xFFFFFFFFL
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
