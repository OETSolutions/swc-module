#include "Link/Ndjson.h"
#include <gtest/gtest.h>
#include <cstring>

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
