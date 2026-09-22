#include "Link/Ndjson.h"
#include <gtest/gtest.h>
#include <cstring>
#include <string>

TEST(NdjsonReader, ADoubleNewlineTerminatedLineIsComplete) {
    NdjsonReader r;
    const char *line = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}\n";
    NdjsonResult res = NdjsonResult::kNeedMore;
    for (const char *p = line; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kComplete);
    EXPECT_STREQ(r.Line(), "{\"v\":1,\"seq\":1,\"type\":\"ping\"}");
}

TEST(NdjsonReader, CarriageReturnIsToleratedBeforeTheNewline) {
    NdjsonReader r;
    const char *line = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}\r\n";
    NdjsonResult res = NdjsonResult::kNeedMore;
    for (const char *p = line; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kComplete);
    EXPECT_STREQ(r.Line(), "{\"v\":1,\"seq\":1,\"type\":\"ping\"}");
}

TEST(NdjsonReader, AnOversizedFrameIsRejectedRatherThanTruncated) {
    NdjsonReader r;
    NdjsonResult res = NdjsonResult::kNeedMore;
    for (size_t i = 0; i < kNdjsonMaxFrame + 100; ++i) res = r.Push('x');
    EXPECT_EQ(res, NdjsonResult::kTooLong);
    // The reader must recover: the next complete frame parses normally.
    r.Consume();
    const char *line = "{\"v\":1,\"seq\":2,\"type\":\"ping\"}\n";
    for (const char *p = line; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kComplete);
}

TEST(NdjsonReader, ConsecutiveFramesParseIndependently) {
    NdjsonReader r;
    const char *a = "{\"v\":1,\"seq\":1,\"type\":\"a\"}\n";
    const char *b = "{\"v\":1,\"seq\":2,\"type\":\"b\"}\n";
    for (const char *p = a; *p; ++p) r.Push(static_cast<uint8_t>(*p));
    ASSERT_STREQ(r.Line(), "{\"v\":1,\"seq\":1,\"type\":\"a\"}");
    r.Consume();
    NdjsonResult res = NdjsonResult::kNeedMore;
    for (const char *p = b; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kComplete);
    EXPECT_STREQ(r.Line(), "{\"v\":1,\"seq\":2,\"type\":\"b\"}");
}

TEST(NdjsonEnvelope, ParsesVersionSequenceAndType) {
    FrameHeader h{};
    ASSERT_TRUE(NdjsonParseEnvelope("{\"v\":1,\"seq\":42,\"type\":\"event\"}", &h));
    EXPECT_EQ(h.v, 1);
    EXPECT_EQ(h.seq, 42u);
    EXPECT_STREQ(h.type, "event");
}

TEST(NdjsonEnvelope, RejectsAMissingEnvelopeField) {
    FrameHeader h{};
    EXPECT_FALSE(NdjsonParseEnvelope("{\"seq\":1,\"type\":\"event\"}", &h));
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":1,\"type\":\"event\"}", &h));
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":1,\"seq\":1}", &h));
}

TEST(NdjsonWriter, EmitsOneLineWithTheEnvelopeAndEscapesQuotesInStrings) {
    NdjsonWriter w;
    w.Write("nack", 7, "\"error\":\"bad \\\"value\\\"\"");
    const size_t n = w.LineLen();
    ASSERT_GT(n, 0u);
    EXPECT_EQ(w.Line()[n - 1], '\n');

    FrameHeader h{};
    std::string line(w.Line(), n - 1);
    ASSERT_TRUE(NdjsonParseEnvelope(line.c_str(), &h));
    EXPECT_STREQ(h.type, "nack");
    EXPECT_EQ(h.seq, 7u);
}

TEST(NdjsonWriter, RefusesToEmitAFrameThatWouldExceedTheMaximum) {
    NdjsonWriter w;
    std::string big(4096, 'x');
    w.Write("event", 1, big.c_str());
    // The writer must not silently produce an unparseable line; an oversized
    // body becomes an error frame instead.
    FrameHeader h{};
    std::string line(w.Line(), w.LineLen() ? w.LineLen() - 1 : 0);
    ASSERT_TRUE(NdjsonParseEnvelope(line.c_str(), &h));
    EXPECT_STREQ(h.type, "error");
    EXPECT_EQ(h.seq, 1u);
    // The fallback frame carries the SAME envelope as every other frame. This
    // branch spelled the version as a literal `1` while its siblings used
    // kNdjsonProtocolVersion, and the peer dispatch (CommandRouter.cpp:429)
    // rejects an envelope whose `v` differs -- so a protocol bump that missed
    // this copy would break exactly the frame that exists to keep the link
    // recoverable. Tying the assertion to the constant, not to `1`, is what
    // makes a divergence fail here instead of on the wire.
    EXPECT_EQ(h.v, kNdjsonProtocolVersion);
}

// A body-less frame is the common case for acks and pings. It is worth its own
// test because the naive writer emits `...,"type":"ack",}` -- valid-looking to
// a string check, and not JSON.
TEST(NdjsonWriter, EmitsAValidLineWhenThereAreNoBodyFields) {
    const char *bodies[] = {nullptr, "", "{}", "null"};
    for (const char *body : bodies) {
        NdjsonWriter w;
        w.Write("ack", 3, body);
        const size_t n = w.LineLen();
        ASSERT_GT(n, 0u);
        EXPECT_EQ(w.Line()[n - 1], '\n');

        FrameHeader h{};
        std::string line(w.Line(), n - 1);
        ASSERT_TRUE(NdjsonParseEnvelope(line.c_str(), &h)) << "body = " << (body ? body : "(null)");
        EXPECT_STREQ(h.type, "ack");
        EXPECT_EQ(h.seq, 3u);
    }
}

// The reader must resynchronize even when the caller never calls Consume(), or
// an overrun line desynchronizes the link until the next explicit reset.
TEST(NdjsonReader, RecoversFromAnOverrunWithoutAnExplicitConsume) {
    NdjsonReader r;
    NdjsonResult res = NdjsonResult::kNeedMore;
    for (size_t i = 0; i < kNdjsonMaxFrame + 100; ++i) res = r.Push('x');
    ASSERT_EQ(res, NdjsonResult::kTooLong);

    // The tail of the overrun line, then a good frame. The tail must be
    // discarded, not handed up as a frame.
    const char *tail = "garbage\n";
    for (const char *p = tail; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kTooLong);

    const char *line = "{\"v\":1,\"seq\":9,\"type\":\"ping\"}\n";
    for (const char *p = line; *p; ++p) res = r.Push(static_cast<uint8_t>(*p));
    EXPECT_EQ(res, NdjsonResult::kComplete);
    EXPECT_STREQ(r.Line(), "{\"v\":1,\"seq\":9,\"type\":\"ping\"}");
}

TEST(NdjsonEnvelope, RejectsAnOutOfRangeOrFractionalVersionAndSequence) {
    // `(uint8_t)256.0` and `(uint32_t)-1.0` are silent corruptions, and so is a
    // fraction: `seq = 1.9` casts to 1, so the frame is acked as sequence 1 and
    // the device's bookkeeping silently disagrees with the peer's -- the same
    // "renumber the peer's frames" failure that making `seq` REQUIRED exists to
    // avoid.
    FrameHeader h{};
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":256,\"seq\":1,\"type\":\"event\"}", &h));
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":-1,\"seq\":1,\"type\":\"event\"}", &h));
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":1.9,\"seq\":1,\"type\":\"event\"}", &h));
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":1,\"seq\":-1,\"type\":\"event\"}", &h));
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":1,\"seq\":1.9,\"type\":\"event\"}", &h));
    EXPECT_FALSE(NdjsonParseEnvelope("{\"v\":1,\"seq\":4294967296,\"type\":\"event\"}", &h));

    // The boundary integers still parse (the bound is not over-tight).
    ASSERT_TRUE(NdjsonParseEnvelope("{\"v\":255,\"seq\":4294967295,\"type\":\"event\"}", &h));
    EXPECT_EQ(h.v, 255);
    EXPECT_EQ(h.seq, 4294967295u);
}

/*
 * cJSON's recursion depth is bounded for the DEVICE's stack, not a desktop one.
 *
 * This is the one test of `CJSON_NESTING_LIMIT`, which `platformio.ini` sets to
 * 32 in `[env]` so BOTH builds carry the same value. The overflow it prevents is
 * a device-only hazard -- `app_main` gets CONFIG_ESP_MAIN_TASK_STACK_SIZE = 3584
 * B and cJSON needs ~64 B per nesting level there, so the default limit of 1000
 * would need ~64 KB and a single hostile line took the poll task down. The host
 * has 8 MB and cannot reproduce the crash; what it CAN do is pin the bound, which
 * is the same cJSON source and so the same behaviour the target runs.
 *
 * The length matters: this is NOT the frame cap. `kNdjsonMaxFrame` is 1024 and a
 * 1000-byte line of `[` is comfortably inside it, which is exactly why a byte
 * limit could not substitute for a depth limit.
 */
TEST(NdjsonEnvelope, RejectsALineNestedDeeperThanTheDeviceStackAllows) {
    FrameHeader h{};

    // The nesting must be WELL-FORMED, and that is the whole subtlety of this
    // test. A run of bare `[` is rejected by cJSON as malformed at ANY limit --
    // so a test built from unbalanced brackets passes whether or not the device
    // bound exists, which is a mutation test surviving. Balanced brackets are the
    // only input where DEPTH alone is what cJSON objects to.
    auto balanced = [](int depth) {
        std::string s = "{\"v\":1,\"seq\":1,\"type\":\"ping\",\"x\":";
        for (int i = 0; i < depth; ++i) s += '[';
        for (int i = 0; i < depth; ++i) s += ']';
        return s + "}";
    };

    // 200 deep is well-formed, under the byte cap, and still refused: the nesting
    // is the only thing wrong with it.
    const std::string deep = balanced(200);
    EXPECT_FALSE(NdjsonParseEnvelope(deep.c_str(), &h))
        << "a well-formed line nested 200 deep must be refused, not parsed";
    EXPECT_LT(deep.size(), kNdjsonMaxFrame)
        << "precondition: the rejection is about DEPTH, not the byte cap";

    // The bound is not over-tight, in both directions: a flat frame parses, and
    // so does the deepest shape the app can actually send -- the config, whose
    // real nesting is depth 6.
    ASSERT_TRUE(NdjsonParseEnvelope("{\"v\":1,\"seq\":7,\"type\":\"ping\"}", &h));
    EXPECT_EQ(h.seq, 7u);
    ASSERT_TRUE(NdjsonParseEnvelope(
        "{\"v\":1,\"seq\":8,\"type\":\"config_get\","
        "\"c\":{\"channels\":[{\"ladder\":{\"buttons\":[{\"a\":1}]}}]}}", &h));
    EXPECT_STREQ(h.type, "config_get");
}
