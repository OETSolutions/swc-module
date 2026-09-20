package com.oetsolutions.swc.model

import com.oetsolutions.swc.contract.ActionKind
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * The Kotlin mirror of the firmware's `Config` (lib/Config/ConfigModel.h).
 *
 * **The widths here are the firmware's, and they are a budget, not a preference.**
 * `Action.target` is 40 chars and `Action.payload` 48 because the firmware's NVS
 * worst case is 22,407 B and the partition holds 48,384 B — see ConfigModel.h's
 * note. The app must not accept a string the firmware cannot store, or the config
 * fails to save on the device after the user has already tapped Save. The limits
 * are enforced in [ConfigJson.encode], not merely documented.
 */

const val K_MAX_CHANNELS = 2
const val K_MAX_BINDINGS = 32
const val K_MAX_ACTIONS_PER_BINDING = 2
const val K_MAX_AUX_BUTTONS = 3
const val K_ACTION_TARGET_LEN = 40
const val K_DATA_PAYLOAD_LEN = 48
const val K_CONFIG_SCHEMA_VERSION = 1

/** Spec 3.5: `SWC1 | SWC2 | AUX1 | AUX2 | AUX3 | ANY`. `ANY` is a real value. */
@Serializable
enum class BindingChannel(val wireName: String) {
    @SerialName("SWC1") SWC1("SWC1"),
    @SerialName("SWC2") SWC2("SWC2"),
    @SerialName("AUX1") AUX1("AUX1"),
    @SerialName("AUX2") AUX2("AUX2"),
    @SerialName("AUX3") AUX3("AUX3"),
    @SerialName("ANY") ANY("ANY");

    companion object {
        fun fromWireName(name: String): BindingChannel? =
            entries.firstOrNull { it.wireName == name }
    }
}

@Serializable
enum class Gesture(val wireName: String) {
    @SerialName("NONE") NONE("NONE"),
    @SerialName("SINGLE") SINGLE("SINGLE"),
    @SerialName("DOUBLE") DOUBLE("DOUBLE"),
    @SerialName("LONG") LONG("LONG");

    companion object {
        fun fromWireName(name: String): Gesture? = entries.firstOrNull { it.wireName == name }
    }
}

/**
 * A channel's gain, or [AUTO] to defer to `settings.gain_policy` (spec 6.2).
 *
 * **`AUTO` must be here even though it is not a gain.** The firmware's decoder
 * accepts a channel `gain_mode` of "AUTO" — the spec's own worked example uses it
 * — and this enum did not, so a config the device considers legal came back to the
 * app as a name nothing matched. The decoder's `firstOrNull` then fell back to
 * TRACKING and the next save wrote a CONCRETE gain the user never chose, silently
 * overriding their policy. An unknown enum member is a data-loss bug in a
 * round-trip codec, not a cosmetic gap.
 */
@Serializable
enum class GainMode(val wireName: String) {
    @SerialName("TRACKING") TRACKING("TRACKING"),
    @SerialName("AMPLIFIED") AMPLIFIED("AMPLIFIED"),
    @SerialName("AUTO") AUTO("AUTO");
}

@Serializable
enum class GainPolicy(val wireName: String) {
    @SerialName("AUTO") AUTO("AUTO"),
    @SerialName("TRACKING") FORCE_TRACKING("TRACKING"),
    @SerialName("AMPLIFIED") FORCE_AMPLIFIED("AMPLIFIED");
}

/**
 * An action: a [kind] plus its params. Spec 3.6.
 *
 * **There is no id.** An action is identified by its kind and params; a config
 * written by one firmware version must be readable by another, and a numeric id
 * would silently rebind when a kind is inserted. The wire carries
 * [ActionKind.wireName].
 *
 * `target` and `payload` are the two generic string slots; which kind calls them
 * what is [ActionKind.paramKey] / [ActionKind.payloadKey]. `keyMv` is the
 * OUT_VOLTAGE level — a VOLTAGE, not a DAC code, because a code is coupled to the
 * gain mode and changing the mode would silently change what every stored code
 * means.
 */
@Serializable
data class Action(
    val kind: ActionKind,
    val target: String = "",
    val payload: String = "",
    // 0 means "absent" on the wire; the output floor is 1800 mV, so nothing below
    // that is representable and 0 cannot be confused with a real level.
    val keyMv: Int = 0,
)

@Serializable
data class Binding(
    val id: String,
    val channel: BindingChannel,
    val button: String,
    val gesture: Gesture,
    val enabled: Boolean = true,
    // 0 is legal and MEANS "swallow the gesture" (ConfigModel.h).
    val actions: List<Action> = emptyList(),
)

@Serializable
data class GestureTimings(
    val debounceMs: Int = 25,
    val doublePressOffMs: Int = 500,
    val longPressMs: Int = 750,
    val sendDurationMs: Int = 200,
)

@Serializable
data class DeviceSettings(
    val timings: GestureTimings = GestureTimings(),
    val gainPolicy: GainPolicy = GainPolicy.AUTO,
    val buzzerLevel: Int = 2,
    val ledLevel: Int = 2,
    val tempCompEnabled: Boolean = true,
    val maintenanceTimeoutMs: Long = 300_000L,
)

@Serializable
data class LadderButton(
    val id: String,
    val name: String,
    val mvCenter: Int,
    val mvTolerance: Int,
    val learnedAtRailMv: Int = 3300,
    val tempCAtLearn: Int = 0,
    val sampleCount: Int = 0,
    val confidence: Int = 0,
)

@Serializable
data class LadderProfile(
    val source: Int = 0,
    /**
     * The idle reading at LEARN time — the normalization reference.
     *
     * This must NOT be replaced by the current idle: the rail-health check (FR-30)
     * works by comparing the current idle against this one, so they are two
     * different quantities that a careless refactor would collapse.
     */
    val learnedIdleMv: Int = 0,
    val buttons: List<LadderButton> = emptyList(),
)

@Serializable
data class OutputProfile(
    val gainMode: GainMode = GainMode.TRACKING,
    val idleDacCode: Int = 4095,
)

@Serializable
data class ChannelConfig(
    val enabled: Boolean = true,
    val name: String = "",
    val ladder: LadderProfile = LadderProfile(),
    val output: OutputProfile = OutputProfile(),
)

@Serializable
data class AuxButtonConfig(
    val id: String,
    val source: Int,
    val mvCenter: Int,
    val mvTolerance: Int,
)

@Serializable
data class Config(
    val schemaVersion: Int = K_CONFIG_SCHEMA_VERSION,
    val deviceId: String = "",
    val updatedAtMs: Long = 0L,
    val settings: DeviceSettings = DeviceSettings(),
    val channels: List<ChannelConfig> = listOf(ChannelConfig()),
    val aux: List<AuxButtonConfig> = emptyList(),
    val bindings: List<Binding> = emptyList(),
)
