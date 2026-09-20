package com.oetsolutions.swc.model

import org.junit.Test
import java.io.File

/**
 * Writes the Kotlin codec's encoding of `sampleConfig()` to a file so the
 * firmware-side host test can decode it. Not an assertion -- it is the producer
 * half of the cross-language agreement check.
 */
class EmitFixtureTest {
    @Test
    fun emit() {
        val out = File(System.getProperty("swc.fixture.out") ?: "build/swc_sample_config.json")
        out.parentFile?.mkdirs()
        out.writeText(ConfigJson.encode(sampleConfig()))
        println("FIXTURE=${out.absolutePath}")
    }
}
