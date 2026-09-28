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
                    // The per-press click is OFF by default, matching the firmware
                    // ("normal switch operation should not cause a beep"). Absent
                    // means false: a config from an older firmware has no such key.
                    keyClickEnabled = s.bool("key_click_enabled") ?: false,
                    maintenanceTimeoutMs = s.long("maintenance_timeout_ms") ?: 300_000L,
                    // Absent means false, matching the firmware's optional-field
                    // rule: a config from an older firmware has no such key, and
                    // treating that as anything but the default would clear the
                    // user's setting on the next save.
                    maintenanceOnBoot = s.bool("maintenance_on_boot") ?: false,
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
        put("key_click_enabled", s.keyClickEnabled)
        put("maintenance_timeout_ms", s.maintenanceTimeoutMs)
        put("maintenance_on_boot", s.maintenanceOnBoot)
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
        // The width too: `ReadStr` refuses a value `>= kDeviceIdLen`, so an
        // over-width id encodes fine and then fails to DECODE -- a round-trip
        // violation surfacing as "corrupt config". The app never edits this field,
        // so it is latent today, but the rule belongs with the other width checks.
        if (c.deviceId.length >= K_DEVICE_ID_LEN)
            out += "device_id must be under $K_DEVICE_ID_LEN chars"
        // At least one channel, and not more than the max -- the firmware refuses
        // `channel_count == 0` (`ConfigValidate`), and an app that only bounded
        // the upper end would send a config with no channels that the device then
        // rejects at decode, losing the whole save.
        if (c.channels.isEmpty()) out += "at least 1 channel"
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
        // Bounded at the top end too (kSendDurationMaxMs): this is how long the KEY
        // line is DRIVEN, so a value near the uint32 maximum pins a phantom press
        // the user cannot release (FR-39). The firmware refuses it in RANGE; without
        // the matching check here the app passes it and the device nacks the whole
        // save with the offending field unnamed.
        if (t.sendDurationMs > K_SEND_DURATION_MAX_MS)
            out += "send_duration_ms must be at most $K_SEND_DURATION_MAX_MS"
        // Feedback levels are 0..3 (the firmware refuses > 3), and the maintenance
        // window is bounded on BOTH ends. None of these had a check, so a config
        // carrying, say, `maintenance_timeout_ms = 4_000_000` passed the app's
        // local gate and was then nacked by the device with the field unnamed.
        if (c.settings.buzzerLevel !in 0..3) out += "buzzer_level must be 0 to 3"
        if (c.settings.ledLevel !in 0..3) out += "led_level must be 0 to 3"
        if (c.settings.maintenanceTimeoutMs <= 0)
            out += "maintenance_timeout_ms must be greater than zero"
        if (c.settings.maintenanceTimeoutMs > K_MAINTENANCE_TIMEOUT_MAX_MS)
            out += "maintenance_timeout_ms must be at most $K_MAINTENANCE_TIMEOUT_MAX_MS"

        c.bindings.forEach { b ->
            // The firmware copies into a fixed char[16]; a longer value is
            // refused by validation rather than truncated (spec 3.5), so the
            // check belongs here where the user can see it.
            if (b.id.length >= K_BINDING_ID_LEN)
                out += "binding '${b.id}': id must be under $K_BINDING_ID_LEN chars"
            // AUX1 carries the programming (1.5 s) and maintenance (3 s) holds
            // (spec 7.5/8.2), so the firmware REFUSES a binding on it
            // (`ConfigValidate`). Refusing here keeps the app's gate at parity:
            // otherwise the edit reaches the device and the whole save is nacked
            // with the offending binding unnamed. AUX2/AUX3 are bindable.
            if (b.channel == BindingChannel.AUX1)
                out += "binding '${b.id}': AUX1 is the programming button and cannot be bound " +
                    "(use AUX2 or AUX3)"
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
            // Mirror `ConfigValidate`'s per-channel checks, which refused a name
            // `ReadStr` cannot decode and (in `LadderProfileIsValid`) bounded the
            // whole learned profile. These four rules had NO app-side check (open
            // item N-40), so a config the app accepted carried a profile the device
            // refused -- the very failure the local gate exists to prevent,
            // reported as a nack naming a check rather than the field.
            if (ch.name.isEmpty())
                out += "channel $i: name must not be empty"
            if (ch.name.length >= K_CHANNEL_NAME_LEN)
                out += "channel $i: name must be under $K_CHANNEL_NAME_LEN chars"

            val p = ch.ladder
            if (p.learnedIdleMv <= 0 || p.learnedIdleMv > K_ADC_CEILING_MV)
                out += "channel $i: learned_idle_mv must be between 1 and $K_ADC_CEILING_MV"
            if (p.buttons.size > K_LADDER_MAX_BUTTONS)
                out += "channel $i: at most $K_LADDER_MAX_BUTTONS buttons"
            p.buttons.forEach { btn ->
                if (btn.id.isEmpty())
                    out += "channel $i button '${btn.id}': id must not be empty"
                if (btn.id.length >= K_LADDER_ID_LEN)
                    out += "channel $i button '${btn.id}': id must be under $K_LADDER_ID_LEN chars"
                // A centre at or above the idle reference is physically impossible
                // (a press pulls the input DOWN), and 0 is unreachable from a learn
                // (an unreadable ADC reports 0). Both bounds are against the ADC
                // ceiling, not the rail -- `LadderProfileIsValid`.
                if (btn.mvCenter == 0 || btn.mvCenter > K_ADC_CEILING_MV)
                    out += "channel $i button '${btn.id}': mv_center must be between 1 and " +
                        "$K_ADC_CEILING_MV"
                if (btn.mvTolerance == 0)
                    out += "channel $i button '${btn.id}': mv_tolerance must not be zero"
                // The DERIVED window must be a real one: a tolerance that rounds to
                // zero permille can never match anything. The ratio is computed here
                // exactly as `LadderRatioPermille` does it, so the two cannot
                // disagree on which tolerances round to zero.
                if (p.learnedIdleMv > 0 &&
                    ladderRatioPermille(btn.mvTolerance, p.learnedIdleMv) <= 0
                ) {
                    out += "channel $i button '${btn.id}': mv_tolerance is too small against " +
                        "learned_idle_mv to form a window"
                }
            }
            // Overlapping windows: the device's `LadderWindowsAreDistinguishable`
            // refuses them, so the app must too. Only meaningful with a reference
            // and at least two buttons -- both are checked above.
            if (p.learnedIdleMv > 0 && p.buttons.size > 1 &&
                !ladderWindowsDistinguishable(p)
            ) {
                out += "channel $i: two buttons' windows overlap, so a press could " +
                    "match both"
            }
        }

        // Mirror `BindingNamesARealInput`: a binding that names an id on no ladder
        // and no AUX input is a binding to nothing, which the device refuses. "NONE"
        // is the programming button, a legal target for a gesture.
        val knownIds = buildSet {
            c.channels.forEach { ch -> ch.ladder.buttons.forEach { add(it.id) } }
            c.aux.forEach { add(it.id) }
            add("NONE")
        }
        c.bindings.forEach { b ->
            if (b.button !in knownIds)
                out += "binding '${b.id}': button '${b.button}' is not a button on any " +
                    "channel or AUX input"
        }
        return out
    }
}

/**
 * Ratio in permille, byte-for-byte the same arithmetic as the firmware's
 * `LadderRatioPermille` (`LadderDecode.cpp`): rounded division, `Long` to avoid any
 * intermediate overflow, and the same `-1` for a non-positive idle.
 *
 * Duplicated rather than shared because there is no cross-language codegen for
 * this predicate (the contract generator covers frames and enums, not arithmetic),
 * and the two must round IDENTICALLY -- a tolerance that one side rounds to zero
 * permille and the other to one is a profile one accepts and the other refuses.
 * `N-40`'s guard test pins the agreement.
 */
internal fun ladderRatioPermille(levelMv: Int, idleMv: Int): Int {
    if (idleMv <= 0) return -1
    val scaled = (levelMv.toLong() * 1000L + idleMv / 2) / idleMv
    if (scaled > 32767L) return 32767
    if (scaled < -32768L) return -32768
    return scaled.toInt()
}

/**
 * Whether every pair of windows is separated by more than the wider of the two
 * tolerances, mirroring `LadderWindowsAreDistinguishable` (`LadderDecode.cpp`).
 *
 * The relation is what `LadderClassify` relies on: within a window's half-width of
 * a centre, the nearest centre is unambiguous. Two windows closer than that mean a
 * press could fall in both, and the device refuses the profile rather than firing
 * whichever it happens to test first.
 */
internal fun ladderWindowsDistinguishable(p: com.oetsolutions.swc.model.LadderProfile): Boolean {
    if (p.learnedIdleMv == 0) return false
    val n = minOf(p.buttons.size, K_LADDER_MAX_BUTTONS)
    for (i in 0 until n) {
        for (j in i + 1 until n) {
            val bi = p.buttons[i]
            val bj = p.buttons[j]
            val ci = ladderRatioPermille(bi.mvCenter, p.learnedIdleMv)
            val cj = ladderRatioPermille(bj.mvCenter, p.learnedIdleMv)
            val ti = ladderRatioPermille(bi.mvTolerance, p.learnedIdleMv)
            val tj = ladderRatioPermille(bj.mvTolerance, p.learnedIdleMv)
            if (ci < 0 || cj < 0 || ti < 0 || tj < 0) return false
            val distance = kotlin.math.abs(ci - cj)
            val tolerance = maxOf(ti, tj)
            if (distance <= tolerance) return false
        }
    }
    return true
}
