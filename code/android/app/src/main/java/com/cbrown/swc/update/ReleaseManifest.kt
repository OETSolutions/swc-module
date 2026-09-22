package com.oetsolutions.swc.update

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/*
 * The release manifest and its comparison (spec §9.5), the APP's half of the
 * update check.
 *
 * Spec §9.5 puts the check in the app on purpose: the ESP32 may have no WiFi in
 * the car, so the phone checks over its own connection and pushes the resulting
 * file over USB. This is the Kotlin counterpart of the firmware's `ReleaseCheck`
 * (`lib/Update/ReleaseCheck.{h,cpp}`), and it is deliberately a faithful mirror:
 * the same nested manifest shape, the same required fields, the same refusal of a
 * self-contradictory manifest, and the same version comparison — because two
 * implementations of "is this release newer" that disagree would let the app
 * offer an update the device then refuses, or hide one the device would take.
 *
 * **Mirrored, not shared.** There is no cross-language codegen for this (the wire
 * contract generator covers frames, not the manifest), so the rules are restated
 * here and the tests on both sides cover the same cases. The firmware's header
 * says why its shape is what it is; the load-bearing parts are repeated below so a
 * reader of one file is not silently missing a rule from the other.
 */

/**
 * Where the published manifest lives (spec §9.5's "git-release scheme").
 *
 * GitHub Releases' `latest/download` alias resolves to the newest release's asset
 * of that name, so this URL is stable across versions and needs no per-release
 * edit. The owner/repo is the project's own remote (`oetsolutions/swc-module`).
 *
 * It is a constant, not a setting, because there is exactly one release channel
 * today; spec §9.5's `channel` (`stable` / `beta`) is carried in the manifest for
 * when a beta path is added, and a channel would select a different URL here.
 */
const val kReleaseManifestUrl =
    "https://github.com/oetsolutions/swc-module/releases/latest/download/version_manifest.json"

/** The manifest's fields (spec §9.5), after a successful parse. */
data class ReleaseInfo(    val latestVersion: String,
    val firmwareVersion: String,
    val sha256: String,
    val sizeBytes: Long,
    val url: String,
    /** Empty when the manifest carries none — the update is offered from any version. */
    val minFromVersion: String,
    /** `stable` / `beta`, carried for display. Empty when absent. */
    val channel: String,
)

/** What the check decided (spec §9.5 step 2 and 4). */
enum class ReleaseDecision {
    /** Same version as the running one. */
    UP_TO_DATE,

    /** Installable. */
    NEWER,

    /** Older than the running version: never offer a downgrade. */
    NOT_NEWER,

    /** The image is newer, but the running version is below `min_from_version`. */
    TOO_OLD_TO_UPGRADE_FROM,

    /** Not parseable, or a required field missing / self-contradictory. */
    MALFORMED,
}

/**
 * The result of checking one manifest. On [MALFORMED] there is no `info`, because
 * a manifest that failed a structural rule must not be partly trusted — the
 * firmware returns `kMalformed` with `out` untouched for the same reason.
 */
sealed interface ManifestCheck {
    data class Decided(val info: ReleaseInfo, val decision: ReleaseDecision) : ManifestCheck
    data object Malformed : ManifestCheck
}

object ReleaseManifest {

    /**
     * Parse `json` and decide whether it describes an installable release relative
     * to `currentVersion`. One entry point, mirroring `ReleaseCheckParse`.
     *
     * The order of the final three decisions matters and mirrors the firmware's:
     * "not newer" before "too old", because a manifest that is BOTH older than the
     * running firmware AND below its own floor is best described as "no upgrade
     * here", not as a migration problem.
     */
    fun check(json: String, currentVersion: String): ManifestCheck {
        val root = try {
            Json.parseToJsonElement(json).jsonObject
        } catch (e: Exception) {
            // A body that is not a JSON object is malformed, not an exception: the
            // caller decides the status, and a thrown parse would escape the
            // coroutine that called it.
            return ManifestCheck.Malformed
        }
        val info = parse(root) ?: return ManifestCheck.Malformed
        val cmp = Semver.compare(info.latestVersion, currentVersion)
        val decision = when {
            cmp == 0 -> ReleaseDecision.UP_TO_DATE
            cmp < 0 -> ReleaseDecision.NOT_NEWER
            info.minFromVersion.isNotEmpty() &&
                Semver.compare(currentVersion, info.minFromVersion) < 0 ->
                ReleaseDecision.TOO_OLD_TO_UPGRADE_FROM
            else -> ReleaseDecision.NEWER
        }
        return ManifestCheck.Decided(info, decision)
    }

    /**
     * The structural rules. A violation returns null, which the caller reports as
     * [ReleaseDecision.MALFORMED] — never a defaulted field, because a manifest
     * with no hash must not be read as "no hash to check".
     */
    private fun parse(root: JsonObject): ReleaseInfo? {
        val latest = root.str("latest_version") ?: return null

        val fw = (root["firmware"] as? JsonObject) ?: return null
        val fwVersion = fw.str("version") ?: return null
        val sha = fw.str("sha256") ?: return null
        val size = fw.size("size_bytes") ?: return null
        val url = fw.str("url") ?: return null

        // Optional: present-but-wrong-type is a malformed manifest, absent is fine.
        val minFrom = root.optStr("min_from_version") ?: return null
        val channel = root.optStr("channel") ?: return null

        // The image must come over TLS. Plain http is a downgrade attack, not a
        // lesser preference: an attacker on the path substitutes the image, and the
        // SHA-256 does not help because the hash arrived over the same hijacked
        // channel.
        if (!url.startsWith("https://")) return null

        // The top-level version is what the pipeline publishes and the firmware
        // block repeats it. If they disagree the manifest is self-contradictory, and
        // picking one silently is how a device installs a version it did not choose.
        if (latest != fwVersion) return null

        return ReleaseInfo(latest, fwVersion, sha, size, url, minFrom, channel)
    }

    /** A required non-empty string field, or null if missing/empty/wrong-typed. */
    private fun JsonObject.str(key: String): String? {
        val v = this[key] as? JsonPrimitive ?: return null
        if (!v.isString) return null
        val s = v.content
        return if (s.isEmpty()) null else s
    }

    /**
     * An optional string field. Absent is fine (empty result); present-but-not-a-
     * string is a malformed manifest, which is why this cannot collapse the two
     * cases onto one nullable.
     */
    private fun JsonObject.optStr(key: String): String? {
        val raw = this[key] ?: return ""
        val v = raw as? JsonPrimitive ?: return null
        if (!v.isString) return null
        return v.content
    }

    /**
     * A required whole-number byte count.
     *
     * A fraction is REFUSED, not truncated — the same rule the firmware's
     * `ReadSize` and `CommandRouter`'s numeric readers enforce. A bare cast accepts
     * a value the sender did not write, and here the consequence is an unexplained
     * permanent failure: `size_bytes: 1543210.9` declares a size no real image has,
     * so every download ends in a size mismatch that names neither the manifest nor
     * the field.
     */
    private fun JsonObject.size(key: String): Long? {
        val v = this[key] as? JsonPrimitive ?: return null
        if (v.isString) return null
        val d = v.content.toDoubleOrNull() ?: return null
        if (d <= 0.0 || d > 4294967295.0) return null
        if (d != kotlin.math.floor(d)) return null
        // `d.toLong()`, NOT `v.longOrNull`: JSON `1543210.0` is a whole byte count
        // written in decimal form, and `longOrNull` rejects it (the content has a
        // dot). The firmware's `ReadSize` accepts an integral `valuedouble`, and the
        // two must agree — refusing this would make the app call a real manifest
        // malformed while the device accepts it.
        return d.toLong()
    }
}

/**
 * Semver comparison, semver 2.0.0 rules 10 and 11, mirroring the firmware's
 * `SemverCompare` (`lib/Update/ImageVerify.cpp`) field for field.
 *
 * Rules 11 and 10 are the two that are easy to half-implement, and the firmware's
 * own comments record both as shipped bugs fixed:
 *
 *  - **Rule 11:** a pre-release sorts BELOW its release (`1.3.0-rc1 < 1.3.0`), and
 *    two pre-releases order identifier by identifier. Without this, a device (or
 *    app) running the pre-release is never offered the final release.
 *  - **Rule 10:** build metadata (`+suffix`) is IGNORED for precedence, **on a
 *    pre-release as well as on a bare release** (`1.3.0-rc+b == 1.3.0-rc`).
 *    Stripping `+` only when it leads the string is the half-done version, which
 *    makes `1.3.0-rc+b` read as newer than `1.3.0-rc` and offers an update to the
 *    same version.
 *
 * **Numeric components compare as digits, not as a parsed integer.** The firmware
 * learned this the hard way (a `long` is 64-bit on the host but 32-bit on xtensa,
 * so an over-wide component wrapped and inverted an ordering). Kotlin's `Long` is
 * 64-bit everywhere this app runs, so the overflow does not bite here — but the
 * comparison is written the same way anyway, because the rule that matters is that
 * the two implementations agree, and a digit-run compare cannot overflow on either
 * side. It also handles a date-stamped tag (`20260923`) and a 4th component, which
 * semver forbids but a real tag produces.
 */
object Semver {

    /** <0, 0 or >0. A missing component counts as zero, so "1.2" equals "1.2.0". */
    fun compare(a: String, b: String): Int {
        var pa = 0
        var pb = 0

        // The three defined components: major.minor.patch.
        for (i in 0 until 3) {
            val sa = pa
            while (pa < a.length && a[pa] in '0'..'9') pa++
            val sb = pb
            while (pb < b.length && b[pb] in '0'..'9') pb++
            val c = compareNumericRuns(a, sa, pa - sa, b, sb, pb - sb)
            if (c != 0) return c
            // The dot is consumed only BETWEEN components, so a 4th component's
            // separator survives for the extension loop below.
            if (i < 2) {
                if (pa < a.length && a[pa] == '.') pa++
                if (pb < b.length && b[pb] == '.') pb++
            }
        }

        // A 4th or later numeric component, compared numerically (so 1.2.3.9 sorts
        // BELOW 1.2.3.10 — a text compare would invert that pair). A component one
        // side lacks reads as zero.
        while (true) {
            val hasA = pa < a.length && a[pa] == '.'
            val hasB = pb < b.length && b[pb] == '.'
            if (!hasA && !hasB) break
            if (hasA) pa++
            if (hasB) pb++
            val sa = pa
            while (pa < a.length && a[pa] in '0'..'9') pa++
            val sb = pb
            while (pb < b.length && b[pb] in '0'..'9') pb++
            val c = compareNumericRuns(a, sa, pa - sa, b, sb, pb - sb)
            if (c != 0) return c
        }

        // Pre-release and build metadata. `+` metadata is dropped from a
        // pre-release too (rule 10), and the pre-release runs to the `+` or the end.
        val paPre = if (pa < a.length && a[pa] == '-') pa + 1 else -1
        val pbPre = if (pb < b.length && b[pb] == '-') pb + 1 else -1
        val na = if (paPre >= 0) lenBeforePlus(a, paPre) else 0
        val nb = if (pbPre >= 0) lenBeforePlus(b, pbPre) else 0
        return prereleaseCompare(
            a, if (paPre >= 0) paPre else 0, na,
            b, if (pbPre >= 0) pbPre else 0, nb,
        )
    }

    private fun lenBeforePlus(s: String, from: Int): Int {
        var i = from
        while (i < s.length && s[i] != '+') i++
        return i - from
    }

    /**
     * Compare two digit runs as non-negative integers of unbounded width: strip
     * leading zeros, then compare by digit count, then digit by digit.
     */
    private fun compareNumericRuns(
        a: String, aFrom: Int, aLen: Int,
        b: String, bFrom: Int, bLen: Int,
    ): Int {
        var i = 0
        while (i < aLen && a[aFrom + i] == '0') i++
        var j = 0
        while (j < bLen && b[bFrom + j] == '0') j++
        val la = aLen - i
        val lb = bLen - j
        if (la != lb) return if (la < lb) -1 else 1
        if (la == 0) return 0
        for (k in 0 until la) {
            val ca = a[aFrom + i + k]
            val cb = b[bFrom + j + k]
            if (ca != cb) return if (ca < cb) -1 else 1
        }
        return 0
    }

    /**
     * Semver rule 11. `na`/`nb` are the pre-release LENGTHS (0 = that side carries
     * no `-`, i.e. it is a release and therefore NEWER than any pre-release).
     * Length-bounded rather than substring-sliced for the same reason the firmware
     * passes slices: the caller has already excluded the `+build` metadata.
     */
    private fun prereleaseCompare(
        a: String, aFrom: Int, na: Int,
        b: String, bFrom: Int, nb: Int,
    ): Int {
        val hasA = na > 0
        val hasB = nb > 0
        if (!hasA && !hasB) return 0
        if (!hasA) return 1    // a is the release, b a pre-release -> a newer
        if (!hasB) return -1
        var ia = 0
        var ib = 0
        while (ia < na || ib < nb) {
            if (ia >= na) return -1   // a ran out of identifiers first -> older
            if (ib >= nb) return 1
            var ea = ia
            while (ea < na && a[aFrom + ea] != '.') ea++
            var eb = ib
            while (eb < nb && b[bFrom + eb] != '.') eb++
            val c = identCompare(
                a, aFrom + ia, ea - ia,
                b, bFrom + ib, eb - ib,
            )
            if (c != 0) return c
            ia = if (ea < na && a[aFrom + ea] == '.') ea + 1 else ea
            ib = if (eb < nb && b[bFrom + eb] == '.') eb + 1 else eb
        }
        return 0
    }

    /** One identifier: numeric beats nothing, numeric < alphanumeric, else byte-wise. */
    private fun identCompare(
        a: String, aFrom: Int, na: Int,
        b: String, bFrom: Int, nb: Int,
    ): Int {
        var aNum = na > 0
        for (k in 0 until na) if (a[aFrom + k] !in '0'..'9') aNum = false
        var bNum = nb > 0
        for (k in 0 until nb) if (b[bFrom + k] !in '0'..'9') bNum = false
        if (aNum && bNum) return compareNumericRuns(a, aFrom, na, b, bFrom, nb)
        if (aNum != bNum) return if (aNum) -1 else 1   // numeric < alphanumeric
        val m = minOf(na, nb)
        for (k in 0 until m) {
            val ca = a[aFrom + k]
            val cb = b[bFrom + k]
            if (ca != cb) return if (ca < cb) -1 else 1
        }
        return when {
            na == nb -> 0
            na < nb -> -1
            else -> 1
        }
    }
}
