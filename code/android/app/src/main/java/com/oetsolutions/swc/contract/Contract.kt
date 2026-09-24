// GENERATED FILE -- do not edit by hand.
//
// Produced by code/tools/gen_contract_kotlin.py from code/tools/contract_schema.py.
// Regenerate with:
//     cd code/tools && python3 gen_contract_kotlin.py
// and commit the result. `test_gen_contract.py` fails if this file and the schema
// disagree.
//
// This is the app's half of the contract that keeps it from drifting from the
// firmware. Referencing these symbols rather than string literals means a frame
// or kind rename breaks the build instead of silently failing at runtime against
// a head unit in a car.
//
// There is deliberately NO `ActionIds` object. Spec 3.6: an action has no id.
package com.oetsolutions.swc.contract

/**
 * The action kinds of spec 3.6, in the firmware's declaration order.
 *
 * [wireName] is what actually travels: spec 3.7 writes `"kind": "OUT_VOLTAGE"`.
 * The ordinal is in-memory only and must never be sent -- inserting a kind would
 * rebind every stored ordinal, so a config written by an older firmware would
 * silently mean something else.
 *
 * [paramKey] is the kind's required string parameter and [payloadKey] its
 * optional second one (spec 3.6: "each kind names them on the wire"). APP_INTENT
 * is the only kind that uses both -- the user's stated example, an intent with a
 * data payload.
 */
enum class ActionKind(
    val wireName: String,
    val paramKey: String?,
    val payloadKey: String?,
    val needsParam: Boolean,
) {
    NONE("NONE", null, null, false),
    OUT_VOLTAGE("OUT_VOLTAGE", null, null, false),
    OUT_RELEASE("OUT_RELEASE", null, null, false),
    APP_LAUNCH("APP_LAUNCH", "package", null, true),
    APP_INTENT("APP_INTENT", "action", "data", true),
    KEYCODE("KEYCODE", "keycode", null, true),
    MEDIA("MEDIA", "command", null, true),
    VOLUME("VOLUME", "target", null, true),
    SYSTEM("SYSTEM", "command", null, true),
    BUZZ("BUZZ", "pattern", null, true),
    APP_RAW("APP_RAW", "command", null, true);

    companion object {
        /**
         * Look up a kind by its wire name, or null if this app does not know it.
         *
         * Null rather than an exception: a newer firmware may legitimately send a
         * kind this app predates, and the caller must show that honestly (spec
         * 4.5 -- no silent partial compatibility) rather than crash.
         */
        fun fromWireName(name: String): ActionKind? = entries.firstOrNull { it.wireName == name }
    }
}

/**
 * The frame `type` strings of spec 4.3.
 *
 * A single object of constants so every call site is a symbol reference. The
 * firmware's half is `SWC_FRAME_*` in `contract/swc_contract.h`.
 */
object Frames {
    const val HELLO = "hello"
    const val EVENT = "event"
    const val STATUS = "status"
    const val LADDER_SAMPLE = "ladder_sample"
    const val MAINTENANCE = "maintenance"
    const val ACK = "ack"
    const val NACK = "nack"
    const val LOG = "log"
    const val LINK_GAP = "link_gap"
    const val CONFIG_GET = "config_get"
    const val CONFIG_BEGIN = "config_begin"
    const val CONFIG_CHUNK = "config_chunk"
    const val CONFIG_END = "config_end"
    const val CONFIG_PATCH = "config_patch"
    const val LEARN_START = "learn_start"
    const val LEARN_STOP = "learn_stop"
    const val LEARN_COMMIT = "learn_commit"
    const val MAINTENANCE_ENTER = "maintenance_enter"
    const val MAINTENANCE_EXIT = "maintenance_exit"
    const val TEST_KEY = "test_key"
    const val IDENTIFY = "identify"
    const val REBOOT = "reboot"
    const val PING = "ping"
    const val TIME_SYNC = "time_sync"
    const val OTA_BEGIN = "ota_begin"
    const val OTA_CHUNK = "ota_chunk"
    const val OTA_END = "ota_end"
}

/**
 * The wire protocol version, which must match the firmware's `hello.protocol_v`.
 *
 * Spec 4.5: if the major version differs the app MUST show an explicit mismatch
 * state rather than attempting to talk. Silent partial compatibility is how a
 * config gets corrupted.
 */
const val PROTOCOL_VERSION = 1
