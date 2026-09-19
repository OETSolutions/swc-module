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
    EXPECT_EQ(out.settings.maintenance_timeout_ms, in.settings.maintenance_timeout_ms);

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
