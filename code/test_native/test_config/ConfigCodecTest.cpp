#include "Config/ConfigCodec.h"
#include <gtest/gtest.h>
#include "ConfigFixtures.h"

using swctest::MakeConfig;
#include <cstring>
#include <string>

namespace {
// The scratch every test here encodes into. It MUST be sized from
// ConfigMaxSerializedSize() and not guessed: the model's JSON form is 22,407 B
// at the worst case, so the `char buf[4096]` an earlier revision used overflowed
// on every single test -- ConfigEncodeJson returns 0, the first ASSERT_GT(n, 0u)
// fires, and the suite fails for a reason that has nothing to do with the codec
// under test. A hard-coded literal also cannot notice the model growing; this
// can, and ConfigMaxSerializedSize() is constexpr precisely so it can be used
// here.
//
// static_assert, not a runtime check: if ConfigMaxSerializedSize() ever
// under-reports the true worst case, every buffer here is silently too small
// again, and the size test below would be asserting the same wrong number. The
// assert is what ties the test's memory to the model.
constexpr size_t kScratch = ConfigMaxSerializedSize();
static_assert(kScratch > 0u, "ConfigMaxSerializedSize must be a real bound");
// The size test asserts this same number fits two NVS slots; if it stops doing
// so, that test fails -- but only after this one has already allocated. Keeping
// the two together is what makes the failure legible.
static_assert(kScratch < 100000u, "a bound this large is a bug, not a config");

}  // namespace

TEST(ConfigCodec, JsonRoundTripsEveryFieldThatWasSet) {
    const Config in = MakeConfig();
    char buf[kScratch] = {};
    const size_t n = ConfigEncodeJson(in, buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    Config out{};
    ASSERT_TRUE(ConfigDecodeJson(buf, n, &out));
    // Every field MakeConfig sets is checked here. A field the encoder silently
    // drops would otherwise pass: JsonRoundTripIsStableUnderReencode only proves
    // the encoder is deterministic, not complete -- a dropped field is stable.
    EXPECT_EQ(out.schema_version, in.schema_version);
    EXPECT_STREQ(out.device_id, in.device_id);
    EXPECT_EQ(out.settings.timings.debounce_ms, in.settings.timings.debounce_ms);
    // The `u` suffixes are not cosmetic. `GestureTimings`'s fields are uint32_t
    // and these are the only two assertions in the file with a bare literal on
    // the right, so they are the only two that instantiate gtest's
    // CmpHelperEQ<uint32_t, int> -- which is `-Wsign-compare` inside
    // gtest.h:1394, and the native env builds with -Wall -Wextra -Werror. Without
    // the suffix this file does not compile on the host, and the error points
    // into gtest rather than at these lines.
    EXPECT_EQ(out.settings.timings.double_press_off_ms, 500u);
    EXPECT_EQ(out.settings.timings.long_press_ms, 750u);
    EXPECT_EQ(out.settings.timings.send_duration_ms, in.settings.timings.send_duration_ms);
    EXPECT_EQ(out.settings.gain_policy, GainPolicy::kAuto);
    EXPECT_EQ(out.settings.buzzer_level, 2);
    EXPECT_EQ(out.settings.led_level, 2);
    EXPECT_TRUE(out.settings.temp_comp_enabled);
    // The per-press click. The fixture sets it TRUE: the shipping default is FALSE,
    // so a field left at the default would pass whether or not it round-tripped.
    EXPECT_TRUE(out.settings.key_click_enabled);
    EXPECT_EQ(out.settings.maintenance_timeout_ms, in.settings.maintenance_timeout_ms);
    // FR-33's next-boot trigger. The fixture sets it TRUE precisely so this
    // assertion can fail: an encoder that dropped the field would decode it back
    // as the `false` default and pass a fixture left at false.
    EXPECT_TRUE(out.settings.maintenance_on_boot);

    EXPECT_EQ(out.channel_count, 1);
    EXPECT_TRUE(out.channels[0].enabled);
    EXPECT_STREQ(out.channels[0].name, "SWC1");
    EXPECT_EQ(out.channels[0].ladder.learned_idle_mv, 2835);
    EXPECT_EQ(out.channels[0].ladder.count, 1);
    EXPECT_EQ(out.channels[0].ladder.buttons[0].id, std::string("VOL_UP"));
    EXPECT_EQ(out.channels[0].ladder.buttons[0].mv_center, 1430);
    EXPECT_EQ(out.channels[0].ladder.buttons[0].mv_tolerance, 120);
    EXPECT_EQ(out.channels[0].output.gain_mode, GainMode::kAmplified);
    EXPECT_EQ(out.channels[0].output.idle_dac_code, 4095);

    // Bindings are top-level and carry an ordered action list.
    ASSERT_EQ(out.binding_count, 2);
    EXPECT_STREQ(out.bindings[0].id, "b1");
    EXPECT_EQ(out.bindings[0].channel, static_cast<uint8_t>(BindingChannel::kSwc1));
    EXPECT_STREQ(out.bindings[0].button, "VOL_UP");
    EXPECT_EQ(out.bindings[0].gesture, Gesture::kSingle);
    EXPECT_TRUE(out.bindings[0].enabled);
    ASSERT_EQ(out.bindings[0].action_count, 1);
    EXPECT_EQ(out.bindings[0].actions[0].kind, ActionKind::kOutVoltage);
    EXPECT_EQ(out.bindings[0].actions[0].key_mv, 2400);

    // The two-action binding is the product's core case; both must survive, in
    // order, with the payload intact.
    EXPECT_EQ(out.bindings[1].gesture, Gesture::kDouble);
    ASSERT_EQ(out.bindings[1].action_count, 2);
    EXPECT_EQ(out.bindings[1].actions[0].kind, ActionKind::kOutRelease);
    EXPECT_EQ(out.bindings[1].actions[1].kind, ActionKind::kAppIntent);
    EXPECT_STREQ(out.bindings[1].actions[1].target, "com.oetsolutions.swc.ACTION_NAVIGATE");
    EXPECT_STREQ(out.bindings[1].actions[1].payload, "geo:40.7608,-111.8910");
}

TEST(ConfigCodec, JsonRoundTripIsStableUnderReencode) {
    const Config in = MakeConfig();
    char a[kScratch] = {};
    char b[kScratch] = {};
    Config mid{};
    ASSERT_TRUE(ConfigDecodeJson(a, ConfigEncodeJson(in, a, sizeof(a)), &mid));
    const size_t nb = ConfigEncodeJson(mid, b, sizeof(b));
    EXPECT_EQ(std::string(a), std::string(b, nb));
}

TEST(ConfigCodec, AConfigWrittenBeforeFR33sTriggerStillDecodes) {
    // The on-disk format is JSON and carries no schema bump for FR-33's added
    // `maintenance_on_boot`, so a config written by an older firmware has no such
    // field -- and every device in the field has one. The decoder must treat an
    // ABSENT field as `false` (the default) rather than refusing the whole config,
    // or an upgrade would strand every already-configured device with "no config".
    //
    // The other half matters just as much: a field that is PRESENT but not a bool
    // must FAIL the decode rather than defaulting to false, because a silent
    // default here is the "wrong value reported as a right one" shape this codec
    // refuses everywhere else.
    const Config in = MakeConfig();
    char buf[kScratch] = {};
    const size_t n = ConfigEncodeJson(in, buf, sizeof(buf));
    std::string s(buf, n);

    // Strip the field exactly as an older encoder would have omitted it.
    const std::string field = ",\"maintenance_on_boot\":true";
    const size_t pos = s.find(field);
    ASSERT_NE(pos, std::string::npos) << "the encoder must emit the field for this test to mean anything";
    s.erase(pos, field.size());

    Config out{};
    ASSERT_TRUE(ConfigDecodeJson(s.c_str(), s.size(), &out))
        << "a config from an older firmware must still decode";
    EXPECT_FALSE(out.settings.maintenance_on_boot)
        << "an absent field means the default, which is OFF";

    // Present-but-malformed is a hard failure, not a silent default.
    std::string bad_json(buf, n);
    const size_t bpos = bad_json.find(field);
    ASSERT_NE(bpos, std::string::npos);
    bad_json.replace(bpos, field.size(), ",\"maintenance_on_boot\":\"yes\"");
    Config bad{};
    EXPECT_FALSE(ConfigDecodeJson(bad_json.c_str(), bad_json.size(), &bad))
        << "a non-bool must fail the decode, not silently become false";
}

TEST(ConfigCodec, MalformedJsonIsRejectedNotPartiallyApplied) {
    Config out{};
    EXPECT_FALSE(ConfigDecodeJson("{ this is not json", 18, &out));
    EXPECT_FALSE(ConfigDecodeJson("{}", 2, &out)) << "missing required fields";
    EXPECT_FALSE(ConfigDecodeJson("", 0, &out));
}

TEST(ConfigCodec, ANewerSchemaVersionIsRefused) {
    const Config in = MakeConfig();
    char buf[kScratch] = {};
    size_t n = ConfigEncodeJson(in, buf, sizeof(buf));
    std::string s(buf, n);
    const std::string from = "\"schema_version\":1";
    const size_t pos = s.find(from);
    ASSERT_NE(pos, std::string::npos);
    s.replace(pos, from.size(), "\"schema_version\":99");
    Config out{};
    EXPECT_FALSE(ConfigDecodeJson(s.c_str(), s.size(), &out))
        << "a future schema must be refused, not misparsed";
}

TEST(ConfigCodec, BlobHasAHeaderAndDetectsATruncatedPayload) {
    const Config in = MakeConfig();
    uint8_t blob[kScratch] = {};
    const size_t n = ConfigEncodeBlob(in, blob, sizeof(blob));
    ASSERT_GT(n, 0u);

    Config out{};
    EXPECT_TRUE(ConfigDecodeBlob(blob, n, &out));
    EXPECT_FALSE(ConfigDecodeBlob(blob, n - 1, &out)) << "a short read must not decode";
    EXPECT_FALSE(ConfigDecodeBlob(blob, 4, &out));
}

TEST(ConfigCodec, BlobDetectsASingleFlippedBitViaCrc) {
    const Config in = MakeConfig();
    uint8_t blob[kScratch] = {};
    const size_t n = ConfigEncodeBlob(in, blob, sizeof(blob));
    ASSERT_GT(n, 8u);
    blob[n / 2] ^= 0x01;
    Config out{};
    EXPECT_FALSE(ConfigDecodeBlob(blob, n, &out));
}

TEST(ConfigCodec, ValidationRejectsInconsistentConfigs) {
    Config c = MakeConfig();
    EXPECT_TRUE(ConfigValidate(c));

    c = MakeConfig();
    c.channel_count = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "at least one channel must exist";

    c = MakeConfig();
    c.channels[0].ladder.count = 1;
    // Above the ADC ceiling: spec 3.2 caps a pin reading at 2900 mV, so a
    // mv_center above it is not a value the hardware can produce.
    c.channels[0].ladder.buttons[0].mv_center = 3300;
    EXPECT_FALSE(ConfigValidate(c)) << "a pin voltage above the ADC ceiling is impossible";

    c = MakeConfig();
    // A binding naming a button that does not exist on its channel.
    std::strncpy(c.bindings[0].button, "NO_SUCH_BUTTON", sizeof(c.bindings[0].button) - 1);
    EXPECT_FALSE(ConfigValidate(c)) << "a binding to a non-existent button";

    c = MakeConfig();
    c.binding_count = static_cast<uint8_t>(kMaxBindings + 1);
    EXPECT_FALSE(ConfigValidate(c)) << "more bindings than the budget allows";

    // Every OTHER count in a Config is checked against its array before use --
    // channel_count, ladder.count, binding_count, action_count -- because a count
    // past its array makes the next read run off the end. `aux_count` is the one
    // that is not, and it is read the same way: BindingNamesARealInput loops
    // `i < c.aux_count` over `c.aux[i]`. An out-of-range count is a read past the
    // 3-entry array, in the validation function that exists to catch exactly this.
    c = MakeConfig();
    c.aux_count = static_cast<uint8_t>(kMaxAuxButtons + 1);
    EXPECT_FALSE(ConfigValidate(c)) << "more AUX buttons than the array holds";

    c = MakeConfig();
    c.bindings[1].action_count = static_cast<uint8_t>(kMaxActionsPerBinding + 1);
    EXPECT_FALSE(ConfigValidate(c)) << "more actions per binding than the budget allows";

    c = MakeConfig();
    // An empty action list is LEGAL and means "swallow the gesture" (spec 3.5);
    // it must not be confused with enabled:false, which is also legal.
    c.bindings[1].action_count = 0;
    EXPECT_TRUE(ConfigValidate(c)) << "empty actions swallow the gesture; that is valid";
    c.bindings[1].enabled = false;
    EXPECT_TRUE(ConfigValidate(c)) << "disabled is a different valid state";

    c = MakeConfig();
    c.settings.timings.debounce_ms = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "zero debounce is not a configurable choice";

    // The maintenance window is the one settings scalar with no coherence rule of
    // its own, and both of its degenerate ends defeat the FR-38 guarantee. A zero
    // makes `now - last_activity >= timeout` true on the opening tick, so the
    // window opens and closes at once -- the feature appears to work and does
    // nothing. A `uint32` maximum (~49.7 days) is "never closes", which is the
    // device-left-unable-to-serve state FR-38 exists to prevent. Both are refused,
    // like `debounce_ms` above and `send_duration_ms` beside it.
    c = MakeConfig();
    c.settings.maintenance_timeout_ms = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "a zero timeout closes the window immediately";
    c.settings.maintenance_timeout_ms = kMaintenanceTimeoutMaxMs + 1;
    EXPECT_FALSE(ConfigValidate(c)) << "a timeout past the ceiling never closes on its own";
    c.settings.maintenance_timeout_ms = kMaintenanceTimeoutMaxMs;
    EXPECT_TRUE(ConfigValidate(c)) << "the ceiling itself is a legal window";

    c = MakeConfig();
    c.settings.timings.long_press_ms = 100;
    c.settings.timings.double_press_off_ms = 500;
    EXPECT_FALSE(ConfigValidate(c)) << "long press must exceed the double window";

    c = MakeConfig();
    c.channels[0].ladder.buttons[0].mv_tolerance = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "a zero-width window can never match";
}

TEST(ConfigCodec, ValidationRejectsButtonsTooCloseToTellApart) {
    Config c = MakeConfig();
    c.channels[0].ladder.count = 2;
    // 1418 and 1445 mV are 27 mV apart at a 120 mV half-width: every reading in
    // the overlap is equally close to both, so classification would be a coin
    // toss. (In permille of the 2835 idle: 500 and 510, each window 42 wide.)
    c.channels[0].ladder.buttons[0] = {"VOL_UP",   "Volume Up",   1418, 120, 3300, 235, 200, 98};
    c.channels[0].ladder.buttons[1] = {"VOL_DOWN", "Volume Down", 1445, 120, 3300, 235, 200, 97};
    EXPECT_FALSE(ConfigValidate(c))
        << "centres closer together than the wider tolerance can never be told apart";
}

TEST(ConfigCodec, ValidationAcceptsButtonsExactlyTolerancePlusOneApart) {
    Config c = MakeConfig();
    c.channels[0].ladder.count = 2;
    // The check is on the DERIVED permille window, so the boundary is expressed
    // in millivolts that land on it: 1430 mV (504 permille) and 1551 mV
    // (547 permille) are 43 permille apart = max(42,42) + 1.
    c.channels[0].ladder.buttons[0] = {"VOL_UP",   "Volume Up",   1430, 120, 3300, 235, 200, 98};
    c.channels[0].ladder.buttons[1] = {"VOL_DOWN", "Volume Down", 1551, 120, 3300, 235, 200, 97};
    EXPECT_TRUE(ConfigValidate(c));

    c.channels[0].ladder.buttons[1] = {"VOL_DOWN", "Volume Down", 1548, 120, 3300, 235, 200, 97};
    EXPECT_FALSE(ConfigValidate(c)) << "exactly tolerance apart still overlaps";
}

TEST(ConfigCodec, SerializedSizeFitsTheNvsPartitionBudget) {
    // Spec 10.5, two limits -- and the second is the one that is easy to miss.
    //
    // (a) A single NVS *value* is capped at ENTRY_SIZE * (ENTRY_COUNT - 1)
    //     = 32 * 125 = 4000 bytes. That is a hard IDF limit (nvs_page.cpp
    //     returns ESP_ERR_NVS_VALUE_TOO_LONG above it), not a budget to tune.
    //     Even a REALISTIC config (~3.9 KB) sits on that line, and a moderate
    //     one (~7.9 KB) blows past it -- so a single-value slot is not a
    //     hypothetical failure, it is the common case.
    EXPECT_GT(ConfigMaxSerializedSize(), 4000u)
        << "the worst case must force chunking, or Task 9's chunked path is dead code";

    // (b) BOTH slots, plus the sequence key, must fit the partition's usable
    //     entry space: 12 pages x 126 entries x 32 B = 48,384 B.
    //
    //     Each chunk key costs 2112 B, NOT 2048. NVS writes a 32-byte metadata
    //     entry plus the payload (nvs_page.cpp:185), and then a separate
    //     32-byte BLOB_IDX entry for the key (nvs_storage.cpp:353). Counting
    //     only the payload understates the budget.
    //
    //     NOTE this is deliberately NOT the weaker "a slot fits in 24 KB"
    //     assertion an earlier revision had: two 24 KB slots need 49,152 B of
    //     entry space and DO NOT FIT a 48,384-byte partition. Asserting that
    //     bound would pass on a config the device cannot actually store twice.
    constexpr size_t kUsableEntryBytes    = 32u * 126u * 12u;              // 48,384
    constexpr size_t kEntryBytesPerChunk  = 32u + kConfigChunkBytes + 32u; // 2,112
    constexpr size_t kSequenceKeyBytes    = 32u;
    const size_t chunks =
        static_cast<size_t>(ConfigChunkCountFor(ConfigMaxSerializedSize()));
    EXPECT_LE(2u * chunks * kEntryBytesPerChunk + kSequenceKeyBytes, kUsableEntryBytes)
        << "two slots + cfg_seq must fit the partition, with entry overhead counted";
}

TEST(ConfigCodec, AChannelMayDeferItsGainModeToTheDevicePolicy) {
    // Spec 6.2: gain comes from `settings.gain_policy`, and the spec's own worked
    // example sets a CHANNEL's `gain_mode` to "AUTO". The codec had no "AUTO" in
    // its channel name table, so it REJECTED that config -- the spec's own example
    // was undecodable, and because every decodable channel named a concrete mode,
    // `settings.gain_policy` was never consulted and FR-14's AUTO rule was
    // unreachable.
    Config in = MakeConfig();
    in.channels[0].output.gain_mode = GainMode::kAuto;
    char buf[8192];
    const size_t n = ConfigEncodeJson(in, buf, sizeof(buf));
    ASSERT_GT(n, 0u);
    const std::string json(buf, n);

    Config out{};
    ASSERT_TRUE(ConfigDecodeJson(buf, n, &out))
        << "a channel that defers to the device policy must decode";
    EXPECT_EQ(out.channels[0].output.gain_mode, GainMode::kAuto);
    // The device-wide policy is the thing it defers TO, so it must survive too.
    EXPECT_EQ(out.settings.gain_policy, GainPolicy::kAuto);

    // And it round-trips as the word "AUTO", not as a silent default.
    EXPECT_NE(json.find("\"gain_mode\":\"AUTO\""), std::string::npos) << json;
}

TEST(ConfigCodec, EveryGainModeRoundTripsByItsOwnName) {
    // The three channel values must be distinguishable on the wire, or a deferring
    // channel and a forced one would decode to the same thing -- which is the
    // defect in its quietest form.
    char buf[8192];
    for (const auto mode : {GainMode::kTracking, GainMode::kAmplified, GainMode::kAuto}) {
        Config in = MakeConfig();
        in.channels[0].output.gain_mode = mode;
        const size_t n = ConfigEncodeJson(in, buf, sizeof(buf));
        ASSERT_GT(n, 0u);
        Config out{};
        ASSERT_TRUE(ConfigDecodeJson(buf, n, &out));
        EXPECT_EQ(out.channels[0].output.gain_mode, mode);
    }
}

TEST(ConfigCodec, AnIntegerFieldCarryingAFractionIsRefusedNotTruncated) {
    // Truncating 750.9 to 750 is a config the device accepts and then behaves
    // differently from what was sent -- the same wrong-value-accepted class as
    // the uint32 wrap ReadU32 already guards against, one size smaller. The
    // `config_patch` path refuses a fractional integer field; if the codec
    // truncated it, the two paths would give two answers to one question.
    const Config in = MakeConfig();
    char a[kScratch] = {};
    ASSERT_GT(ConfigEncodeJson(in, a, sizeof(a)), 0u);
    std::string s(a);
    const size_t at = s.find("\"long_press_ms\":");
    ASSERT_NE(at, std::string::npos) << "the fixture must set this field";
    const size_t num = s.find_first_of("0123456789", at);
    const size_t end = s.find_first_not_of("0123456789", num);
    s.replace(num, end - num, "750.9");  // a legal-looking value, one fraction over
    Config out{};
    EXPECT_FALSE(ConfigDecodeJson(s.c_str(), s.size(), &out))
        << "a fractional integer field must be refused";

    // And the integer form of the same field still decodes (the bound is not so
    // tight it refuses a legal value).
    std::string t(a);
    t.replace(num, end - num, "751");
    EXPECT_TRUE(ConfigDecodeJson(t.c_str(), t.size(), &out));
    EXPECT_EQ(out.settings.timings.long_press_ms, 751u);
}

TEST(ConfigCodec, ValidationRefusesASendDurationThatWouldPinTheKeyLine) {
    // `send_duration_ms` is how long the KEY line is DRIVEN (FR-15), so its upper
    // end is not a style choice -- a `uint32` maximum (~49.7 days) is a phantom
    // key press the user cannot end, the exact hazard FR-39 exists to prevent. It
    // was the one timing scalar bounded only by its WIDTH: `ReadU32` refuses a
    // value past u32, but the u32 maximum itself passed, and so did a patch of
    // `1e10` that `NumToU32` had already wrapped into range.
    Config c = MakeConfig();
    c.settings.timings.send_duration_ms = 0;
    EXPECT_FALSE(ConfigValidate(c)) << "a zero hold is not a configurable choice";
    c.settings.timings.send_duration_ms = kSendDurationMaxMs + 1;
    EXPECT_FALSE(ConfigValidate(c)) << "a hold past the ceiling pins the key line";
    c.settings.timings.send_duration_ms = kSendDurationMaxMs;
    EXPECT_TRUE(ConfigValidate(c)) << "the ceiling itself is a legal hold";
}
