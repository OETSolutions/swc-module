package com.oetsolutions.swc.link

import com.oetsolutions.swc.model.ConfigJson
import com.oetsolutions.swc.model.sampleConfig
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceUntilIdle
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
        val written = mutableListOf<String>()
        private val flow = MutableSharedFlow<ByteArray>(extraBufferCapacity = 64)
        override suspend fun write(bytes: ByteArray) {
            written += String(bytes)
        }
        override val incoming: Flow<ByteArray> = flow
        override fun close() {}
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
}
