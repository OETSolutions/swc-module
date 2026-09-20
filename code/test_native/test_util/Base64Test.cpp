#include "Util/Base64.h"

#include "Link/Ndjson.h"   // kNdjsonMaxFrame: the cap kConfigWireChunkBytes is sized against

#include <gtest/gtest.h>

#include <string>

TEST(Base64, RoundTripsEveryLengthAroundThePaddingBoundaries) {
    // Starts at 1, not 0. `Base64Encode` returns the bytes written and 0 to
    // signal "did not fit", so a zero-length input -- which correctly encodes to
    // nothing -- is indistinguishable from a failure by return value alone. Both
    // transports always have at least one byte, so this is not a case either
    // hits; the test starts at 1 and the next test pins the buffer boundary
    // explicitly rather than leaving the collision undocumented.
    for (size_t n = 1; n < 80; ++n) {
        std::string in;
        for (size_t i = 0; i < n; ++i) in.push_back(static_cast<char>(i * 7 % 256));
        char enc[256];
        const size_t el = Base64Encode(reinterpret_cast<const uint8_t *>(in.data()), in.size(),
                                       enc, sizeof(enc));
        ASSERT_GT(el, 0u) << "n=" << n;
        uint8_t dec[256];
        size_t dn = 0;
        ASSERT_TRUE(Base64Decode(enc, el, dec, sizeof(dec), &dn)) << "n=" << n;
        EXPECT_EQ(dn, n) << "n=" << n;
        EXPECT_EQ(std::string(reinterpret_cast<char *>(dec), dn), in) << "n=" << n;
    }
}

TEST(Base64, AFullOutputBufferIsRefusedRatherThanTruncated) {
    // The exact-fit boundary, which is where an off-by-one in the size check
    // lives: 3 bytes encode to 4, so a 5-byte buffer holds it including the NUL.
    char exact[5];
    const char *abc = "abc";
    EXPECT_EQ(Base64Encode(reinterpret_cast<const uint8_t *>(abc), 3, exact, sizeof(exact)), 4u);
    EXPECT_STREQ(exact, "YWJj");
    char one_short[4];
    EXPECT_EQ(Base64Encode(reinterpret_cast<const uint8_t *>(abc), 3, one_short, sizeof(one_short)), 0u);
}

TEST(Base64, EncodesTheRfc4648Vectors) {
    char out[16];
    const char *f = "foobar";
    const size_t n = Base64Encode(reinterpret_cast<const uint8_t *>(f), 6, out, sizeof(out));
    EXPECT_EQ(std::string(out, n), "Zm9vYmFy");
    const char *m = "f";
    const size_t n2 = Base64Encode(reinterpret_cast<const uint8_t *>(m), 1, out, sizeof(out));
    EXPECT_EQ(std::string(out, n2), "Zg==");
}

TEST(Base64, RefusesAnOutputBufferThatCannotHoldTheResult) {
    // A silent truncation here would corrupt a config transfer with no error
    // anywhere -- the receiver would see a short chunk and blame the CRC.
    const std::string in(300, 'x');
    char small[16];
    EXPECT_EQ(Base64Encode(reinterpret_cast<const uint8_t *>(in.data()), in.size(),
                           small, sizeof(small)), 0u);
}

TEST(Base64, AMalformedInputIsRefused) {
    uint8_t out[64];
    size_t n = 0;
    // Not a multiple of 4.
    EXPECT_FALSE(Base64Decode("YWJ", 3, out, sizeof(out), &n));
    // A character outside the alphabet.
    EXPECT_FALSE(Base64Decode("YWJ!", 4, out, sizeof(out), &n));
    // Padding in a position that cannot mean anything.
    EXPECT_FALSE(Base64Decode("Y=Jj", 4, out, sizeof(out), &n));
    EXPECT_FALSE(Base64Decode("=YWJ", 4, out, sizeof(out), &n));
    // Output that cannot hold the decoded bytes.
    EXPECT_FALSE(Base64Decode("YWJj", 4, out, 2, &n));
    // Valid, so the refusals above are not vacuous.
    EXPECT_TRUE(Base64Decode("YWJj", 4, out, sizeof(out), &n));
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 'a');
}

TEST(Base64, PaddedInputDecodesToTheShorterLength) {
    // The padding cases carry the actual risk: "Zg==" is ONE byte, not three, so
    // a length computed from the input size alone writes two bytes too many.
    uint8_t out[8];
    size_t n = 0;
    ASSERT_TRUE(Base64Decode("Zg==", 4, out, sizeof(out), &n));
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(out[0], 'f');

    ASSERT_TRUE(Base64Decode("Zm8=", 4, out, sizeof(out), &n));
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(out), n), "fo");
}

TEST(Base64, ADecodedChunkFitsTheLineCap) {
    // The whole reason kConfigWireChunkBytes is 512 and not 1024: the DECODED
    // size is what the transport buffers, but the ENCODED size is what must fit
    // the frame. Sizing by the decoded length is how a chunked transport ends up
    // unable to send its own chunks.
    std::string in(kConfigWireChunkBytes, 'x');
    char enc[1024];
    const size_t el = Base64Encode(reinterpret_cast<const uint8_t *>(in.data()), in.size(),
                                   enc, sizeof(enc));
    ASSERT_GT(el, 0u);
    // Envelope + offset + separators, generously bounded.
    EXPECT_LT(el + 128u, kNdjsonMaxFrame);
}
