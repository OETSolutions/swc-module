package com.oetsolutions.swc.model

import com.oetsolutions.swc.contract.ActionKind
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/**
 * The config JSON codec, mirroring `lib/Config/ConfigCodec.cpp`.
 *
 * **This is hand-written against the firmware's encoder, not `@Serializable`.**
 * Three of the firmware's wire conventions cannot be expressed by a serialization
 * annotation, and each one would silently produce a config the device refuses:
 *
 *  1. **Per-kind parameter names.** An action's string slots are called `package`,
 *     `action`/`data`, `keycode`, `command`, `target` or `pattern` depending on
 *     its kind (spec 3.6), and a slot the kind does not use is OMITTED. A
 *     `@SerialName` is fixed per property, so it cannot do this.
 *  2. **Temperature and confidence are scaled on the wire.** `temp_c_at_learn`
 *     crosses as a decimal (23.5) while the model holds tenths (235), and
 *     `confidence` crosses as 0.0–1.0 while the model holds 0–100. See
 *     `AddTenths`/`AddConfidence`.
 *  3. **The wire is compact and byte-exact.** The device CRCs exactly these bytes
 *     and FR-27 requires a byte-identical round trip, so the output must be
 *     whitespace-free.
 *
 * Every field name below is quoted from the encoder with the function that writes
 * it noted, because a name invented here is a config the device cannot read --
 * and the failure surfaces as "corrupt config", which names nothing.
 */
object ConfigJson {

    private val json = Json {
        // The device emits compact JSON (`cJSON_PrintUnformatted`) and CRCs those
        // exact bytes. prettyPrint would still DECODE fine, so a round-trip test
        // would pass while the bytes differed -- which is why this is a
        // correctness setting here, not a formatting preference.
        prettyPrint = false
        isLenient = false
        ignoreUnknownKeys = true
        encodeDefaults = true
    }

    // ---------------------------------------------------------------- decode

    private fun JsonObject.str(key: String): String? =
        (this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content

    private fun JsonObject.int(key: String): Int? =
        (this[key] as? JsonPrimitive)?.intOrNull

    private fun JsonObject.long(key: String): Long? =
        (this[key] as? JsonPrimitive)?.longOrNull

    private fun JsonObject.bool(key: String): Boolean? =
        (this[key] as? JsonPrimitive)?.booleanOrNull

    private fun JsonObject.obj(key: String): JsonObject? = this[key] as? JsonObject
    private fun JsonObject.arr(key: String): JsonArray? = this[key] as? JsonArray

    /** The tenths-of-a-degree integer behind a wire decimal. Round half up, as C does. */
    private fun tenthsOf(e: JsonElement?): Int {
        val d = (e as? JsonPrimitive)?.doubleOrNull ?: return 0
        return Math.round(d * 10.0).toInt()
    }

    private fun percentOf(e: JsonElement?): Int {
        val d = (e as? JsonPrimitive)?.doubleOrNull ?: return 0
        return Math.round(d * 100.0).toInt()
    }

    private fun decodeAction(o: JsonObject): Action {
        // A kind this app does not know decodes to a sentinel rather than
        // throwing: a config written by a newer firmware must be openable (spec
        // 4.5 -- show the mismatch, do not crash). ActionKind.NONE is the
        // sentinel because it is the one kind with no parameters to misread.
        val kind = o.str("kind")?.let { ActionKind.fromWireName(it) } ?: ActionKind.NONE
        val paramKey = kind.paramKey
        val payloadKey = kind.payloadKey
        return Action(
            kind = kind,
            target = paramKey?.let { o.str(it) } ?: "",
            payload = payloadKey?.let { o.str(it) } ?: "",
            keyMv = o.int("key_mv") ?: 0,
        )
    }

    private fun decodeButton(o: JsonObject) = LadderButton(
        id = o.str("id") ?: "",
        name = o.str("name") ?: "",
        mvCenter = o.int("mv_center") ?: 0,
        mvTolerance = o.int("mv_tolerance") ?: 0,
        learnedAtRailMv = o.int("learned_at_rail_mv") ?: 3300,
        tempCAtLearn = tenthsOf(o["temp_c_at_learn"]),
        sampleCount = o.int("sample_count") ?: 0,
        confidence = percentOf(o["confidence"]),
    )

    private fun decodeChannel(o: JsonObject): ChannelConfig {
        val ladderObj = o.obj("ladder")
        val outputObj = o.obj("output")
        return ChannelConfig(
            // Encoder writes "name" (EncodeChannels); spec 3.7's example calls it
            // "id". Read BOTH so a config from either source loads -- the encoder
            // is authoritative for what we WRITE.
            enabled = o.bool("enabled") ?: true,
            name = o.str("name") ?: o.str("id") ?: "",
            ladder = LadderProfile(
                source = ladderObj?.int("source") ?: 0,
                // Wire key is `idle_mv`; the firmware's struct field is
                // `learned_idle_mv` (see EncodeChannels' note on why they differ).
                learnedIdleMv = ladderObj?.int("idle_mv") ?: 0,
                buttons = ladderObj?.arr("buttons")
                    ?.mapNotNull { (it as? JsonObject)?.let(::decodeButton) }
                    ?: emptyList(),
            ),
            output = OutputProfile(
                // `AUTO` is a member (see GainMode), so every name the firmware can
                // emit is known here. An unknown name cannot arrive from a real
                // device: the firmware's own decoder REFUSES one
                // (`VALUE_OF(kGainModeNames, ...) < 0` -> `return false`,
                // ConfigCodec.cpp), so a device that spoke a fourth mode would fail
                // the protocol-version gate first and never reach this line. The
                // `?: TRACKING` is therefore a defensive default for a hand-written
                // config, NOT a mapping of device data -- and it is deliberately not
                // a silent coercion of a real mode, because there is no real mode it
                // could be. (An earlier comment here claimed unknown names were
                // "recorded so a caller can refuse to round-trip"; no such mechanism
                // exists, and this is the honest statement of why none is needed.)
                gainMode = outputObj?.str("gain_mode")
                    ?.let { n -> GainMode.entries.firstOrNull { it.wireName == n } }
                    ?: GainMode.TRACKING,
                idleDacCode = outputObj?.int("idle_dac_code") ?: 4095,
            ),
        )
    }

    fun decode(text: String): Config {
        val root = json.parseToJsonElement(text).jsonObject
        return Config(
            schemaVersion = root.int("schema_version") ?: K_CONFIG_SCHEMA_VERSION,
            deviceId = root.str("device_id") ?: "",
            updatedAtMs = root.long("updated_at_ms") ?: 0L,
            settings = root.obj("settings")?.let { s ->
                DeviceSettings(
                    timings = GestureTimings(
                        debounceMs = s.int("debounce_ms") ?: 25,
                        doublePressOffMs = s.int("double_press_off_ms") ?: 500,
                        longPressMs = s.int("long_press_ms") ?: 750,
                        sendDurationMs = s.int("send_duration_ms") ?: 200,
                    ),
                    gainPolicy = s.str("gain_policy")
                        ?.let { n -> GainPolicy.entries.firstOrNull { it.wireName == n } }
                        ?: GainPolicy.AUTO,
                    buzzerLevel = s.int("buzzer_level") ?: 2,
                    ledLevel = s.int("led_level") ?: 2,
                    tempCompEnabled = s.bool("temp_comp_enabled") ?: true,
                    maintenanceTimeoutMs = s.long("maintenance_timeout_ms") ?: 300_000L,
                )
            } ?: DeviceSettings(),
            channels = root.arr("channels")
                ?.mapNotNull { (it as? JsonObject)?.let(::decodeChannel) }
                ?: listOf(ChannelConfig()),
            aux = root.arr("aux")?.mapNotNull { e ->
                (e as? JsonObject)?.let {
                    AuxButtonConfig(
                        id = it.str("id") ?: "",
                        source = it.int("source") ?: 0,
                        mvCenter = it.int("mv_center") ?: 0,
                        mvTolerance = it.int("mv_tolerance") ?: 0,
                    )
                }
            } ?: emptyList(),
            bindings = root.arr("bindings")?.mapNotNull { e ->
                (e as? JsonObject)?.let { b ->
                    Binding(
                        id = b.str("id") ?: "",
                        channel = b.str("channel")
                            ?.let { n -> BindingChannel.fromWireName(n) }
                            ?: BindingChannel.SWC1,
                        button = b.str("button") ?: "",
                        gesture = b.str("gesture")
                            ?.let { n -> Gesture.fromWireName(n) }
                            ?: Gesture.NONE,
                        enabled = b.bool("enabled") ?: true,
                        actions = b.arr("actions")
                            ?.mapNotNull { a -> (a as? JsonObject)?.let(::decodeAction) }
                            ?: emptyList(),
                    )
                }
            } ?: emptyList(),
        )
    }

    // ---------------------------------------------------------------- encode

    private fun actionJson(a: Action): JsonObject = buildJsonObject {
        put("kind", a.kind.wireName)
        // Omitted when empty, never written blank -- a NONE action is exactly
        // {"kind":"NONE"} on the wire (EncodeActions).
        val paramKey = a.kind.paramKey
        val payloadKey = a.kind.payloadKey
        if (paramKey != null && a.target.isNotEmpty()) put(paramKey, a.target)
        if (payloadKey != null && a.payload.isNotEmpty()) put(payloadKey, a.payload)
        if (a.keyMv != 0) put("key_mv", a.keyMv)
    }

    private fun buttonJson(b: LadderButton): JsonObject = buildJsonObject {
        put("id", b.id)
        put("name", b.name)
        put("mv_center", b.mvCenter)
        put("mv_tolerance", b.mvTolerance)
        put("learned_at_rail_mv", b.learnedAtRailMv)
        // Decimal on the wire, tenths in the model: the wire form is what a human
        // reads, the integer is what makes the round trip exact (AddTenths).
        put("temp_c_at_learn", b.tempCAtLearn / 10.0)
        put("sample_count", b.sampleCount)
        put("confidence", b.confidence / 100.0)
    }

    private fun channelJson(c: ChannelConfig): JsonObject = buildJsonObject {
        put("name", c.name)
        put("enabled", c.enabled)
        put("ladder", buildJsonObject {
            put("source", c.ladder.source)
            put("idle_mv", c.ladder.learnedIdleMv)
            put("buttons", buildJsonArray {
                c.ladder.buttons.forEach { add(buttonJson(it)) }
            })
        })
        put("output", buildJsonObject {
            put("gain_mode", c.output.gainMode.wireName)
            put("idle_dac_code", c.output.idleDacCode)
        })
    }

    private fun settingsJson(s: DeviceSettings): JsonObject = buildJsonObject {
        put("debounce_ms", s.timings.debounceMs)
        put("double_press_off_ms", s.timings.doublePressOffMs)
        put("long_press_ms", s.timings.longPressMs)
        put("send_duration_ms", s.timings.sendDurationMs)
        put("gain_policy", s.gainPolicy.wireName)
        put("buzzer_level", s.buzzerLevel)
        put("led_level", s.ledLevel)
        put("temp_comp_enabled", s.tempCompEnabled)
        put("maintenance_timeout_ms", s.maintenanceTimeoutMs)
    }

    fun encode(c: Config): String = buildJsonObject {
        put("schema_version", c.schemaVersion)
        put("device_id", c.deviceId)
        put("updated_at_ms", c.updatedAtMs)
        put("settings", settingsJson(c.settings))
        put("aux", buildJsonArray {
            c.aux.forEach { a ->
                addJsonObject {
                    put("id", a.id)
                    put("source", a.source)
                    put("mv_center", a.mvCenter)
                    put("mv_tolerance", a.mvTolerance)
                }
            }
        })
        put("channels", buildJsonArray { c.channels.forEach { add(channelJson(it)) } })
        put("bindings", buildJsonArray {
            c.bindings.forEach { b ->
                addJsonObject {
                    put("id", b.id)
                    put("channel", b.channel.wireName)
                    put("button", b.button)
                    put("gesture", b.gesture.wireName)
                    put("enabled", b.enabled)
                    // Added even when empty: an empty list is the legal "swallow
                    // this gesture" state, distinct from a missing field.
                    put("actions", buildJsonArray { b.actions.forEach { add(actionJson(it)) } })
                }
            }
        })
    }.toString()

    // --------------------------------------------------------------- validate

    /**
     * The problems that would make the device REFUSE this config, or accept it and
     * store something other than what was sent.
     *
     * Mirrors `ConfigValidate` (ConfigCodec.cpp) plus the fixed field widths from
     * ConfigModel.h. It exists so the UI can refuse before the user taps Save
     * rather than surfacing a nack afterwards.
     */
    fun problems(c: Config): List<String> {
        val out = mutableListOf<String>()
        if (c.deviceId.isEmpty()) out += "device_id must not be empty"
        if (c.channels.size > K_MAX_CHANNELS) out += "at most $K_MAX_CHANNELS channels"
        if (c.bindings.size > K_MAX_BINDINGS) out += "at most $K_MAX_BINDINGS bindings"
        if (c.aux.size > K_MAX_AUX_BUTTONS) out += "at most $K_MAX_AUX_BUTTONS aux buttons"
        val t = c.settings.timings
        if (t.debounceMs == 0) out += "debounce_ms must be greater than zero"
        if (t.doublePressOffMs < t.debounceMs)
            out += "double_press_off_ms must not be below debounce_ms"
        if (t.longPressMs <= t.doublePressOffMs)
            out += "long_press_ms must be above the double-press window"

        if (t.sendDurationMs <= 0) out += "send_duration_ms must be greater than zero"

        c.bindings.forEach { b ->
            // The firmware copies into a fixed char[16]; a longer value is
            // refused by validation rather than truncated (spec 3.5), so the
            // check belongs here where the user can see it.
            if (b.id.length >= 16) out += "binding '${b.id}': id must be under 16 chars"
            if (b.actions.size > K_MAX_ACTIONS_PER_BINDING)
                out += "binding '${b.id}': at most $K_MAX_ACTIONS_PER_BINDING actions"
            b.actions.forEach { a ->
                // ActionTakesPayload's set, via the generated contract -- so the
                // rule is not restated here and cannot drift from the firmware.
                if (a.kind.needsParam && a.target.isEmpty())
                    out += "binding '${b.id}': ${a.kind.wireName} needs a ${a.kind.paramKey}"
                if (a.target.length >= K_ACTION_TARGET_LEN)
                    out += "binding '${b.id}': ${a.kind.wireName} target must be under " +
                        "$K_ACTION_TARGET_LEN chars"
                if (a.payload.length >= K_DATA_PAYLOAD_LEN)
                    out += "binding '${b.id}': ${a.kind.wireName} payload must be under " +
                        "$K_DATA_PAYLOAD_LEN chars"
                // key_mv is a VOLTAGE and 0 means "absent". The output floor is
                // 1800 mV, so a zero level is unreachable rather than a value.
                if (a.kind == ActionKind.OUT_VOLTAGE && a.keyMv == 0)
                    out += "binding '${b.id}': OUT_VOLTAGE needs a key_mv"
                if (a.keyMv != 0 && (a.keyMv < 0 || a.keyMv > 65535))
                    out += "binding '${b.id}': key_mv is out of range"
            }
        }
        c.channels.forEachIndexed { i, ch ->
            ch.ladder.buttons.forEach { btn ->
                if (btn.id.length >= 16)
                    out += "channel $i button '${btn.id}': id must be under 16 chars"
                if (btn.mvTolerance == 0)
                    out += "channel $i button '${btn.id}': mv_tolerance must not be zero"
            }
        }
        return out
    }
}
