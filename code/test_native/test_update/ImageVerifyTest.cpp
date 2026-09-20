#include "Update/ImageVerify.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {
// The payload's digest, computed through the same stream the device uses.
std::string PayloadHash(const std::string &data) {
    Sha256Stream s;
    s.Update(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    uint8_t d[32];
    s.Final(d);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
    return std::string(hex, 64);
}
}  // namespace

TEST(ImageVerify, AcceptsAnImageWhoseChecksumAndSizeBothMatch) {
    const std::string img(65536, 'A');
    const std::string hash = PayloadHash(img);
    ASSERT_EQ(ImageVerifyBegin(hash.c_str(), img.size(), 2u * 1024 * 1024), VerifyResult::kOk);
    ASSERT_EQ(ImageVerifyChunk(reinterpret_cast<const uint8_t *>(img.data()), img.size()),
              VerifyResult::kOk);
    EXPECT_EQ(ImageVerifyEnd(), VerifyResult::kOk);
}

TEST(ImageVerify, DetectsASingleFlippedBitAnywhereInTheImage) {
    const std::string img(65536, 'A');
    const std::string hash = PayloadHash(img);
    std::string corrupt = img;
    corrupt[40000] ^= 0x01;
    ASSERT_EQ(ImageVerifyBegin(hash.c_str(), corrupt.size(), 2u * 1024 * 1024), VerifyResult::kOk);
    ImageVerifyChunk(reinterpret_cast<const uint8_t *>(corrupt.data()), corrupt.size());
    EXPECT_EQ(ImageVerifyEnd(), VerifyResult::kChecksumMismatch);
}

TEST(ImageVerify, DetectsAShortImageEvenWhenTheBytesItHasAreCorrect) {
    const std::string img(65536, 'A');
    const std::string hash = PayloadHash(img);
    const std::string truncated = img.substr(0, 65535);
    ASSERT_EQ(ImageVerifyBegin(hash.c_str(), img.size(), 2u * 1024 * 1024), VerifyResult::kOk);
    ImageVerifyChunk(reinterpret_cast<const uint8_t *>(truncated.data()), truncated.size());
    EXPECT_EQ(ImageVerifyEnd(), VerifyResult::kSizeMismatch);
}

TEST(ImageVerify, RefusesAnImageLargerThanTheTargetPartitionBeforeStreamingIt) {
    EXPECT_EQ(ImageVerifyBegin("00", 4u * 1024 * 1024, 2u * 1024 * 1024), VerifyResult::kTooLarge)
        << "refuse up front rather than after writing 4MB to flash";
}

TEST(ImageVerify, RefusesAZeroLengthImage) {
    EXPECT_EQ(ImageVerifyBegin("00", 0, 2u * 1024 * 1024), VerifyResult::kEmpty);
}

TEST(ImageVerify, VerifiesAcrossManyChunksNotJustOne) {
    // Chunk boundaries are where a streaming verifier usually breaks: state
    // carried between calls is the whole risk.
    std::string img;
    for (int i = 0; i < 100000; ++i) img.push_back(static_cast<char>(i * 7 % 251));
    const std::string hash = PayloadHash(img);
    ASSERT_EQ(ImageVerifyBegin(hash.c_str(), img.size(), 2u * 1024 * 1024), VerifyResult::kOk);
    for (size_t off = 0; off < img.size(); off += 997) {
        const size_t n = std::min<size_t>(997, img.size() - off);
        ASSERT_EQ(ImageVerifyChunk(reinterpret_cast<const uint8_t *>(img.data() + off), n),
                  VerifyResult::kOk);
    }
    EXPECT_EQ(ImageVerifyEnd(), VerifyResult::kOk);
}

TEST(ImageVerify, AMalformedHashStringIsRefusedRatherThanTreatedAsAZeroHash) {
    const std::string img(1024, 'A');
    EXPECT_EQ(ImageVerifyBegin("not-a-hash", img.size(), 2u * 1024 * 1024),
              VerifyResult::kMalformedHash);
    EXPECT_EQ(ImageVerifyBegin("aabb", img.size(), 2u * 1024 * 1024),
              VerifyResult::kMalformedHash);
}

TEST(ImageVerify, ResetClearsStateSoAFailedRunCannotLeakIntoTheNext) {
    // A verifier that keeps its stream across runs would hash run 2 appended to
    // run 1, and reject a perfectly good image.
    const std::string a(4096, 'A');
    const std::string b(4096, 'B');
    ASSERT_EQ(ImageVerifyBegin(PayloadHash(a).c_str(), a.size(), 1u << 20), VerifyResult::kOk);
    ImageVerifyChunk(reinterpret_cast<const uint8_t *>(a.data()), a.size());
    ImageVerifyReset();

    ASSERT_EQ(ImageVerifyBegin(PayloadHash(b).c_str(), b.size(), 1u << 20), VerifyResult::kOk);
    ImageVerifyChunk(reinterpret_cast<const uint8_t *>(b.data()), b.size());
    EXPECT_EQ(ImageVerifyEnd(), VerifyResult::kOk) << "run 2 must not see run 1's bytes";
}

TEST(ImageVerify, AChunkBeforeBeginIsRefused) {
    ImageVerifyReset();
    const std::string img(16, 'A');
    EXPECT_EQ(ImageVerifyChunk(reinterpret_cast<const uint8_t *>(img.data()), img.size()),
              VerifyResult::kMalformedHash)
        << "streaming without a validated hash is not a verification";
}

TEST(ImageVerify, TooLargeIsReportedRatherThanEmptyForAZeroSizeOverTheCap) {
    // Ordered guards: an image that is both empty and over the cap is not a case
    // (0 > max is false), but a caller passing max_size 0 must get a refusal
    // rather than an accept of a zero-length region.
    EXPECT_EQ(ImageVerifyBegin("00", 100, 0), VerifyResult::kTooLarge);
}

// --- semver ---------------------------------------------------------------

TEST(ImageVerify, SemverComparesNumericallyNotLexicographically) {
    // The reason a string compare is wrong: "1.10.0" > "1.9.0" numerically, but
    // "1.10.0" < "1.9.0" character by character. A lexicographic compare would
    // never offer 1.10 to a device running 1.9 -- the release after the tenth
    // minor would be invisible forever.
    EXPECT_GT(SemverCompare("1.10.0", "1.9.0"), 0);
    EXPECT_LT(SemverCompare("1.9.0", "1.10.0"), 0);
    EXPECT_GT(SemverCompare("1.2.0", "1.1.99"), 0);
    EXPECT_LT(SemverCompare("2.0.0", "10.0.0"), 0);
}

TEST(ImageVerify, SemverTreatsEqualVersionsAsEqualAndMissingPartsAsZero) {
    EXPECT_EQ(SemverCompare("1.2.0", "1.2.0"), 0);
    EXPECT_EQ(SemverCompare("1.2", "1.2.0"), 0);
    EXPECT_EQ(SemverCompare("1.2.0", "1.2"), 0);
    EXPECT_EQ(SemverCompare("1", "1.0.0"), 0);
}
