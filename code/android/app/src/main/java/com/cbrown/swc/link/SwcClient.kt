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

    /** Replies awaiting a matching `for_seq`. */
    private val awaiting = HashMap<Int, CompletableDeferred<Frame>>()

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
            Frames.CONFIG_END -> {
                // The chunked run completed; the assembled JSON is what the device
                // has stored. Applied only on success, which is what keeps the local
                // model from claiming a config the device rejected.
                frame.fields["config"]?.jsonPrimitive?.content?.let { body ->
                    _config.value = ConfigJson.decode(body)
                }
            }
        }

        val forSeq = frame.fields["for_seq"]?.jsonPrimitive?.intOrNull
        if (forSeq != null) awaiting.remove(forSeq)?.complete(frame)

        _frames.tryEmit(frame)
    }

    private suspend fun send(type: String, body: (JsonObject) -> JsonObject = { it }) {
        val n = ++seq
        val base = JsonObject(
            mapOf(
                "v" to kotlinx.serialization.json.JsonPrimitive(PROTOCOL_VERSION),
                "seq" to kotlinx.serialization.json.JsonPrimitive(n),
                "type" to kotlinx.serialization.json.JsonPrimitive(type),
            )
        )
        val frame = body(base)
        writeLock.withLock {
            transport.write((frame.toString() + "\n").toByteArray())
        }
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

    /** Request the config and wait for the chunked run to fill [config]. */
    suspend fun getConfig(timeoutMs: Long = 15_000): Config {
        request(Frames.CONFIG_GET, timeoutMs)
        return _config.value
    }

    /** Send a frame of [type] and wait for the reply carrying its `for_seq`. */
    private suspend fun request(
        type: String,
        timeoutMs: Long = 15_000,
        body: (JsonObject) -> JsonObject = { it },
    ): AckResult {
        val n = seq + 1
        val deferred = CompletableDeferred<Frame>()
        awaiting[n] = deferred
        send(type, body)
        val reply = awaitOrNull(deferred, timeoutMs)
            ?: run {
                awaiting.remove(n)
                return AckResult.Timeout
            }
        return when (reply.type) {
            Frames.ACK -> AckResult.Ok(reply.fields["for_seq"]?.jsonPrimitive?.int ?: n)
            Frames.NACK -> AckResult.Nacked(
                reply.fields["err"]?.jsonPrimitive?.content ?: "",
                reply.fields["detail"]?.jsonPrimitive?.content ?: "",
            )
            else -> AckResult.Ok(n)
        }
    }

    private suspend fun awaitOrNull(
        d: CompletableDeferred<Frame>,
        timeoutMs: Long,
    ): Frame? = withTimeoutOrNullMillis(timeoutMs) { d.await() }

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

private fun base64(data: ByteArray): String =
    android.util.Base64.encodeToString(data, android.util.Base64.NO_WRAP)
