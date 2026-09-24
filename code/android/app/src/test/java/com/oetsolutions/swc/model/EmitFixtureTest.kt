package com.oetsolutions.swc.model

import org.junit.Test
import java.io.File

/**
 * Writes the Kotlin codec's encoding of `sampleConfig()` to a file so the
 * firmware-side host test can decode it. Not an assertion -- it is the producer
 * half of the cross-language agreement check.
 *
 * **The target is the CHECKED-IN fixture, `code/contract/swc_sample_config.json`.**
 * It used to default to `build/`, which is cleaned, while `tools/crosscheck_config.sh`
 * read `/tmp/` -- so the one check that proves the Kotlin and C codecs agree was
 * unrunnable from a clean checkout and ran in no CI job. Both halves now share this
 * path, the android workflow regenerates it and fails on a diff, and the firmware
 * workflow decodes it with the firmware's own decoder. Override the target with
 * `-Dswc.fixture.out=...` for a one-off.
 */
class EmitFixtureTest {
    @Test
    fun emit() {
        val out = File(System.getProperty("swc.fixture.out") ?: "../../contract/swc_sample_config.json")
        out.parentFile?.mkdirs()
        out.writeText(ConfigJson.encode(sampleConfig()))
        println("FIXTURE=${out.absolutePath}")
    }
}
