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
 * are CHECKED in [ConfigJson.problems], which [com.oetsolutions.swc.link.SwcClient.setConfig]
 * runs before it sends anything — an earlier version of this comment named
 * `ConfigJson.encode` as the enforcement point, and `encode` contains no width
 * check at all: it would happily write an over-long target and leave the device
 * to refuse the save.
 */

const val K_MAX_CHANNELS = 2
const val K_MAX_BINDINGS = 32
const val K_MAX_ACTIONS_PER_BINDING = 2
const val K_MAX_AUX_BUTTONS = 3
const val K_ACTION_TARGET_LEN = 40
const val K_DATA_PAYLOAD_LEN = 48
const val K_CONFIG_SCHEMA_VERSION = 1

/**
 * The binding-id width, mirroring `kBindingIdLen` (ConfigModel.h). `Binding.id`
 * and `Binding.button` are both `char[kBindingIdLen]`, and `ReadStr` refuses any
 * value at or over the width (truncation is unreachable), so the device's rule is
 * `length < 16` for both.
 *
 * It was a bare `16` literal at the check site until 2026-09-23, and
 * `check_app_limits.py` pinned only the target/payload widths — so a re-tune of
 * `kBindingIdLen` on either side would have gone unnoticed. The failure has two
 * directions and both are silent: a larger firmware width makes the app refuse ids
 * the device would store (the N-44 shape — the gate forecloses its own edit), and a
 * smaller one lets the app send an id the device refuses at decode, which nacks the
 * WHOLE save with the field unnamed.
 */
const val K_BINDING_ID_LEN = 16

/**
 * The ladder-button-id width, mirroring `kLadderIdLen` (LadderDecode.h).
 * `LadderButton.id` is `char[kLadderIdLen]` — a SEPARATE constant from
 * [K_BINDING_ID_LEN], even though both are 16 today. A binding's `button` field
 * copies a ladder id into a `kBindingIdLen` array, so the two are compared
 * field-by-field against the same 16; keeping them as two constants means a
 * re-tune of one alone is caught rather than silently applied to both.
 */
const val K_LADDER_ID_LEN = 16

/**
 * The channel-name width, mirroring `kChannelNameLen` (ConfigModel.h). `ReadStr`
 * refuses a value `>= width`, so an over-width name is a config the device cannot
 * decode — a round-trip violation (FR-27) surfacing as "corrupt config".
 */
const val K_CHANNEL_NAME_LEN = 16

/**
 * The device-id width, mirroring `kDeviceIdLen` (ConfigModel.h). Same rule as
 * [K_CHANNEL_NAME_LEN]: an over-width value is refused at decode by `ReadStr`.
 */
const val K_DEVICE_ID_LEN = 24

/**
 * The ADC's calibrated ceiling in millivolts, mirroring `kAdcFullScaleMv12dB`
 * (CalibrationCurve.h), which the firmware's `LadderDecode.cpp` aliases as its own
 * `kAdcCeilingMv`.
 *
 * Ladder geometry is bounded against THIS, not the 3300 mV rail: no pin reading can
 * exceed the calibrated ceiling, so a value above it is not a measurement. The app
 * must use the same bound, or it accepts a profile the device's `LadderProfileIsValid`
 * refuses.
 */
const val K_ADC_CEILING_MV = 2900

/**
 * The ladder's button count ceiling, mirroring `kLadderMaxButtons`
 * (LadderDecode.h). A stored count above it is refused by the firmware.
 */
const val K_LADDER_MAX_BUTTONS = 16

/**
 * The maintenance window's upper bound, mirroring `kMaintenanceTimeoutMaxMs`
 * (ConfigModel.h). A `uint32` maximum would be ~49.7 days, i.e. a window that
 * never closes on its own — the "device left unable to serve presses" state FR-38
 * exists to prevent. The firmware refuses it; the app must refuse it too, or the
 * whole save is nacked at decode with the offending field unnamed.
 */
const val K_MAINTENANCE_TIMEOUT_MAX_MS = 3_600_000L

/**
 * The key-hold ceiling, mirroring `kSendDurationMaxMs` (ConfigModel.h).
 *
 * `send_duration_ms` is how long the KEY line stays DRIVEN, so a value near the
 * `uint32` maximum (~49.7 days) is a phantom key press the user cannot release —
 * the hazard FR-39 exists to prevent. The firmware refuses it in RANGE, not just
 * by magnitude; the app must too, or the whole save is nacked at decode with the
 * field unnamed — the same drift [K_MAINTENANCE_TIMEOUT_MAX_MS] guards against.
 */
const val K_SEND_DURATION_MAX_MS = 10_000L

/**
 * The `key_mv` range for an `OUT_VOLTAGE` action, mirroring the firmware's
 * `uint16` field and spec 6.2's output envelope.
 *
 * The firmware decodes `key_mv` with `ReadU16` (ConfigCodec.cpp), so a value above
 * 65535 is refused at decode and nacks the WHOLE save with the field unnamed — and
 * 0 is separately refused in range by `ValidateAction` ("OUT_VOLTAGE needs a
 * key_mv"), because 0 means "absent" rather than a reachable level. The picker's
 * Apply gate uses these so it cannot commit a value `ConfigJson.problems()` would
 * then reject on the user's behalf.
 */
const val K_KEY_MV_MIN = 1
const val K_KEY_MV_MAX = 65535

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
    /**
     * FR-33's next-boot maintenance trigger (spec 8.2): the device opens its setup
     * window on the boot that follows a config carrying this set.
     *
     * Defaults to false, matching the firmware. It survives a round trip because
     * the encoder writes it and the decoder reads it back -- a field the app
     * silently dropped would be cleared on the next save, and the device would
     * never open the window the user asked for.
     */
    val maintenanceOnBoot: Boolean = false,
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
