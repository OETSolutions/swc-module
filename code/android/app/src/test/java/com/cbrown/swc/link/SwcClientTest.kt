package com.oetsolutions.swc.link

import com.oetsolutions.swc.model.ConfigJson
import com.oetsolutions.swc.model.sampleConfig
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The protocol client's logic, with no device attached.
 *
 * The transport is a fake because everything under test -- reassembly across
 * arbitrary read boundaries, version-mismatch surfacing, chunked config -- is
 * pure logic. Anything that needed real USB would be an instrumented test, and
 * there is nothing here that does.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class SwcClientTest {

    private class FakeTransport : SwcTransport {
        // Synchronized: the concurrency test below writes from two real threads, and
        // an unsynchronized `MutableList` would corrupt under that regardless of what
        // the client does.
        val written = java.util.Collections.synchronizedList(mutableListOf<String>())
        private val flow = MutableSharedFlow<ByteArray>(extraBufferCapacity = 512)
        // Opt-in: several tests depend on an unanswered request timing out, so the
        // default transport stays silent like a device that never replied.
        var autoAck = false
        override suspend fun write(bytes: ByteArray) {
            val s = String(bytes)
            written += s
            if (!autoAck) return
            // A real device answers every command. The buffer is large so the ack
            // queues even when the reader is momentarily behind.
            val seq = Regex("\"seq\":(\\d+)").find(s)?.groupValues?.get(1)?.toInt() ?: return
            flow.tryEmit("{\"v\":1,\"seq\":$seq,\"type\":\"ack\",\"for_seq\":$seq}\n".toByteArray())
        }
        override val incoming: Flow<ByteArray> = flow
        override fun close() {}
        /** How many collectors are attached, so a test can wait for a real subscriber. */
        val subscribers: Int get() = flow.subscriptionCount.value
        suspend fun emit(text: String) = flow.emit(text.toByteArray())
    }

    /**
     * Start the client's receive loop and let it SUBSCRIBE before anything is
     * emitted.
     *
     * This is not test ceremony. `incoming` is a `SharedFlow` with no replay, so a
     * frame emitted before the collector attaches is dropped -- and `runTest`'s
     * scheduler does not start a `launch` until it is advanced, so the first
     * version of these tests emitted into the void and failed. The same race
     * exists in production between starting the transport and collecting, which is
     * why the app must collect before it connects.
     */
    private fun kotlinx.coroutines.test.TestScope.startClient(client: SwcClient) =
        launch { client.run() }.also { advanceUntilIdle() }

    /** The `seq` a request put on the wire, so a reply can name it in `for_seq`. */
    private fun seqOf(wire: String): String =
        Regex("\"seq\":(\\d+)").find(wire)?.groupValues?.get(1)
            ?: error("no seq in $wire")

    @Test
    fun `config round trips through the codec unchanged`() {
        val c = sampleConfig()
        val decoded = ConfigJson.decode(ConfigJson.encode(c))
        assertEquals(c, decoded)
    }

    @Test
    fun `frames split across reads are reassembled`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val seen = mutableListOf<Frame>()
        val job = startClient(client)
        val collector = launch { client.frames.collect { seen += it } }
        advanceUntilIdle()
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"he")
        t.emit("llo\"}\n")
        advanceUntilIdle()
        assertEquals(1, seen.size)
        assertEquals("hello", seen[0].type)
        job.cancel(); collector.cancel()
    }

    @Test
    fun `two frames in one read both arrive`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val seen = mutableListOf<Frame>()
        val job = startClient(client)
        val collector = launch { client.frames.collect { seen += it } }
        advanceUntilIdle()
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"hello\"}\n{\"v\":1,\"seq\":2,\"type\":\"status\"}\n")
        advanceUntilIdle()
        assertEquals(listOf("hello", "status"), seen.map { it.type })
        job.cancel(); collector.cancel()
    }

    @Test
    fun `a version mismatch surfaces rather than being swallowed`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        t.emit("{\"v\":99,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        assertEquals(LinkState.VersionMismatch(firmware = 99, app = 1), client.state.value)
        job.cancel()
    }

    @Test
    fun `a version mismatch nack also surfaces`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"nack\",\"for_seq\":1,\"err\":\"version_mismatch\"}\n")
        advanceUntilIdle()
        assertTrue(client.state.value is LinkState.VersionMismatch)
        job.cancel()
    }

    @Test
    fun `an oversized line is dropped and reported instead of growing unbounded`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        t.emit("x".repeat(1100))   // no newline: never a complete frame
        advanceUntilIdle()
        assertTrue(client.state.value is LinkState.Failed)
        job.cancel()
    }

    @Test
    fun `a locally invalid config is refused before it reaches the wire`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        // OUT_VOLTAGE with no key_mv: the device would refuse it, so the client
        // must refuse it locally and name the field.
        val bad = sampleConfig().copy(
            bindings = listOf(
                com.oetsolutions.swc.model.Binding(
                    id = "b1",
                    channel = com.oetsolutions.swc.model.BindingChannel.SWC1,
                    button = "vol_up",
                    gesture = com.oetsolutions.swc.model.Gesture.SINGLE,
                    actions = listOf(com.oetsolutions.swc.model.Action(
                        kind = com.oetsolutions.swc.contract.ActionKind.OUT_VOLTAGE,
                        keyMv = 0)),
                )
            )
        )
        val result = client.setConfig(bad)
        assertTrue("expected a local refusal, got $result", result is AckResult.Nacked)
        assertTrue(t.written.isEmpty())
    }
    // ---------------------------------------------------------------- inbound config
    //
    // These cover the seam that let a real bug through: the client looked for a
    // `config` field on `config_end`, which the firmware never sends. It carried
    // the whole assembled config in one frame. Every test passed because none of
    // them fed the client the frames it actually receives.

    /** Build the device's three frame types for a config body. */
    private fun configRun(body: String): List<String> {
        val bytes = body.toByteArray()
        val crc = crc32ForTest(bytes)
        val out = mutableListOf<String>()
        out += "{\"v\":1,\"seq\":1,\"type\":\"config_begin\"," +
            "\"total_len\":${bytes.size},\"crc32\":$crc}"
        var off = 0
        while (off < bytes.size) {
            val end = minOf(off + 512, bytes.size)
            val chunk = java.util.Base64.getEncoder()
                .encodeToString(bytes.copyOfRange(off, end))
            out += "{\"v\":1,\"seq\":2,\"type\":\"config_chunk\"," +
                "\"offset\":$off,\"data_b64\":\"$chunk\"}"
            off = end
        }
        out += "{\"v\":1,\"seq\":3,\"type\":\"config_end\",\"sha256\":\"${sha256ForTest(bytes)}\"}"
        return out
    }

    @Test
    fun `a chunked config reply is assembled into the local model`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        val body = com.oetsolutions.swc.model.ConfigJson.encode(sampleConfig())
        configRun(body).forEach { t.emit(it + "\n") }
        advanceUntilIdle()
        assertEquals("swc-a1b2c3", client.config.value.deviceId)
        assertEquals(sampleConfig(), client.config.value)
        job.cancel()
    }

    @Test
    fun `a config whose sha256 does not match is not adopted`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        val body = com.oetsolutions.swc.model.ConfigJson.encode(sampleConfig())
        // Same frames, digest replaced. The bytes are intact and parse fine, so a
        // client that skipped the digest would adopt them -- which is the whole
        // reason the run carries one.
        configRun(body).dropLast(1).forEach { t.emit(it + "\n") }
        t.emit("{\"v\":1,\"seq\":3,\"type\":\"config_end\",\"sha256\":\"deadbeef\"}\n")
        advanceUntilIdle()
        assertEquals("", client.config.value.deviceId)
        assertTrue(client.state.value is LinkState.Failed)
        job.cancel()
    }

    @Test
    fun `a config run that ends early is reported rather than half-applied`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        val body = com.oetsolutions.swc.model.ConfigJson.encode(sampleConfig())
        val frames = configRun(body)
        // Begin and end, but skip the chunks between them.
        t.emit(frames.first() + "\n")
        t.emit(frames.last() + "\n")
        advanceUntilIdle()
        assertEquals("", client.config.value.deviceId)
        assertTrue(client.state.value is LinkState.Failed)
        job.cancel()
    }

    @Test
    fun `a reply waiting on for_seq times out rather than hanging forever`() = runTest {
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        // Nothing is ever emitted. The request must give up, because a UI that
        // awaits forever is a spinner with no exit.
        val result = client.setConfig(sampleConfig(), timeoutMs = 100)
        assertTrue("expected a timeout, got $result", result is AckResult.Timeout)
        job.cancel()
    }

    @Test
    fun `getConfig returns as soon as the run ends, without waiting out its timeout`() = runTest {
        // The firmware answers `config_get` with the chunked run itself, and no
        // frame of that run carries a `for_seq`. Waiting on one therefore never
        // matched, so `getConfig` returned only after its FULL timeout -- which is
        // what `AppViewModel.connect()` did on every launch. A generous timeout
        // with a fast run is what tells the two behaviors apart: the fix returns
        // promptly, the bug blocks for the whole 15 s.
        //
        // `runCurrent`, not `advanceUntilIdle`: advancing would jump virtual time
        // past the 15 s timeout and complete the call the wrong way, hiding the
        // very bug this asserts against.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        val body = com.oetsolutions.swc.model.ConfigJson.encode(sampleConfig())

        val call = launch { client.getConfig(timeoutMs = 15_000) }
        runCurrent()
        assertTrue("the request must go out", t.written.any { it.contains("\"type\":\"config_get\"") })

        configRun(body).forEach { t.emit(it + "\n") }
        runCurrent()

        assertTrue("the run must resolve the call", call.isCompleted)
        assertEquals(sampleConfig(), client.config.value)
        job.cancel()
    }

    @Test
    fun `two overlapping getConfig calls are both resolved by the one run`() = runTest {
        // Overlap is reachable in normal use (a connect while the update screen
        // re-reads the config). A single waiter slot would let the second call
        // overwrite the first's, so the first would time out on a link that
        // answered it -- the same shape the `awaiting` map was made concurrent
        // for. One run ends both callers.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        val body = com.oetsolutions.swc.model.ConfigJson.encode(sampleConfig())

        val a = launch { client.getConfig(timeoutMs = 15_000) }
        runCurrent()
        val b = launch { client.getConfig(timeoutMs = 15_000) }
        runCurrent()

        configRun(body).forEach { t.emit(it + "\n") }
        runCurrent()

        assertTrue("the first call must be resolved too", a.isCompleted)
        assertTrue("the second call must be resolved", b.isCompleted)
        job.cancel()
    }

    @Test
    fun `getConfig still gives up when the device never runs`() = runTest {
        // The other direction: waiting on the run must not become waiting
        // forever. A device that accepts the request and stays silent has to
        // time out, or the connect spinner never exits.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        val call = launch { client.getConfig(timeoutMs = 100) }
        runCurrent()
        advanceTimeBy(1_000)
        assertTrue("must not hang on a silent device", call.isCompleted)
        job.cancel()
    }

    private fun crc32ForTest(data: ByteArray): Long {
        var crc = 0xFFFFFFFFL
        for (b in data) {
            crc = crc xor (b.toLong() and 0xFF)
            for (i in 0 until 8) {
                crc = if (crc and 1L != 0L) (crc ushr 1) xor 0xEDB88320L else crc ushr 1
            }
        }
        return (crc xor 0xFFFFFFFFL) and 0xFFFFFFFFL
    }

    private fun sha256ForTest(data: ByteArray): String =
        java.security.MessageDigest.getInstance("SHA-256")
            .digest(data).joinToString("") { "%02x".format(it) }

    @Test
    fun `entering maintenance sends the frame the firmware actually implements`() = runTest {
        // Spec §8.2 calls `maintenance_enter` the "primary, from the Android app"
        // trigger, and the app had no way to send it: the frame constant, the
        // trigger enum and the explanatory link message all existed, but no sender
        // did. Maintenance is what turns on WiFi, so without this the app cannot
        // get a device onto its provisioning page at all.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        advanceUntilIdle()

        val call = launch { client.enterMaintenance() }
        advanceUntilIdle()

        // The request must be the real frame type, with the envelope the firmware's
        // dispatcher keys on.
        val sent = t.written.last()
        assertTrue("must send maintenance_enter: $sent", sent.contains("\"type\":\"maintenance_enter\""))
        assertTrue("must carry a seq the reply can name", sent.contains("\"seq\":"))

        // And it must complete on the ack the firmware sends, not time out.
        t.emit("{\"v\":1,\"seq\":99,\"type\":\"ack\",\"for_seq\":${seqOf(sent)},\"ok\":true}\n")
        advanceUntilIdle()
        assertTrue("the ack must resolve the call", call.isCompleted)

        job.cancel()
    }

    @Test
    fun `leaving maintenance sends its own frame, distinctly from entering`() = runTest {
        // The two must not be the same call: exiting a closed window is idempotent
        // on the firmware, entering one is not, and swapping them would leave a user
        // unable to get out of maintenance from the app.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        advanceUntilIdle()

        val call = launch { client.exitMaintenance() }
        advanceUntilIdle()
        val sent = t.written.last()
        assertTrue("must send maintenance_exit, not _enter: $sent",
            sent.contains("\"type\":\"maintenance_exit\""))

        t.emit("{\"v\":1,\"seq\":99,\"type\":\"ack\",\"for_seq\":${seqOf(sent)},\"ok\":true}\n")
        advanceUntilIdle()
        assertTrue(call.isCompleted)
        job.cancel()
    }

    @Test
    fun `a good frame after a malformed one recovers the link instead of staying failed`() = runTest {
        // The failure state was terminal until the next `hello`, which only arrives
        // on reconnect -- so ONE torn line froze the UI on "No device found" while
        // the device kept answering. This was reachable in normal use: the firmware's
        // own `hello` was malformed, so the app failed on the very first frame.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        assertEquals(LinkState.Connected, client.state.value)

        t.emit("not json\n")
        advanceUntilIdle()
        assertTrue(client.state.value is LinkState.Failed)

        // Any well-formed frame proves the peer is talking again. Use `status`, NOT
        // `hello`: recovery must not depend on a reconnect.
        t.emit("{\"v\":1,\"seq\":2,\"type\":\"status\"}\n")
        advanceUntilIdle()
        assertEquals(LinkState.Connected, client.state.value)
        job.cancel()
    }

    @Test
    fun `a config error is not erased by the frame that reported it`() = runTest {
        // The opposite pressure: the frame that reports a bad digest is itself
        // well-formed, so a naive "a good frame clears the failure" would clear the
        // error on the same frame that raised it.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        val body = com.oetsolutions.swc.model.ConfigJson.encode(sampleConfig())
        configRun(body).dropLast(1).forEach { t.emit(it + "\n") }
        t.emit("{\"v\":1,\"seq\":3,\"type\":\"config_end\",\"sha256\":\"deadbeef\"}\n")
        advanceUntilIdle()
        assertTrue("the digest failure must survive its own frame",
            client.state.value is LinkState.Failed)
        job.cancel()
    }

    @Test
    fun `a version mismatch is not cleared by later well-formed frames`() = runTest {
        // Spec 4.5 requires the app to stop talking on a mismatch. Recovery would
        // turn that into best-effort parsing, which is what the rule forbids.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        t.emit("{\"v\":99,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        assertTrue(client.state.value is LinkState.VersionMismatch)
        t.emit("{\"v\":99,\"seq\":2,\"type\":\"status\"}\n")
        advanceUntilIdle()
        assertTrue("a mismatch is a property of the peer, not a transient",
            client.state.value is LinkState.VersionMismatch)
        job.cancel()
    }

    @Test
    fun `overlapping requests on real threads each resolve`() {
        // NOT a `runTest`: this race needs genuine parallelism. `runTest`'s scheduler
        // runs its coroutines on ONE thread, so its `launch`es can never interleave
        // between allocating `seq` and taking the write lock -- an earlier version of
        // this test passed against the racy client for exactly that reason.
        //
        // The client is documented to be driven from several coroutines on the app's
        // multi-threaded dispatcher, so the overlap is a production shape.
        //
        // The observable is the OUTCOME, not the wire bytes: the racy client still put
        // distinct `seq`s on the wire (the counter increments under the lock), but it
        // registered the reply WAITER under a key read outside the lock, so a thread's
        // waiter was overwritten and its acknowledged request timed out. Every racer
        // gets its own ack here, so any timeout means a waiter was lost.
        val racers = 16
        val t = FakeTransport()
        t.autoAck = true
        val client = SwcClient(t)
        val reader = Thread { runBlocking { client.run() } }.apply { isDaemon = true; start() }
        // Wait until the client has actually SUBSCRIBED before racing. `incoming` is
        // a SharedFlow with no replay, so an ack emitted before the reader attaches is
        // dropped -- and a dropped ack looks exactly like a collision, which made an
        // earlier version of this test flaky under a loaded build.
        val deadline = System.currentTimeMillis() + 5_000
        while (t.subscribers < 1 && System.currentTimeMillis() < deadline) Thread.sleep(1)
        assertEquals("the client reader never subscribed", 1, t.subscribers)

        repeat(30) {
            val gate = java.util.concurrent.CyclicBarrier(racers)
            val results = java.util.Collections.synchronizedList(mutableListOf<AckResult>())
            val threads = (0 until racers).map {
                Thread {
                    runBlocking {
                        gate.await(3, java.util.concurrent.TimeUnit.SECONDS)
                        results += client.enterMaintenance(3_000)
                    }
                }.apply { isDaemon = true; start() }
            }
            threads.forEach { it.join(5_000) }
            val timeouts = results.count { it is AckResult.Timeout }
            assertEquals("every acknowledged request must resolve; $timeouts timed out",
                0, timeouts)
        }
        reader.interrupt()
        client.close()
    }
}
