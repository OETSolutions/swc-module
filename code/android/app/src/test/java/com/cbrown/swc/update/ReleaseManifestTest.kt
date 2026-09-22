package com.oetsolutions.swc.update

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The release manifest parser and the version comparison (spec §9.5, open item
 * N-12).
 *
 * This is the APP's mirror of the firmware's `ReleaseCheck`, and the load-bearing
 * cases are the SAME ones `ReleaseCheckTest` covers — because a Kotlin comparison
 * that disagrees with the C one would let the app offer an update the device then
 * refuses, or hide one the device would accept. Semver rules 10 and 11 are the two
 * that are easy to half-implement, so both are pinned here as well as there.
 */
class ReleaseManifestTest {

    private fun manifest(
        latest: String = "1.2.0",
        fw: String = latest,
        url: String = "https://github.com/oetsolutions/swc-module/releases/download/v1.2.0/firmware.bin",
        size: String = "1543210",
        sha: String = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        minFrom: String? = null,
    ): String = buildString {
        append("""{"latest_version":"$latest","channel":"stable",""")
        append(""""firmware":{"version":"$fw","url":"$url","size_bytes":$size,"sha256":"$sha"},""")
        if (minFrom != null) append(""""min_from_version":"$minFrom",""")
        append(""""release_date":"2026-09-18T00:00:00Z"}""")
    }

    private fun decided(json: String, current: String): Pair<ReleaseInfo, ReleaseDecision> {
        val r = ReleaseManifest.check(json, current)
        assertTrue("expected a decision, got $r", r is ManifestCheck.Decided)
        return (r as ManifestCheck.Decided).let { it.info to it.decision }
    }

    @Test
    fun `a newer release is offered`() {
        val (info, decision) = decided(manifest(latest = "1.2.0", minFrom = "1.0.0"), "1.1.0")
        assertEquals(ReleaseDecision.NEWER, decision)
        assertEquals("1.2.0", info.latestVersion)
        assertEquals(1_543_210L, info.sizeBytes)
        assertEquals("1.0.0", info.minFromVersion)
    }

    @Test
    fun `the same version is up to date`() {
        assertEquals(ReleaseDecision.UP_TO_DATE, decided(manifest(latest = "0.12.0"), "0.12.0").second)
    }

    @Test
    fun `an older release is never offered as an upgrade`() {
        // A downgrade is not a lesser update; it is a rollback an attacker can
        // induce to put a known-vulnerable image back on the device.
        assertEquals(ReleaseDecision.NOT_NEWER, decided(manifest(latest = "0.12.0"), "0.13.0").second)
        assertEquals(ReleaseDecision.NOT_NEWER, decided(manifest(latest = "0.12.0"), "1.0.0").second)
    }

    @Test
    fun `a version below the declared floor is refused with its own reason`() {
        val (info, decision) = decided(manifest(latest = "1.2.0", minFrom = "1.0.0"), "0.4.0")
        assertEquals(ReleaseDecision.TOO_OLD_TO_UPGRADE_FROM, decision)
        assertEquals("1.0.0", info.minFromVersion)
        // Exactly AT the floor is allowed -- the boundary belongs to the caller.
        assertEquals(
            ReleaseDecision.NEWER,
            decided(manifest(latest = "1.2.0", minFrom = "1.0.0"), "1.0.0").second,
        )
    }

    @Test
    fun `a numeric gap uses semver, not string comparison`() {
        // The case a flat `version_code` hid entirely, and a string compare gets
        // backwards: 0.10.0 IS newer than 0.9.0.
        assertEquals(ReleaseDecision.NEWER, decided(manifest(latest = "0.10.0"), "0.9.0").second)
    }

    @Test
    fun `a malformed manifest is refused, not partly applied`() {
        assertEquals(ManifestCheck.Malformed, ReleaseManifest.check("{ not json", "0.11.0"))
        assertEquals(ManifestCheck.Malformed, ReleaseManifest.check("{}", "0.11.0"))
        // A manifest missing the hash must not be accepted as "no hash to check".
        val noHash = """{"latest_version":"1.0.0","firmware":{"version":"1.0.0",
            "url":"https://x/y.bin","size_bytes":10}}"""
        assertEquals(ManifestCheck.Malformed, ReleaseManifest.check(noHash, "0.11.0"))
        // A JSON array is not a manifest object.
        assertEquals(ManifestCheck.Malformed, ReleaseManifest.check("[1,2,3]", "0.11.0"))
    }

    @Test
    fun `a non-https image url is refused`() {
        // Plain http is a downgrade attack, not a lesser preference: an attacker on
        // the path substitutes the image, and the SHA-256 does not help because the
        // hash came over the same hijacked channel.
        assertEquals(
            ManifestCheck.Malformed,
            ReleaseManifest.check(
                manifest(url = "http://github.com/oetsolutions/swc-module/releases/download/v1/firmware.bin"),
                "0.11.0",
            ),
        )
    }

    @Test
    fun `a manifest whose two versions disagree is refused`() {
        assertEquals(
            ManifestCheck.Malformed,
            ReleaseManifest.check(manifest(latest = "9.9.9", fw = "1.0.0"), "0.1.0"),
        )
    }

    @Test
    fun `a fractional size is refused rather than truncated`() {
        // The firmware's `ReadSize` refuses this for the same reason: a bare cast
        // accepts a value the sender did not write, and here it would declare a size
        // no image has, so every download fails with an unexplained size mismatch.
        assertEquals(
            ManifestCheck.Malformed,
            ReleaseManifest.check(manifest(size = "1543210.9"), "0.1.0"),
        )
        // A whole number with a `.0` is still whole and accepted.
        assertEquals(
            ReleaseDecision.NEWER,
            decided(manifest(latest = "1.2.0", size = "1543210.0"), "0.1.0").second,
        )
    }

    @Test
    fun `a missing min_from_version means any version may upgrade`() {
        val (info, decision) = decided(manifest(latest = "2.0.0", minFrom = null), "0.1.0")
        assertEquals("", info.minFromVersion)
        assertEquals(ReleaseDecision.NEWER, decision)
    }

    @Test
    fun `a present-but-wrong-typed optional field is malformed, not absent`() {
        // `min_from_version` set to a NUMBER is a manifest bug the parser must not
        // read as "no floor": treating it as absent would offer an update the floor
        // was meant to gate.
        val bad = """{"latest_version":"1.2.0","min_from_version":123,
            "firmware":{"version":"1.2.0","url":"https://x/y.bin","size_bytes":10,
            "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}}"""
        assertEquals(ManifestCheck.Malformed, ReleaseManifest.check(bad, "0.1.0"))
    }

    // --- semver, rules 10 and 11 ------------------------------------------

    @Test
    fun `a pre-release sorts below its release`() {
        // Rule 11. Without this a device (or app) running the pre-release is never
        // offered the final release -- the exact failure the firmware fixed.
        assertTrue(Semver.compare("1.3.0-rc1", "1.3.0") < 0)
        assertTrue(Semver.compare("1.3.0", "1.3.0-rc1") > 0)
        assertEquals(
            ReleaseDecision.NEWER,
            decided(manifest(latest = "1.3.0"), "1.3.0-rc1").second,
        )
    }

    @Test
    fun `build metadata is ignored on a pre-release as well as on a release`() {
        // Rule 10, the HALF-DONE case: stripping `+` only when it leads the string
        // leaves `1.3.0-rc+b` inside the compared pre-release, so it reads as newer
        // than `1.3.0-rc` and the check offers an update to the same version.
        assertEquals(0, Semver.compare("1.3.0-rc+build", "1.3.0-rc"))
        assertEquals(0, Semver.compare("1.3.0+build", "1.3.0"))
        assertEquals(0, Semver.compare("1.3.0-rc", "1.3.0-rc+other"))
    }

    @Test
    fun `pre-release identifiers order numerically and numeric below alphanumeric`() {
        // Rule 11's identifier rules.
        assertTrue(Semver.compare("1.3.0-2", "1.3.0-10") < 0)
        // An ALPHANUMERIC identifier compares byte-wise, NOT numerically: "rc2" is
        // greater than "rc10" because '2' > '1'. This is semver's own rule (and its
        // well-known gotcha), so the app must not "helpfully" number-suffix it; the
        // firmware's identifier compare is lexical too, and the two must agree.
        assertTrue(Semver.compare("1.3.0-rc2", "1.3.0-rc10") > 0)
        // numeric < alphanumeric
        assertTrue(Semver.compare("1.3.0-1", "1.3.0-alpha") < 0)
        // fewer identifiers sorts lower
        assertTrue(Semver.compare("1.3.0-alpha", "1.3.0-alpha.1") < 0)
    }

    @Test
    fun `a fourth component is compared numerically, not by text`() {
        // Semver forbids a 4th field, but a date- or build-stamped tag produces one.
        // A text compare inverts 1.2.3.9 vs 1.2.3.10.
        assertTrue(Semver.compare("1.2.3.9", "1.2.3.10") < 0)
        assertTrue(Semver.compare("202609231.0.0", "202609230.0.0") > 0)
        // A missing component counts as zero.
        assertEquals(0, Semver.compare("1.2.3", "1.2.3.0"))
    }

    @Test
    fun `an over-wide component does not wrap and invert the order`() {
        // The firmware's own bug (a 32-bit `long` on xtensa wrapped and inverted the
        // order). Kotlin's `Long` is 64-bit here, but the comparison is written as a
        // digit-run compare so the rule is identical on both sides.
        assertTrue(Semver.compare("99999999999999999999.0.0", "1.0.0") > 0)
        assertTrue(Semver.compare("99999999999999999998.0.0", "99999999999999999999.0.0") < 0)
    }

    @Test
    fun `a missing component reads as zero`() {
        assertEquals(0, Semver.compare("1.2", "1.2.0"))
        assertEquals(0, Semver.compare("1", "1.0.0"))
        assertTrue(Semver.compare("1.2", "1.2.1") < 0)
    }
}
