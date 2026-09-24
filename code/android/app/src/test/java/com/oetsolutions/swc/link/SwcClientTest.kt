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
import org.junit.Assert.assertFalse
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
        // When set, the named frame `type` is answered with a `nack` of `nackErr`
        // instead of an ack, so a test can exercise the client's mid-run abort.
        var nackType: String? = null
        var nackErr: String = "gap"
        override suspend fun write(bytes: ByteArray) {
            val s = String(bytes)
            written += s
            if (!autoAck) return
            // A real device answers every command. The buffer is large so the ack
            // queues even when the reader is momentarily behind.
            val seq = Regex("\"seq\":(\\d+)").find(s)?.groupValues?.get(1)?.toInt() ?: return
            val type = Regex("\"type\":\"(\\w+)\"").find(s)?.groupValues?.get(1)
            val reply = if (type != null && type == nackType) {
                "{\"v\":1,\"seq\":$seq,\"type\":\"nack\",\"for_seq\":$seq,\"err\":\"$nackErr\",\"detail\":\"\"}\n"
            } else {
                "{\"v\":1,\"seq\":$seq,\"type\":\"ack\",\"for_seq\":$seq}\n"
            }
            flow.tryEmit(reply.toByteArray())
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
    fun `a version mismatch stops the app talking`() = runTest {
        // Spec 4.5: on a mismatch the app must STOP TALKING, not carry on
        // best-effort -- "silent partial compatibility is how a config gets
        // corrupted". The state was set, but nothing gated outbound frames: after a
        // mismatched `hello` a `connect()` still wrote `ping`, a `getConfig()`
        // still wrote `config_get`, and a config push still wrote every chunk --
        // all onto a peer whose vocabulary disagrees with ours.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        t.emit("{\"v\":99,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        assertTrue(client.state.value is LinkState.VersionMismatch)

        val before = t.written.size
        client.connect()
        client.getConfig(timeoutMs = 1)
        advanceUntilIdle()
        assertEquals(
            "no frame may reach the wire once the versions disagree",
            before, t.written.size,
        )
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
    fun `a config_begin with an out-of-range total_len releases the getConfig waiter`() = runTest {
        // A `total_len` past the protocol maximum makes the run unusable, so the
        // client goes to Failed -- and MUST release the pending `getConfig`. It
        // used to set `inbound = null` and fail the state without calling
        // `finishConfigRun`, so the waiter hung its full 15 s timeout: a spinner
        // with no exit for a run the device had already made impossible.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)

        val call = launch { client.getConfig(timeoutMs = 15_000) }
        runCurrent()
        assertTrue("the request must go out", t.written.any { it.contains("\"type\":\"config_get\"") })

        // A begin whose total_len is absurdly large, and no further frames.
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"config_begin\",\"total_len\":99999999,\"crc32\":1}\n")
        runCurrent()

        assertTrue("the bad begin must release the waiter, not leave it hanging", call.isCompleted)
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
    fun `a config run whose bytes are not a JSON object does not kill the link`() = runTest {
        // A run can pass BOTH digests and still be undecodable: the crc32 and the
        // sha256 are over the raw bytes, so any byte string whose digests the
        // device computed correctly satisfies them, and the config is only
        // required to be UTF-8 JSON by the ENCODER. A peer (or a device whose
        // config slot is corrupt in a way it still digests) can deliver a body
        // that parses as JSON but is not an object -- `parseToJsonElement` then
        // succeeds, `.jsonObject` throws. `handle()` guards its OWN parse because
        // "a malformed frame from a peer is not fatal to the link", but
        // `endInboundConfig` decoded directly, so the throw escaped `handle`,
        // escaped the collector, and ended `run()` -- the link's only consumer.
        // The app then went deaf for the rest of the session while the device
        // kept talking, and no frame could recover it.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)

        // A body that is valid JSON, has valid digests, and is not an object.
        val body = "[1,2,3]"
        configRun(body).forEach { t.emit(it + "\n") }
        runCurrent()

        assertTrue("the run must be reported, not thrown",
            client.state.value is LinkState.Failed)
        assertEquals("the local model must be left alone", "", client.config.value.deviceId)

        // The link must still be alive: a later good frame is handled, which it
        // cannot be if the collector died on the throw.
        t.emit("{\"v\":1,\"seq\":42,\"type\":\"hello\",\"protocol_v\":1,\"caps\":[]}\n")
        runCurrent()
        assertTrue("the link must survive a bad config run", client.state.value is LinkState.Connected)
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

    @Test
    fun `getConfig does not wait out its timeout once the versions disagree`() = runTest {
        // Spec 4.5's silence has ONE more caller than the reply-bearing requests.
        // `sendLocked` already refuses to put `config_get` on the wire on a
        // mismatch, but `getConfig` also registers a waiter that only a config RUN
        // can end -- and with no frame out, no run can come. So it parked on its
        // full 15 s timeout on a silence the app chose, which is what `connect()`
        // hits on every launch against a mismatched device. The early return must
        // resolve it with the unchanged local model instead.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        t.emit("{\"v\":99,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        assertTrue(client.state.value is LinkState.VersionMismatch)

        val before = t.written.size
        val call = launch { client.getConfig(timeoutMs = 15_000) }
        runCurrent()
        assertTrue(
            "getConfig must not park for its whole timeout under a version mismatch",
            call.isCompleted,
        )
        assertEquals(
            "and it must still send nothing",
            before, t.written.size,
        )
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
    fun `a config error is not erased when the link was ALREADY failed`() = runTest {
        // The recovery rule is "a well-formed frame clears a failure raised by an
        // EARLIER frame". The guard was `failedBeforeThisFrame && Failed`, read AFTER
        // dispatch -- and that cannot tell a STALE failure from one this very frame
        // just raised: both read as Failed with the flag set. So a frame that both
        // started on a Failed state AND raised a new failure erased its own error.
        //
        // `config_begin` is the frame that does both: an out-of-range `total_len` is
        // refused INSIDE the dispatch, on a frame that is itself well-formed. Reached
        // after any earlier malformed line, the bound check's failure vanished and
        // the link reported a healthy `Connected` while every config request was
        // rejected -- the exact "detected but not reported" failure.
        val t = FakeTransport()
        val client = SwcClient(t)
        val job = startClient(client)
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        assertEquals(LinkState.Connected, client.state.value)

        // A stale failure: a malformed line.
        t.emit("not json\n")
        advanceUntilIdle()
        assertTrue(client.state.value is LinkState.Failed)

        // A well-formed `config_begin` whose declared length the wire cannot carry.
        // `beginInboundConfig` raises a failure for it -- on the SAME frame.
        t.emit("{\"v\":1,\"seq\":2,\"type\":\"config_begin\"," +
            "\"total_len\":99999999,\"crc32\":0}\n")
        advanceUntilIdle()

        assertTrue(
            "an out-of-range config_begin must report its own failure even when the " +
                "link was already failed: was ${client.state.value}",
            client.state.value is LinkState.Failed,
        )
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

    // ---------------------------------------------------------------- USB OTA
    // Spec §9.3's in-car update path. The firmware side is wired (N-14); these
    // cover the app half -- that a push emits begin/chunk/end in order, that the
    // offsets are the DECODED byte offsets (the same rule config uses), and that an
    // oversize or empty image is refused locally rather than streamed.

    @Test
    fun `pushFirmware sends begin then chunks then end`() = runTest {
        val t = FakeTransport().apply { autoAck = true }
        val client = SwcClient(t)
        val job = startClient(client)
        val image = ByteArray(1024) { it.toByte() }
        val result = client.pushFirmware(image)
        assertTrue("the push must succeed when every frame is acked", result is AckResult.Ok)

        val types = t.written.map { Regex("\"type\":\"(\\w+)\"").find(it)?.groupValues?.get(1) }
        assertEquals("ota_begin", types.first())
        assertEquals("ota_end", types.last())
        assertEquals("the image needs two 512-byte chunks", 2, types.count { it == "ota_chunk" })
        job.cancel()
    }

    @Test
    fun `pushFirmware offsets are decoded byte offsets not chunk indices`() = runTest {
        val t = FakeTransport().apply { autoAck = true }
        val client = SwcClient(t)
        val job = startClient(client)
        client.pushFirmware(ByteArray(1200) { 7 })
        val offsets = t.written.mapNotNull {
            Regex("\"type\":\"ota_chunk\".*?\"offset\":(\\d+)").find(it)?.groupValues?.get(1)?.toInt()
        }
        assertEquals(listOf(0, 512, 1024), offsets)
        job.cancel()
    }

    @Test
    fun `pushFirmware reports progress per acked chunk`() = runTest {
        val t = FakeTransport().apply { autoAck = true }
        val client = SwcClient(t)
        val job = startClient(client)
        val progress = mutableListOf<Pair<Int, Int>>()
        client.pushFirmware(ByteArray(1024) { 1 }, onProgress = { sent, total ->
            progress += sent to total
        })
        assertEquals(listOf(512 to 1024, 1024 to 1024), progress)
        job.cancel()
    }

    @Test
    fun `pushFirmware refuses an empty image without sending anything`() = runTest {
        val t = FakeTransport().apply { autoAck = true }
        val client = SwcClient(t)
        val job = startClient(client)
        val result = client.pushFirmware(ByteArray(0))
        assertTrue(result is AckResult.Nacked)
        assertEquals("empty_image", (result as AckResult.Nacked).err)
        assertTrue("nothing must reach the wire", t.written.isEmpty())
        job.cancel()
    }

    @Test
    fun `pushFirmware refuses an oversize image locally`() = runTest {
        val t = FakeTransport().apply { autoAck = true }
        val client = SwcClient(t)
        val job = startClient(client)
        // One byte past the app slot (1,920 KiB = 1,966,080, the firmware's
        // `kAppSlotBytes`). The device would refuse it at ota_begin; the app refuses
        // it before a single chunk, which is what this asserts.
        val result = client.pushFirmware(ByteArray(1_966_080 + 1))
        assertTrue(result is AckResult.Nacked)
        assertEquals("too_large", (result as AckResult.Nacked).err)
        assertTrue("nothing must reach the wire", t.written.isEmpty())
        job.cancel()
    }

    @Test
    fun `pushFirmware stops when the device nacks a chunk`() = runTest {
        // The device rejects a gap or an oversize run mid-transfer. The app must
        // not keep streaming chunks into a run the device already closed, and the
        // caller must see the nack rather than a timeout.
        val t = FakeTransport().apply { autoAck = true; nackType = "ota_chunk" }
        val client = SwcClient(t)
        val job = startClient(client)
        val result = client.pushFirmware(ByteArray(1024) { 3 })
        assertTrue("the nack must reach the caller, not become a timeout", result is AckResult.Nacked)
        assertEquals("gap", (result as AckResult.Nacked).err)
        // begin + exactly one chunk, then it stopped. No further chunk, no end.
        val types = t.written.map { Regex("\"type\":\"(\\w+)\"").find(it)!!.groupValues[1] }
        assertEquals(listOf("ota_begin", "ota_chunk"), types)
        job.cancel()
    }

    // --- spec §4.4 keepalive and liveness (open item N-27) ------------------

    /**
     * A clock the test moves by hand, in the SAME units the client's own
     * injection point uses.
     *
     * The client reads wall-clock milliseconds, so the fake advances real
     * milliseconds too: the mechanism is time-based and a fake that used a
     * different unit would test a translation nothing performs. Hand-advanced so
     * the test controls exactly when "5 s" and "10 s" pass, rather than racing a
     * real timer.
     */
    private class Clock {
        var ms: Long = 1_000_000L
        fun advance(delta: Long) { ms += delta }
    }

    @Test
    fun `the ping is emitted once the idle threshold passes`() = runTest {
        val t = FakeTransport()
        val clock = Clock()
        val client = SwcClient(t, nowMs = { clock.ms })
        val job = startClient(client)
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()

        // Just under the threshold: still silent, no ping.
        clock.advance(4_999)
        assertTrue("just under 5 s must not ping", client.SilenceTick() == false)
        assertEquals(0, t.written.size)

        // Over it: exactly one ping.
        clock.advance(2)
        assertTrue("over 5 s must ping", client.SilenceTick() == false)
        assertEquals(1, t.written.size)
        assertEquals("ping", Regex("\"type\":\"(\\w+)\"").find(t.written[0])!!.groupValues[1])

        // And NOT again on the very next tick -- the 5 s timer is stamped on the
        // attempt, so an unanswered ping does not become a flood.
        assertTrue(client.SilenceTick() == false)
        assertEquals("one ping per 5 s idle window", 1, t.written.size)
        job.cancel()
    }

    @Test
    fun `ten seconds of silence reports the link down rather than a healthy link`() = runTest {
        val t = FakeTransport()
        val clock = Clock()
        val client = SwcClient(t, nowMs = { clock.ms })
        val job = startClient(client)
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        assertEquals(LinkState.Connected, client.state.value)

        clock.advance(10_000)
        assertTrue("10 s of silence must be reported as down", client.SilenceTick())
        assertEquals(LinkState.SilenceExpired, client.state.value)
        job.cancel()
    }

    @Test
    fun `an expired link is not re-reported and no ping goes into it`() = runTest {
        // The watchdog's own liveness matters: a ping into a dead link is a write
        // nothing drains, and a state rewritten every tick is work with no effect.
        // `SilenceTick` therefore STAYS true once down (idempotent) without doing
        // anything further, and a frame is the only thing that revives the link.
        val t = FakeTransport()
        val clock = Clock()
        val client = SwcClient(t, nowMs = { clock.ms })
        val job = startClient(client)
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        clock.advance(10_000)
        assertTrue(client.SilenceTick())
        val stateAfter = client.state.value
        val pingsAfterExpiry = t.written.size

        clock.advance(10_000)
        assertTrue("an already-dead link keeps reporting down", client.SilenceTick())
        assertEquals("the state is not rewritten", stateAfter, client.state.value)
        assertEquals("no ping goes into a dead link", pingsAfterExpiry, t.written.size)
        job.cancel()
    }

    @Test
    fun `a frame after the silence expires revives the link`() = runTest {
        // Spec §4.4's recovery, and it must not need a reconnect: a device that was
        // unplugged and replugged without the app restarting answers again.
        val t = FakeTransport()
        val clock = Clock()
        val client = SwcClient(t, nowMs = { clock.ms })
        val job = startClient(client)
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        clock.advance(10_000)
        assertTrue(client.SilenceTick())
        assertEquals(LinkState.SilenceExpired, client.state.value)

        // A new frame: the peer is talking, so the link is healthy again.
        clock.advance(100)
        t.emit("{\"v\":1,\"seq\":2,\"type\":\"status\"}\n")
        advanceUntilIdle()
        assertEquals(LinkState.Connected, client.state.value)

        // And the silence clock was re-armed from that frame, not from the expiry:
        // the next 10 s is measured from the revival, so a link is not declared
        // dead again the moment it comes back.
        clock.advance(5_000)
        assertTrue("the clock restarted at the revival", client.SilenceTick() == false)
        job.cancel()
    }

    @Test
    fun `a client that has never heard a frame does not report the link down`() = runTest {
        // The bug the Long.MIN_VALUE sentinels exist to prevent: with a zero
        // initialiser, `now - last` is a huge positive number on the first tick and
        // a freshly constructed client declares the link down before any device has
        // been asked a question. That would paint "Not responding" over the startup
        // screen of an app whose cable is perfectly fine.
        val t = FakeTransport()
        val clock = Clock()
        val client = SwcClient(t, nowMs = { clock.ms })
        val job = startClient(client)
        // No frame at all. Move well past both thresholds.
        clock.advance(60_000)
        assertTrue("no peer has ever spoken: nothing to measure",
            client.SilenceTick() == false)
        assertEquals(LinkState.Disconnected, client.state.value)
        assertEquals("no ping to a device that never connected", 0, t.written.size)
        job.cancel()
    }

    @Test
    fun `a version mismatch is not overwritten by an expired silence`() = runTest {
        // Spec §4.5's mismatch is a property of the peer and must survive the
        // liveness watchdog; the watchdog must also not start pinging a peer the
        // app has agreed to stop talking to.
        val t = FakeTransport()
        val clock = Clock()
        val client = SwcClient(t, nowMs = { clock.ms })
        val job = startClient(client)
        t.emit("{\"v\":99,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        assertTrue(client.state.value is LinkState.VersionMismatch)
        clock.advance(30_000)
        assertTrue("the watchdog defers to a version mismatch",
            client.SilenceTick() == false)
        assertTrue("the mismatch must not become SilenceExpired",
            client.state.value is LinkState.VersionMismatch)
        assertEquals("no traffic on a mismatched link", 0, t.written.size)
        job.cancel()
    }

    @Test
    fun `a malformed line is not a liveness signal`() = runTest {
        // Only a DECODED frame counts as the peer talking (spec §4.4). Garbage on
        // the wire -- the torn write the framing contract tolerates -- must not keep
        // a dead link looking alive, or the watchdog can never fire.
        val t = FakeTransport()
        val clock = Clock()
        val client = SwcClient(t, nowMs = { clock.ms })
        val job = startClient(client)
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()
        clock.advance(6_000)
        t.emit("not a frame\n")
        advanceUntilIdle()
        clock.advance(5_000)
        assertTrue("garbage does not revive the clock; the link is down",
            client.SilenceTick())
        job.cancel()
    }

    @Test
    fun `DriveLiveness drives the loop and returns when the link expires`() = runTest {
        // The loop the activity runs. Driven on the test's VIRTUAL clock, so the
        // cadence is verified rather than asserted -- which is only possible because
        // the loop is a suspend function instead of a timer started in a
        // constructor. `nowMs` reads the scheduler's own clock here, so advancing
        // the scheduler advances the client's clock too and there is no second
        // timeline to keep in step.
        val t = FakeTransport()
        val client = SwcClient(t, nowMs = { testScheduler.currentTime })
        val reader = startClient(client)
        t.emit("{\"v\":1,\"seq\":1,\"type\":\"hello\"}\n")
        advanceUntilIdle()

        var returned = false
        val driver = launch { client.DriveLiveness(); returned = true }

        advanceTimeBy(4_000); runCurrent()
        assertTrue("no ping before the 5 s idle", t.written.isEmpty())
        assertFalse("the driver is still running", returned)

        advanceTimeBy(1_500); runCurrent()
        assertTrue("the driver pinged after 5 s idle", t.written.isNotEmpty())
        assertFalse(returned)

        advanceTimeBy(6_000); runCurrent()
        assertTrue("the driver returns once the link expires", returned)
        assertEquals(LinkState.SilenceExpired, client.state.value)
        driver.cancel(); reader.cancel()
    }
}
