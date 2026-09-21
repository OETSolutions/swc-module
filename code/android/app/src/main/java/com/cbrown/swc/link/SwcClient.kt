package com.oetsolutions.swc.link

import com.oetsolutions.swc.contract.Frames
import com.oetsolutions.swc.contract.PROTOCOL_VERSION
import com.oetsolutions.swc.model.Config
import com.oetsolutions.swc.model.ConfigJson
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/** One decoded NDJSON frame. `fields` is the whole object, `v`/`seq`/`type` included. */
data class Frame(val type: String, val seq: Int, val version: Int, val fields: JsonObject)

/** The link's condition, as the user needs to see it. Spec 4.5: never silent. */
sealed interface LinkState {
    data object Disconnected : LinkState
    data object Connected : LinkState
    data class VersionMismatch(val firmware: Int, val app: Int) : LinkState
    data class Failed(val reason: String) : LinkState
}

sealed interface AckResult {
    data class Ok(val forSeq: Int) : AckResult
    data class Nacked(val err: String, val detail: String) : AckResult
    /** No reply arrived within the wait. Distinct from a nack: the device said nothing. */
    data object Timeout : AckResult
}

/**
 * The protocol client: frames in, frames out, and the state the UI renders.
 *
 * It owns no threading policy of its own -- [frames] is a hot flow that the caller
 * collects on whatever dispatcher it likes. That is deliberate: a client that
 * started its own coroutine scope would be a client that leaks one when the app
 * is backgrounded, and its tests would have to reason about two schedulers.
 */
class SwcClient(private val transport: SwcTransport) {

    private val _frames = MutableSharedFlow<Frame>(extraBufferCapacity = 256)
    val frames: Flow<Frame> = _frames.asSharedFlow()

    private val _state = MutableStateFlow<LinkState>(LinkState.Disconnected)
    val state: StateFlow<LinkState> = _state.asStateFlow()

    private val _config = MutableStateFlow(Config())
    val config: StateFlow<Config> = _config.asStateFlow()

    private val writeLock = Mutex()
    private var seq = 0

    // Frames arriving before the newline. Held as bytes because a UTF-8 sequence
    // can straddle a read boundary, and decoding each chunk independently would
    // corrupt a multi-byte character.
    private val pending = ArrayList<Byte>()
    private val lineCap = 1024   // kNdjsonMaxFrame, spec 4.2

    // Replies awaiting a matching `for_seq`. A CONCURRENT map, not a plain
    // `HashMap`: it is written from a UI coroutine (registering a waiter) and from
    // the reader coroutine (completing one) at the same time -- `Dispatchers.Default`
    // is multi-threaded -- so a plain map is a data race that can lose a waiter,
    // duplicate an entry, or corrupt the bucket chain.
    private val awaiting = java.util.concurrent.ConcurrentHashMap<Int, CompletableDeferred<Frame>>()

    // --- inbound chunked config run (spec 4.2) -----------------------------
    //
    // The firmware sends a config as `config_begin` (total_len, crc32), N x
    // `config_chunk` (offset, data_b64) and `config_end` (sha256) -- there is NO
    // `config` field carrying the whole thing. An earlier version of this class
    // looked for exactly that, so a real reply assembled nothing and the app would
    // have shown a stale config forever while every test passed, because the tests
    // only exercised the frame types the client did not need.
    //
    // `offset` is the byte offset of the chunk's first DECODED byte and each chunk
    // decodes independently, so placement is exact rather than append-order
    // dependent -- a retransmitted chunk lands in the right place.
    private var inbound: ByteArray? = null
    private var inboundLen = 0
    private var inboundCrc = 0L

    // Signals the END of an inbound config run to any caller awaiting it.
    //
    // `getConfig` cannot wait on `for_seq`. The firmware answers `config_get`
    // with the chunked run ITSELF, and no frame of that run
    // (`config_begin`/`config_chunk`/`config_end`) carries a `for_seq` -- they
    // ARE the reply, not a frame answering one. Waiting on a `for_seq` therefore
    // never matched and every `getConfig` burned its whole timeout before
    // returning, which is what `connect()` did on every launch.
    //
    // A SET, and a concurrent one, for the same reason `awaiting` is a
    // `ConcurrentHashMap`: calls overlap (a connect while the update screen
    // re-reads the config), they arrive from `Dispatchers.Default` coroutines
    // while the reader completes them, and a single slot would let the second
    // call overwrite the first's waiter -- so the first would time out on a link
    // that answered it. Every waiter is completed and the set swept, because one
    // run ends all of them.
    private val configRunWaiters =
        java.util.concurrent.CopyOnWriteArrayList<CompletableDeferred<Unit>>()

    private fun finishConfigRun() {
        for (w in configRunWaiters) w.complete(Unit)
        configRunWaiters.clear()
    }

    private fun beginInboundConfig(frame: Frame) {
        val total = frame.fields["total_len"]?.jsonPrimitive?.intOrNull ?: 0
        inboundCrc = frame.fields["crc32"]?.jsonPrimitive?.longOrNull ?: 0L
        // Bounded by the protocol's own maximum, not by the peer's claim: a
        // `total_len` from the wire is an allocation size chosen by the other end.
        inbound = if (total in 1..kWireConfigMaxBytes) ByteArray(total) else null
        inboundLen = 0
        if (inbound == null) {
            _state.value = LinkState.Failed("device offered a ${total}-byte config")
        }
    }

    private fun acceptInboundChunk(frame: Frame) {
        val buf = inbound ?: return
        val offset = frame.fields["offset"]?.jsonPrimitive?.intOrNull ?: return
        val b64 = frame.fields["data_b64"]?.jsonPrimitive?.content ?: return
        val bytes = try {
            base64Decode(b64)
        } catch (e: Exception) {
            _state.value = LinkState.Failed("chunk was not valid base64")
            inbound = null
            return
        }
        if (offset < 0 || offset + bytes.size > buf.size) {
            _state.value = LinkState.Failed("chunk ran past the declared length")
            inbound = null
            return
        }
        bytes.copyInto(buf, offset)
        inboundLen += bytes.size
    }

    private fun endInboundConfig(frame: Frame) {
        val buf = inbound
        inbound = null
        if (buf == null || inboundLen != buf.size) {
            _state.value = LinkState.Failed("config run ended early")
            finishConfigRun()
            return
        }
        // The run carries a crc32 at the start and a sha256 at the end. Both are
        // checked, because a truncated-then-completed transfer is exactly the case
        // that produces a config the device is NOT running while looking plausible.
        if (crc32(buf) != inboundCrc) {
            _state.value = LinkState.Failed("config failed its crc32 check")
            finishConfigRun()
            return
        }
        val want = frame.fields["sha256"]?.jsonPrimitive?.content ?: ""
        if (want.isNotEmpty() && sha256Hex(buf) != want) {
            _state.value = LinkState.Failed("config failed its sha256 check")
            finishConfigRun()
            return
        }
        // Applied only now, on a verified run. Adopting the bytes earlier would let
        // a torn transfer become what the app believes the device holds.
        _config.value = ConfigJson.decode(buf.toString(Charsets.UTF_8))
        // A run that FAILED its checks still ENDS: the waiter must be released so
        // `getConfig` returns the (unchanged) local model rather than hanging on a
        // run that already finished. All three exits above release it too.
        finishConfigRun()
    }

    /** Feed everything the transport delivers. Call this from a collector on [SwcTransport.incoming]. */
    suspend fun run(): Nothing {
        transport.incoming.collect { bytes ->
            for (b in bytes) {
                pending.add(b)
                if (b == NEWLINE) {
                    val line = pending.toByteArray()
                    pending.clear()
                    // Trim the newline; the wire contract delivers frames without it.
                    val text = String(line, 0, line.size - 1)
                    if (text.isNotEmpty()) handle(text)
                } else if (pending.size > lineCap) {
                    // The device never emits a longer line; a runaway here means the
                    // stream is not the protocol. Drop the partial rather than growing
                    // without bound, and say so instead of failing silently.
                    pending.clear()
                    _state.value = LinkState.Failed("input line exceeded $lineCap bytes")
                }
            }
        }
        error("transport.incoming completed; a USB link must not")
    }

    private suspend fun handle(text: String) {
        val obj = try {
            kotlinx.serialization.json.Json.parseToJsonElement(text).jsonObject
        } catch (e: Exception) {
            // A malformed frame from a peer is not fatal to the link: the next
            // newline resynchronizes. Reported as state, never thrown.
            _state.value = LinkState.Failed("malformed frame from device")
            return
        }
        val type = obj["type"]?.jsonPrimitive?.content ?: return
        val version = obj["v"]?.jsonPrimitive?.intOrNull ?: 0
        val fseq = obj["seq"]?.jsonPrimitive?.intOrNull ?: 0
        val frame = Frame(type, fseq, version, obj)

        // Spec 4.5: a protocol version this app does not implement must become an
        // explicit mismatch state, not a best-effort parse. A mismatch means the
        // two sides disagree about the frame vocabulary, so continuing invites a
        // corrupted config.
        if (version != PROTOCOL_VERSION) {
            _state.value = LinkState.VersionMismatch(firmware = version, app = PROTOCOL_VERSION)
            return
        }

        // Captured BEFORE the dispatch: a failure this frame is about to report (a
        // bad config digest, a short run) must STICK, and clearing it below would
        // erase the very error the frame was describing. Only a failure that
        // predates this frame -- i.e. an earlier malformed line -- is recovered.
        val failedBeforeThisFrame = _state.value is LinkState.Failed

        when (type) {
            Frames.HELLO -> {
                _state.value = LinkState.Connected
            }
            Frames.NACK -> {
                // A version_mismatch nack is the other way the device reports it.
                val err = frame.fields["err"]?.jsonPrimitive?.content ?: ""
                if (err == "version_mismatch") {
                    _state.value = LinkState.VersionMismatch(firmware = version, app = PROTOCOL_VERSION)
                }
            }
            Frames.CONFIG_BEGIN -> beginInboundConfig(frame)
            Frames.CONFIG_CHUNK -> acceptInboundChunk(frame)
            Frames.CONFIG_END -> endInboundConfig(frame)
        }

        // A well-formed frame means the peer is talking to us again, so a failure
        // from an EARLIER frame is cleared.
        //
        // Without this, a FAILED state was terminal until the next `hello`, which
        // arrives only on reconnect -- so ONE malformed line (a torn write during
        // enumeration, a dropped byte) froze the UI on "No device found" for the
        // rest of the session while the device kept answering frames. That is not
        // the "resynchronize at the next newline" the framing contract promises,
        // and it was reachable in normal use because the firmware's own `hello` was
        // malformed (see FwVersion.h): the app failed on the first frame and never
        // recovered.
        //
        // VersionMismatch is deliberately NOT cleared: the disagreement is a
        // property of the peer, not a transient, and spec 4.5 requires the app to
        // stop talking rather than carry on best-effort.
        if (failedBeforeThisFrame && _state.value is LinkState.Failed) {
            _state.value = LinkState.Connected
        }

        val forSeq = frame.fields["for_seq"]?.jsonPrimitive?.intOrNull
        if (forSeq != null) awaiting.remove(forSeq)?.complete(frame)

        _frames.tryEmit(frame)
    }

    private suspend fun send(type: String, body: (JsonObject) -> JsonObject = { it }) {
        writeLock.withLock { sendLocked(type, body) }
    }

    /**
     * Send a frame and register its reply waiter as ONE atomic step.
     *
     * **This is why [request] does not call [send].** Allocating the sequence
     * number and registering `awaiting[n]` in two separate critical sections lets
     * two overlapping callers read the same `seq` and both adopt the same `n`: one
     * waiter overwrites the other, whichever reply arrives completes only one of
     * them, and the loser waits out its full timeout on a link that answered
     * normally. Measured before the fix: two concurrent requests timed out 33 times
     * in 50 trials. The app issues requests from several coroutines on
     * `Dispatchers.Default` (a connect while a maintenance toggle is in flight, an
     * OTA run while the config is re-read), so the overlap is reachable in normal
     * use, not just in a test.
     *
     * Returns the allocated `seq`, so the caller can report the right `for_seq`
     * even on a timeout.
     */
    private suspend fun sendAndAwait(
        type: String,
        timeoutMs: Long,
        body: (JsonObject) -> JsonObject,
    ): Pair<Int, AckResult> {
        val deferred = CompletableDeferred<Frame>()
        val n = writeLock.withLock {
            val allocated = seq + 1
            awaiting[allocated] = deferred
            sendLocked(type, body)
            allocated
        }
        val reply = awaitOrNull(deferred, timeoutMs)
            ?: run {
                awaiting.remove(n)
                return n to AckResult.Timeout
            }
        val result = when (reply.type) {
            Frames.ACK -> AckResult.Ok(reply.fields["for_seq"]?.jsonPrimitive?.int ?: n)
            Frames.NACK -> AckResult.Nacked(
                reply.fields["err"]?.jsonPrimitive?.content ?: "",
                reply.fields["detail"]?.jsonPrimitive?.content ?: "",
            )
            else -> AckResult.Ok(n)
        }
        return n to result
    }

    /** The body of [send], already under [writeLock]. */
    private suspend fun sendLocked(type: String, body: (JsonObject) -> JsonObject) {
        val n = ++seq
        val base = JsonObject(
            mapOf(
                "v" to kotlinx.serialization.json.JsonPrimitive(PROTOCOL_VERSION),
                "seq" to kotlinx.serialization.json.JsonPrimitive(n),
                "type" to kotlinx.serialization.json.JsonPrimitive(type),
            )
        )
        val frame = body(base)
        transport.write((frame.toString() + "\n").toByteArray())
    }

    /**
     * Ask the device to identify itself. Spec 4.5: `hello` carries `protocol_v`,
     * which is what [state] keys the mismatch off.
     */
    suspend fun connect() {
        send(Frames.PING)
    }

    /**
     * Push a whole config.
     *
     * A real config is ~22 KB and the line cap is 1024 B, so this MUST go through
     * the chunked transport (spec 4.2) rather than a single `config_set`. The
     * offset is the byte offset of the chunk's first DECODED byte, so each chunk
     * decodes independently -- which is what makes a retry possible without
     * replaying the whole transfer.
     *
     * The local model is adopted ONLY on a non-nacked reply. Adopting it up front
     * would leave the app showing a config the device rejected, which is the
     * "config the device is not running" lie the nack exists to prevent.
     */
    suspend fun setConfig(c: Config, timeoutMs: Long = 15_000): AckResult {
        val problems = ConfigJson.problems(c)
        if (problems.isNotEmpty()) {
            // Refuse locally: the device would nack anyway, and a local refusal can
            // name the FIELD while the device's nack can only name a check.
            return AckResult.Nacked("invalid_config", problems.joinToString("; "))
        }

        val body = ConfigJson.encode(c).toByteArray()
        val crc = crc32(body)
        val sha = sha256Hex(body)

        var result = request(Frames.CONFIG_BEGIN) { o ->
            o.with("total_len", body.size).with("crc32", crc)
        }
        if (result !is AckResult.Ok) return result

        var offset = 0
        while (offset < body.size) {
            val end = minOf(offset + CHUNK_BYTES, body.size)
            val slice = body.copyOfRange(offset, end)
            val encoded = base64(slice)
            result = request(Frames.CONFIG_CHUNK) { o ->
                o.with("offset", offset).with("data_b64", encoded)
            }
            if (result !is AckResult.Ok) return result
            // The offset of the NEXT chunk is the size of what has been sent so
            // far, in DECODED bytes -- not a chunk index, and not the encoded
            // length. Getting this wrong makes the device assemble a config whose
            // bytes are correct in the wrong places.
            offset = end
        }

        result = request(Frames.CONFIG_END) { o -> o.with("sha256", sha) }
        if (result is AckResult.Ok) _config.value = c
        return result
    }

    /**
     * Request the config and wait for the chunked run to fill [config].
     *
     * The wait is on the RUN's end, not on a `for_seq`. The firmware answers
     * `config_get` with `config_begin`/`config_chunk`/`config_end` themselves
     * (spec §4.2), and none of those carries a `for_seq` -- so awaiting one never
     * matched and this always returned after its full timeout. Callers got the
     * right config (the handlers fill [config] as the run lands) but blocked for
     * 15 s doing it, which is exactly what `AppViewModel.connect()` does on launch.
     *
     * `config_get` is also not acked: §4.2's reply IS the run, so there is no
     * `ack` to wait for and `request()` would time out twice over.
     *
     * The waiter is registered BEFORE the frame goes out, because the device may
     * answer before `send` has even returned and the reader coroutine would
     * otherwise complete a run with nobody listening.
     *
     * A run that fails its digest leaves [config] unchanged and raises
     * `LinkState.Failed`; the caller sees the OLD model plus a failed link rather
     * than a silent hang.
     */
    suspend fun getConfig(timeoutMs: Long = 15_000): Config {
        val waiter = CompletableDeferred<Unit>()
        configRunWaiters.add(waiter)
        try {
            send(Frames.CONFIG_GET)
            awaitOrNull(waiter, timeoutMs)
        } finally {
            // Removed unconditionally: a later run must not complete THIS waiter,
            // and a timed-out call must not be left in the set holding memory.
            configRunWaiters.remove(waiter)
        }
        return _config.value
    }

    /**
     * Ask the device to enter maintenance mode (spec §8.2).
     *
     * Spec §8.2 lists four entry triggers and calls this one "**primary**, from the
     * Android app" — so the app is the intended way to get there, and it could not
     * do it. The frame type, the trigger enum and the link-problem message that
     * explains maintenance mode all existed; the sender did not, which is the same
     * shape as `ActionRunner` before it got a caller.
     *
     * Maintenance is what turns on WiFi and BLE, so the device can then be reached
     * at its provisioning page and can check for updates over its own connection.
     * Without this, a user whose car has no WiFi has no way in from the app.
     */
    suspend fun enterMaintenance(timeoutMs: Long = 5_000): AckResult =
        request(Frames.MAINTENANCE_ENTER, timeoutMs)

    /** Leave maintenance mode (spec §8.2). */
    suspend fun exitMaintenance(timeoutMs: Long = 5_000): AckResult =
        request(Frames.MAINTENANCE_EXIT, timeoutMs)

    /** Send a frame of [type] and wait for the reply carrying its `for_seq`. */
    private suspend fun request(
        type: String,
        timeoutMs: Long = 15_000,
        body: (JsonObject) -> JsonObject = { it },
    ): AckResult = sendAndAwait(type, timeoutMs, body).second

    // Generic in the awaited type: a request waits on a reply `Frame`, while
    // `getConfig` waits on the chunked run finishing, which carries no frame of
    // its own to hand back.
    private suspend fun <T> awaitOrNull(
        d: CompletableDeferred<T>,
        timeoutMs: Long,
    ): T? = withTimeoutOrNullMillis(timeoutMs) { d.await() }

    private suspend fun <T> withTimeoutOrNullMillis(ms: Long, block: suspend () -> T): T? =
        try {
            kotlinx.coroutines.withTimeout(ms) { block() }
        } catch (e: kotlinx.coroutines.TimeoutCancellationException) {
            null
        }

    fun close() = transport.close()

    // ------------------------------------------------------------- helpers

    private companion object {
        const val NEWLINE: Byte = '\n'.code.toByte()

        /**
         * 512, matching the firmware's `kConfigWireChunkBytes` (spec 4.2): 512
         * decoded bytes become 684 base64 characters, which fits the 1024-byte
         * NDJSON line cap with the envelope around it. A larger chunk would
         * silently exceed the cap and be dropped by the device's reader.
         */
        const val CHUNK_BYTES = 512

        /**
         * The largest config the wire can carry, from `ConfigMaxSerializedSize()`
         * (22,407 B in the firmware, spec 3.5). Used to bound the inbound buffer so
         * a peer's `total_len` cannot choose an allocation size.
         */
        const val kWireConfigMaxBytes = 22_407
    }
}

/** Add a string field to a frame body. */
private fun JsonObject.with(key: String, value: String): JsonObject =
    JsonObject(this + (key to kotlinx.serialization.json.JsonPrimitive(value)))

/** Add a numeric field to a frame body. */
private fun JsonObject.with(key: String, value: Int): JsonObject =
    JsonObject(this + (key to kotlinx.serialization.json.JsonPrimitive(value)))

/**
 * Add a field to a frame body.
 *
 * A separate `Long` overload because `crc32` is a uint32 and would not fit an
 * `Int` without going negative. Sent as an unsigned number so the firmware's
 * `ReadU32` -- which REJECTS a negative -- accepts it.
 */
private fun JsonObject.with(key: String, value: Long): JsonObject = JsonObject(
    this + (key to kotlinx.serialization.json.JsonUnquotedLiteral(value.toString()))
)

/**
 * CRC-32, the same polynomial the firmware's `ConfigCrc32` uses.
 *
 * Spec 4.2 puts a crc32 in `config_begin` so a corrupted transfer is caught
 * BEFORE the whole config has been sent, rather than by the sha256 at the end.
 * It is not a substitute for the hash -- it is what makes the early failure
 * cheap.
 */
private fun crc32(data: ByteArray): Long {
    var crc = 0xFFFFFFFFL
    for (b in data) {
        crc = crc xor (b.toLong() and 0xFF)
        for (i in 0 until 8) {
            crc = if (crc and 1L != 0L) (crc ushr 1) xor 0xEDB88320L else crc ushr 1
        }
    }
    return (crc xor 0xFFFFFFFFL) and 0xFFFFFFFFL
}

/** Lowercase hex SHA-256, matching the firmware's in-tree implementation. */
private fun sha256Hex(data: ByteArray): String {
    val md = java.security.MessageDigest.getInstance("SHA-256")
    return md.digest(data).joinToString("") { "%02x".format(it) }
}

/*
 * `java.util.Base64`, NOT `android.util.Base64`.
 *
 * The Android one is a stub that returns null in a plain JVM unit test, so using
 * it would make this class need Robolectric to test -- while its whole design
 * claim is that the transport seam makes it testable on the JVM with no Android
 * at all. It failed exactly that way: every inbound-config test saw a null decode.
 *
 * `java.util.Base64` needs API 26, which is this app's minSdk, so nothing is
 * given up. It also emits standard, unwrapped base64, which is what the
 * firmware's decoder expects.
 */

/** Encode a chunk for `config_chunk` / `ota_chunk`. */
private fun base64(data: ByteArray): String =
    java.util.Base64.getEncoder().encodeToString(data)

/** Decode the firmware's base64. Throws on malformed input, which the caller reports. */
private fun base64Decode(s: String): ByteArray =
    java.util.Base64.getDecoder().decode(s)
