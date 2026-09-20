package com.oetsolutions.swc.model

import com.oetsolutions.swc.contract.ActionKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** A config exercising every field the model carries, for the round-trip tests. */
fun sampleConfig(): Config = Config(
    schemaVersion = 1,
    deviceId = "swc-a1b2c3",
    updatedAtMs = 1_700_000_000_000L,
    settings = DeviceSettings(
        timings = GestureTimings(25, 500, 750, 200),
        gainPolicy = GainPolicy.AUTO,
        buzzerLevel = 2,
        ledLevel = 1,
        tempCompEnabled = true,
        maintenanceTimeoutMs = 300_000L,
    ),
    channels = listOf(
        ChannelConfig(
            enabled = true,
            name = "SWC1",
            ladder = LadderProfile(
                source = 0,
                learnedIdleMv = 2835,
                buttons = listOf(
                    LadderButton("vol_up", "Volume Up", 1430, 120, 3300, 235, 200, 98),
                    LadderButton("next", "Next Track", 2145, 110, 3300, 235, 200, 99),
                ),
            ),
            output = OutputProfile(GainMode.TRACKING, 4095),
        )
    ),
    aux = listOf(AuxButtonConfig("aux1", 1, 100, 1600)),
    bindings = listOf(
        Binding(
            "b1", BindingChannel.SWC1, "vol_up", Gesture.SINGLE, true,
            listOf(Action(ActionKind.OUT_VOLTAGE, "", "", 2400)),
        ),
        Binding(
            "b2", BindingChannel.SWC1, "vol_up", Gesture.LONG, true,
            listOf(Action(ActionKind.OUT_RELEASE)),
        ),
        Binding(
            "b3", BindingChannel.SWC1, "next", Gesture.DOUBLE, true,
            listOf(Action(ActionKind.APP_LAUNCH, "com.spotify.music")),
        ),
        Binding(
            "b4", BindingChannel.ANY, "next", Gesture.LONG, true,
            listOf(
                Action(
                    ActionKind.APP_INTENT,
                    "com.oetsolutions.swc.ACTION_NAVIGATE",
                    "geo:40.7608,-111.8910?q=Home",
                )
            ),
        ),
    ),
)

class ConfigCodecTest {

    @Test
    fun `every field survives the encode-decode round trip`() {
        val c = sampleConfig()
        val decoded = ConfigJson.decode(ConfigJson.encode(c))
        assertEquals(c, decoded)
    }

    @Test
    fun `the encoded action uses the KINDS own parameter name`() {
        // Spec 3.6: an action carries its params under the kind's own names. An
        // app writing a generic `target` key produces a config the firmware's
        // decoder routes to its default case -- storing an action with no target,
        // which is exactly the "silently accepted, behaves differently" failure.
        val text = ConfigJson.encode(sampleConfig())
        assertTrue("APP_LAUNCH must write `package`", text.contains("\"package\":\"com.spotify.music\""))
        assertTrue("APP_INTENT must write `action`", text.contains("\"action\":\"com.oetsolutions"))
        assertTrue("APP_INTENT must write `data`", text.contains("\"data\":\"geo:40.7608"))
        assertTrue("OUT_VOLTAGE must write key_mv", text.contains("\"key_mv\":2400"))
    }

    @Test
    fun `a field the kind does not use is omitted rather than written empty`() {
        val text = ConfigJson.encode(sampleConfig())
        assertTrue("OUT_RELEASE is exactly {kind}", text.contains("{\"kind\":\"OUT_RELEASE\"}"))
    }

    @Test
    fun `temperature and confidence are scaled on the wire`() {
        // The model holds tenths (235) and percent (98); the wire carries 23.5
        // and 0.98 (spec 3.7). Encoding the raw integers would round-trip in a
        // test that only compares models while sending the device a temperature
        // 10x too high.
        val text = ConfigJson.encode(sampleConfig())
        assertTrue(text.contains("\"temp_c_at_learn\":23.5"))
        assertTrue(text.contains("\"confidence\":0.98"))
    }

    @Test
    fun `an unknown action kind decodes to a sentinel rather than throwing`() {
        // A newer firmware may write a kind this app predates. Opening the config
        // must not crash -- spec 4.5: show the mismatch, do not fail silently or
        // violently. A kind is a NAME on the wire, so this is a lookup with a
        // fallback (an unknown numeric action id cannot occur; spec 3.6 has none).
        val text = """
            {"schema_version":1,"device_id":"d","updated_at_ms":0,
             "settings":{},"aux":[],
             "channels":[{"name":"SWC1","enabled":true}],
             "bindings":[{"id":"b1","channel":"SWC1","button":"x","gesture":"SINGLE",
                          "enabled":true,
                          "actions":[{"kind":"FUTURE_KIND","whatever":1}]}]}
        """.trimIndent()
        val c = ConfigJson.decode(text)
        assertEquals(ActionKind.NONE, c.bindings[0].actions[0].kind)
    }

    @Test
    fun `validation refuses an OUT_VOLTAGE with no key_mv`() {
        val bad = sampleConfig().copy(
            bindings = listOf(
                Binding("b1", BindingChannel.SWC1, "vol_up", Gesture.SINGLE, true,
                    listOf(Action(ActionKind.OUT_VOLTAGE, "", "", 0)))
            )
        )
        val problems = ConfigJson.problems(bad)
        assertTrue("expected a key_mv complaint, got $problems",
            problems.any { it.contains("OUT_VOLTAGE needs a key_mv") })
    }

    @Test
    fun `validation refuses a kind that needs a target with an empty one`() {
        val bad = sampleConfig().copy(
            bindings = listOf(
                Binding("b1", BindingChannel.SWC1, "vol_up", Gesture.SINGLE, true,
                    listOf(Action(ActionKind.APP_LAUNCH, "")))
            )
        )
        assertTrue(ConfigJson.problems(bad).any { it.contains("APP_LAUNCH needs a package") })
    }

    @Test
    fun `validation refuses an over-width target`() {
        // The firmware copies into char[40] and REFUSES a longer value rather
        // than truncating (spec 3.5), so the app must catch it first or the user
        // loses the config after tapping Save.
        val bad = sampleConfig().copy(
            bindings = listOf(
                Binding("b1", BindingChannel.SWC1, "vol_up", Gesture.SINGLE, true,
                    listOf(Action(ActionKind.APP_LAUNCH, "x".repeat(40))))
            )
        )
        assertTrue(ConfigJson.problems(bad).any { it.contains("under 40 chars") })
    }

    @Test
    fun `a valid sample config has no problems`() {
        assertEquals(emptyList<String>(), ConfigJson.problems(sampleConfig()))
    }

    @Test
    fun `the encoded form is compact`() {
        // The device CRCs exactly these bytes (FR-27), so whitespace is not a
        // formatting preference -- pretty-printing would change the hash.
        val text = ConfigJson.encode(sampleConfig())
        assertTrue("encoded JSON must be single-line", !text.contains('\n'))
        assertTrue("encoded JSON must not be pretty-printed", !text.contains(": "))
    }
}
