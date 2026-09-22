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
    fun `validation refuses an over-width binding id and ladder button id`() {
        // Both ids are `char[16]` on the firmware side -- `kBindingIdLen` for
        // `Binding.id`, `kLadderIdLen` for `LadderButton.id` -- and `ReadStr`
        // REFUSES a value at or over the width rather than truncating. The check is
        // `length < N`, so exactly N characters must be refused: that boundary is
        // the whole point of a width mirror, and it is what N-44 got wrong when the
        // app DERIVED a 16-char binding id and then refused its own edit.
        val idAtWidth = "x".repeat(16)
        val idUnderWidth = "x".repeat(15)

        fun problemsWith(bindings: List<Binding>, buttons: List<LadderButton>) =
            ConfigJson.problems(
                sampleConfig().copy(
                    bindings = bindings,
                    channels = listOf(
                        sampleConfig().channels[0].copy(
                            ladder = sampleConfig().channels[0].ladder.copy(buttons = buttons),
                        ),
                    ),
                )
            )

        val goodButton = sampleConfig().channels[0].ladder.buttons.first()
        val bindingAtWidth = Binding(
            idAtWidth, BindingChannel.SWC1, goodButton.id, Gesture.SINGLE, true,
            listOf(Action(ActionKind.OUT_VOLTAGE, keyMv = 2000)),
        )

        assertTrue(
            "a 16-char binding id is at the width and must be refused: " +
                "${problemsWith(listOf(bindingAtWidth), listOf(goodButton))}",
            problemsWith(listOf(bindingAtWidth), listOf(goodButton))
                .any { it.contains("under 16 chars") },
        )
        assertTrue(
            "a 15-char binding id is under the width and must be accepted",
            problemsWith(
                listOf(bindingAtWidth.copy(id = idUnderWidth)),
                listOf(goodButton),
            ).isEmpty(),
        )
        assertTrue(
            "a 16-char ladder button id is at the width and must be refused: " +
                "${problemsWith(emptyList(), listOf(goodButton.copy(id = idAtWidth)))}",
            problemsWith(emptyList(), listOf(goodButton.copy(id = idAtWidth)))
                .any { it.contains("under 16 chars") },
        )
        assertTrue(
            "a 15-char ladder button id is under the width and must be accepted",
            problemsWith(emptyList(), listOf(goodButton.copy(id = idUnderWidth))).isEmpty(),
        )
    }

    @Test
    fun `a valid sample config has no problems`() {
        assertEquals(emptyList<String>(), ConfigJson.problems(sampleConfig()))
    }

    /**
     * The firmware's `ConfigValidate` bounds these and the app did not, so a
     * config could pass the app's local gate and then be nacked by the device at
     * decode -- losing the whole save with the offending field UNNAMED. Each
     * check here mirrors a specific firmware rule; if the firmware's rule moves,
     * this test is the reminder that the app's copy moved too.
     */
    @Test
    fun `the app refuses the fields the firmware refuses`() {
        val base = sampleConfig()
        fun problemsWith(settings: DeviceSettings, channels: List<ChannelConfig>) =
            ConfigJson.problems(base.copy(settings = settings, channels = channels))

        // maintenance_timeout_ms: bounded on both ends (kMaintenanceTimeoutMaxMs).
        assertTrue(
            "a >max maintenance window must be refused, not sent to be nacked",
            problemsWith(
                base.settings.copy(maintenanceTimeoutMs = K_MAINTENANCE_TIMEOUT_MAX_MS + 1),
                base.channels,
            ).any { it.contains("maintenance_timeout_ms") },
        )
        assertTrue(
            "a zero maintenance window would close instantly",
            problemsWith(base.settings.copy(maintenanceTimeoutMs = 0), base.channels)
                .any { it.contains("maintenance_timeout_ms") },
        )
        // buzzer_level / led_level are 0..3.
        assertTrue(
            "buzzer_level above 3 is refused by the firmware",
            problemsWith(base.settings.copy(buzzerLevel = 4), base.channels)
                .any { it.contains("buzzer_level") },
        )
        assertTrue(
            "led_level above 3 is refused by the firmware",
            problemsWith(base.settings.copy(ledLevel = 4), base.channels)
                .any { it.contains("led_level") },
        )
        // channel_count == 0 is refused (ConfigValidate).
        assertTrue(
            "a config with no channels is refused by the firmware",
            problemsWith(base.settings, emptyList()).any { it.contains("1 channel") },
        )
        // send_duration_ms: bounded on both ends (kSendDurationMaxMs). The top end
        // is how long the KEY line is DRIVEN, so an unbounded value is a phantom
        // press the user cannot release (FR-15/FR-39).
        assertTrue(
            "a >max send_duration_ms must be refused, not sent to be nacked",
            problemsWith(
                base.settings.copy(
                    timings = base.settings.timings.copy(
                        sendDurationMs = (K_SEND_DURATION_MAX_MS + 1).toInt(),
                    ),
                ),
                base.channels,
            ).any { it.contains("send_duration_ms") },
        )
        assertTrue(
            "a zero send_duration_ms would drive the line for no time at all",
            problemsWith(
                base.settings.copy(timings = base.settings.timings.copy(sendDurationMs = 0)),
                base.channels,
            ).any { it.contains("send_duration_ms") },
        )
    }

    @Test
    fun `the encoded form is compact`() {
        // The device CRCs exactly these bytes (FR-27), so whitespace is not a
        // formatting preference -- pretty-printing would change the hash.
        val text = ConfigJson.encode(sampleConfig())
        assertTrue("encoded JSON must be single-line", !text.contains('\n'))
        assertTrue("encoded JSON must not be pretty-printed", !text.contains(": "))
    }

    /**
     * The four rules the app's validator used to be missing (open item N-40): it
     * checked the NUMERIC limits but not the ladder-geometry rules, so a config
     * carrying an out-of-range `mv_center`, an over-width name, overlapping windows
     * or a binding to a nonexistent button passed the app's local gate and was then
     * refused by the device at decode -- the failure the local gate exists to
     * prevent, reported as a nack naming a check rather than the field.
     */
    private fun problemsWithChannels(channels: List<ChannelConfig>): List<String> =
        // Bindings cleared: these tests exercise the CHANNEL rules, and the sample's
        // bindings name `vol_up`/`next`, which a replaced ladder no longer has -- so
        // keeping them would trip the (separately tested) dangling-binding rule and
        // confuse which check failed.
        ConfigJson.problems(sampleConfig().copy(channels = channels, bindings = emptyList()))

    private fun ladderOf(vararg buttons: LadderButton, idleMv: Int = 2835) =
        sampleConfig().channels[0].copy(
            ladder = LadderProfile(source = 0, learnedIdleMv = idleMv, buttons = buttons.toList())
        )

    @Test
    fun `an over-width channel name is refused`() {
        // `ReadStr` refuses `n >= kChannelNameLen`, so an over-width name encodes
        // fine and then fails to decode as "corrupt config" -- a round-trip
        // violation (FR-27).
        val atWidth = "N".repeat(K_CHANNEL_NAME_LEN)      // 16 -> refused
        assertTrue(
            "a name at the width is refused at decode",
            problemsWithChannels(listOf(ladderOf().copy(name = atWidth)))
                .any { it.contains("name") },
        )
        assertTrue(
            "a 15-char name is accepted",
            problemsWithChannels(listOf(ladderOf().copy(name = "N".repeat(K_CHANNEL_NAME_LEN - 1))))
                .isEmpty(),
        )
        assertTrue(
            "an empty name is refused",
            problemsWithChannels(listOf(ladderOf().copy(name = ""))).any { it.contains("name") },
        )
    }

    @Test
    fun `an over-width device id is refused`() {
        val c = sampleConfig().copy(deviceId = "d".repeat(K_DEVICE_ID_LEN))
        assertTrue(
            "a device_id at the width is refused at decode",
            ConfigJson.problems(c).any { it.contains("device_id") },
        )
    }

    @Test
    fun `a ladder centre outside the ADC ceiling is refused`() {
        // `LadderProfileIsValid`: a press pulls the input DOWN, so a centre at or
        // above the idle reference is physically impossible, and 0 is unreachable
        // from a learn. Bounds are against the ADC CEILING (2900), not the rail.
        assertTrue(
            "mv_center 0 is refused",
            problemsWithChannels(listOf(ladderOf(LadderButton("a", "A", 0, 120))))
                .any { it.contains("mv_center") },
        )
        // Above the ceiling: no pin reading can exceed it.
        assertTrue(
            "mv_center above the ADC ceiling is refused",
            problemsWithChannels(listOf(ladderOf(LadderButton("a", "A", K_ADC_CEILING_MV + 1, 120))))
                .any { it.contains("mv_center") },
        )
        // The boundary itself is legal.
        assertTrue(
            "mv_center exactly at the ceiling is accepted",
            problemsWithChannels(listOf(ladderOf(LadderButton("a", "A", K_ADC_CEILING_MV, 120))))
                .isEmpty(),
        )
    }

    @Test
    fun `a learned idle outside the plausible ADC range is refused`() {
        // `LadderProfileIsValid`: `(0, kAdcCeilingMv]`.
        assertTrue(
            "idle 0 is refused",
            problemsWithChannels(listOf(ladderOf(LadderButton("a", "A", 1000, 120), idleMv = 0)))
                .any { it.contains("learned_idle_mv") },
        )
        assertTrue(
            "idle above the ceiling is refused",
            problemsWithChannels(
                listOf(ladderOf(LadderButton("a", "A", 1000, 120), idleMv = K_ADC_CEILING_MV + 1))
            ).any { it.contains("learned_idle_mv") },
        )
    }

    @Test
    fun `a tolerance that rounds to zero permille is refused`() {
        // The DERIVED window must be real: `LadderRatioPermille(tolerance, idle)`
        // must be positive. A tolerance of 1 mV against a large idle rounds to 0.
        assertTrue(
            "a sub-permille tolerance forms no window",
            problemsWithChannels(
                listOf(ladderOf(LadderButton("a", "A", 1000, 1), idleMv = 2835))
            ).any { it.contains("too small") },
        )
        // **The rounding boundary, which is where this app's copy must agree with
        // the firmware's EXACTLY.** `LadderRatioPermille` does rounded division
        // (`+ idle/2`); truncating it instead gives a different answer here: a 2 mV
        // tolerance against a 2835 mV idle is `2000+1417)/2835 = 1` permille ROUNDED
        // but `2000/2835 = 0` truncated -- so a truncating copy refuses a window the
        // device accepts, and the app would block a legal learned profile. Verified
        // by mutation: making this predicate truncate survives every other case.
        assertTrue(
            "a 2 mV tolerance against a 2835 mV idle is 1 permille rounded, so it " +
                "must be accepted: ${problemsWithChannels(
                    listOf(ladderOf(LadderButton("a", "A", 1000, 2), idleMv = 2835))
                )}",
            problemsWithChannels(
                listOf(ladderOf(LadderButton("a", "A", 1000, 2), idleMv = 2835))
            ).isEmpty(),
        )
    }

    @Test
    fun `overlapping windows are refused`() {
        // `LadderWindowsAreDistinguishable`: two centres closer than the wider
        // tolerance mean a press could match both, and the device refuses the
        // profile rather than firing whichever it tests first.
        val overlapping = listOf(
            ladderOf(
                LadderButton("a", "A", 1430, 200),
                LadderButton("b", "B", 1500, 200),   // 70 mV apart, 200 tolerance
            )
        )
        assertTrue(
            "windows that overlap must be refused",
            problemsWithChannels(overlapping).any { it.contains("overlap") },
        )
        // Far enough apart is fine.
        val separated = listOf(
            ladderOf(
                LadderButton("a", "A", 1430, 120),
                LadderButton("b", "B", 2145, 110),
            )
        )
        assertTrue(
            "well-separated windows are accepted",
            problemsWithChannels(separated).isEmpty(),
        )
    }

    @Test
    fun `a binding to a button on no channel is refused`() {
        // `BindingNamesARealInput`: a binding that names an id on no ladder and no
        // AUX input is a binding to nothing, which the device refuses.
        val base = sampleConfig()
        val dangling = base.copy(
            bindings = base.bindings + Binding(
                "b5", BindingChannel.SWC1, "nonexistent", Gesture.SINGLE, true,
                listOf(Action(ActionKind.OUT_RELEASE)),
            )
        )
        assertTrue(
            "a binding naming a button that exists nowhere must be refused",
            ConfigJson.problems(dangling).any { it.contains("not a button") },
        )
        // A binding naming an AUX input IS real.
        val auxBinding = base.copy(
            bindings = base.bindings + Binding(
                "b6", BindingChannel.AUX2, "aux1", Gesture.SINGLE, true,
                listOf(Action(ActionKind.OUT_RELEASE)),
            )
        )
        assertTrue(
            "an AUX binding must not be refused as dangling: ${ConfigJson.problems(auxBinding)}",
            ConfigJson.problems(auxBinding).none { it.contains("not a button") },
        )
        // "NONE" (the programming button) is a legal target too.
        val noneBinding = base.copy(
            bindings = base.bindings + Binding(
                "b7", BindingChannel.ANY, "NONE", Gesture.LONG, true,
                listOf(Action(ActionKind.OUT_RELEASE)),
            )
        )
        assertTrue(
            "'NONE' is a legal button target",
            ConfigJson.problems(noneBinding).none { it.contains("not a button") },
        )
    }

    @Test
    fun `a button count above the ladder maximum is refused`() {
        val many = (0 until K_LADDER_MAX_BUTTONS + 1).map {
            LadderButton("b$it", "B$it", 100 + it * 150, 50)
        }
        assertTrue(
            "more buttons than the firmware array holds must be refused",
            problemsWithChannels(listOf(ladderOf(*many.toTypedArray())))
                .any { it.contains("at most") },
        )
    }
}

class GainModeRoundTripTest {

    @Test
    fun `a channel that defers its gain survives a round trip as AUTO`() {
        // The firmware accepts a channel `gain_mode` of "AUTO" -- the spec's own
        // worked example uses it -- and this enum did not, so the decoder's
        // `firstOrNull` fell back to TRACKING and the next save wrote a concrete
        // gain the user never chose, silently overriding their policy. An unknown
        // enum member in a round-trip codec is data loss, not a cosmetic gap.
        val c = sampleConfig().let {
            it.copy(channels = it.channels.map { ch ->
                ch.copy(output = ch.output.copy(gainMode = GainMode.AUTO))
            })
        }
        val decoded = ConfigJson.decode(ConfigJson.encode(c))
        assertEquals(GainMode.AUTO, decoded.channels[0].output.gainMode)
    }

    @Test
    fun `every gain mode is distinguishable on the wire`() {
        // If two values encoded to the same name, a deferring channel and a forced
        // one would decode alike -- the defect in its quietest form.
        val names = GainMode.entries.map { it.wireName }
        assertEquals(names.size, names.toSet().size)
        assertEquals(setOf("TRACKING", "AMPLIFIED", "AUTO"), names.toSet())
    }
}
