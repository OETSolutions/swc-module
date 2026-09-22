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
    // An EXPLICIT zero tail still equals an absent one, which is why the
    // extension loop reads a missing component as zero rather than bailing out.
    EXPECT_EQ(SemverCompare("1.2.3", "1.2.3.0"), 0);
    EXPECT_EQ(SemverCompare("1.2.3.0", "1.2.3"), 0);
}

TEST(ImageVerify, SemverComparesAFourthComponentRatherThanDroppingIt) {
    // **The off-by-one that made a whole release invisible.** The numeric loop
    // ran FOUR times, so it exited with both pointers still ON a 4th component
    // and the code after it compared only pre-release/build metadata -- neither
    // of which "1.2.3.4" has. Measured on the pre-fix function:
    // `SemverCompare("1.2.3.4.5", "1.2.3.4.6")` returned **0**.
    //
    // A 4-component version is not strictly semver, but it is exactly what a
    // date- or build-stamped tag produces, and the failure is silent and in the
    // worst direction: the device is told it is up to date and never offered the
    // new image.
    EXPECT_LT(SemverCompare("1.2.3.4.5", "1.2.3.4.6"), 0) << "was EQUAL before the fix";
    EXPECT_GT(SemverCompare("1.2.3.4.6", "1.2.3.4.5"), 0);
    // And it must compare NUMERICALLY, not as text: "9" < "10" or the pair
    // inverts, which is the same lexicographic trap the top-level test covers.
    EXPECT_LT(SemverCompare("1.2.3.9", "1.2.3.10"), 0) << "text compare would invert this";
    EXPECT_GT(SemverCompare("1.2.3.10", "1.2.3.9"), 0);
    EXPECT_LT(SemverCompare("1.2.3", "1.2.3.4"), 0) << "an extra component is newer";
    EXPECT_GT(SemverCompare("1.2.3.4", "1.2.3"), 0);
}

TEST(ImageVerify, SemverComparesComponentsWiderThanAnyIntegerType) {
    // The comparison is on the DIGITS, not on an accumulated integer, so a
    // component longer than the widest native type still orders correctly. The
    // previous version accumulated into a `long`, which is 64-bit on the host but
    // 32-bit on xtensa -- so a component past 19 digits overflowed the host's and
    // past 9 the device's. That is undefined behaviour (UBSan, pre-fix:
    // "signed integer overflow: 999999999999999999 * 10 cannot be represented in
    // type 'long'"), and a wrapped component can INVERT an ordering, so the
    // device would refuse a real upgrade or accept a downgrade. `kReleaseVersionLen`
    // is 24, so a manifest the parser accepts can carry such a component, and
    // `202609231` (a date-stamped build) already exceeds a 32-bit `long`.
    //
    // 20 digits: wider than a 64-bit `long` can hold.
    EXPECT_LT(SemverCompare("99999999999999999999.0.0",
                            "99999999999999999999.0.1"), 0)
        << "an over-wide component must still let the next one decide";
    EXPECT_LT(SemverCompare("99999999999999999998.0.0",
                            "99999999999999999999.0.0"), 0)
        << "two over-wide components must order by value, not by wrap";
    EXPECT_GT(SemverCompare("99999999999999999999.0.0",
                            "99999999999999999998.0.0"), 0);
    // 9 digits: past a 32-bit `long`'s safe range, which is where the DEVICE
    // would have wrapped even though the host did not. A date stamp is exactly
    // this shape.
    EXPECT_GT(SemverCompare("202609231.0.0", "202609230.0.0"), 0);
    EXPECT_LT(SemverCompare("20260923.0.0", "202609230.0.0"), 0)
        << "digit-count orders a longer numeral above a shorter one";
    // Leading zeros do not inflate the digit count.
    EXPECT_EQ(SemverCompare("0000000000000000000001.0.0", "1.0.0"), 0);
    // **The value that actually INVERTS under a signed accumulate, so this is the
    // assertion that catches the regression.** 2^63 is genuinely larger than 1,
    // but accumulating it into a signed 64-bit wraps it negative, and the compare
    // then reports the larger version as the SMALLER one -- a device that accepts
    // a "downgrade" as an upgrade. Measured against the reintroduced-`long` mutant:
    // this line returned -1 where the fixed code returns +1. The 4th component is
    // the one the extension loop handles, so the case must sit there rather than in
    // the first three (those went through the same mutate and did not invert,
    // which is why an earlier version of this test failed to kill the mutant).
    EXPECT_GT(SemverCompare("1.2.3.9223372036854775808", "1.2.3.1"), 0)
        << "2^63 must compare ABOVE 1; a signed accumulate wraps it negative and "
           "inverts the ordering";
    // The same inversion at the DEVICE's 32-bit `long` (xtensa is ILP32, verified
    // against the toolchain), so a date-stamped 4th component is a real trigger.
    EXPECT_GT(SemverCompare("1.2.3.2147483648", "1.2.3.1"), 0) << "2^31 must exceed 1";
    // The same unbounded compare backs a numeric pre-release identifier.
    EXPECT_GT(SemverCompare("1.3.0-99999999999999999999", "1.3.0-99999999999999999998"), 0);
}

TEST(ImageVerify, SemverOrdersPrereleasesBelowTheirRelease) {
    // semver 2.0.0 rule 11. Without this, "1.3.0-rc1" compared EQUAL to "1.3.0",
    // so a device running the release candidate was never offered the final
    // release -- a silent no-upgrade, the same shape as the lexicographic bug
    // above but one field over.
    EXPECT_LT(SemverCompare("1.3.0-rc1", "1.3.0"), 0);
    EXPECT_GT(SemverCompare("1.3.0", "1.3.0-rc1"), 0);
    EXPECT_LT(SemverCompare("1.3.0-alpha", "1.3.0-beta"), 0);
    EXPECT_LT(SemverCompare("1.3.0-alpha.1", "1.3.0-alpha.2"), 0);
    EXPECT_LT(SemverCompare("1.3.0-alpha", "1.3.0-alpha.1"), 0)
        << "fewer identifiers sorts lower";
    // Numeric identifiers compare numerically and sort below alphanumeric.
    EXPECT_LT(SemverCompare("1.3.0-2", "1.3.0-10"), 0);
    EXPECT_LT(SemverCompare("1.3.0-1", "1.3.0-abc"), 0);
    EXPECT_EQ(SemverCompare("1.3.0-01", "1.3.0-1"), 0) << "leading zeros compare equal";
    // Build metadata is ignored for precedence (rule 10).
    EXPECT_EQ(SemverCompare("1.3.0+build5", "1.3.0"), 0);
    EXPECT_EQ(SemverCompare("1.3.0+build5", "1.3.0+build9"), 0);
    // Rule 10 applies to a PRE-release too: the "+meta" is dropped whether it
    // follows the release or a pre-release identifier. It used to be stripped
    // only when it led the string (the pure "1.3.0+build" case), so "1.3.0-rc+b"
    // kept "+b" inside the compared prerelease and read as NEWER than "1.3.0-rc"
    // -- a release-check that offers an update to the same version, or (reversed)
    // never offers a genuinely newer one.
    EXPECT_EQ(SemverCompare("1.3.0-rc+build", "1.3.0-rc"), 0);
    EXPECT_EQ(SemverCompare("1.3.0-rc.1+build", "1.3.0-rc.1"), 0);
    EXPECT_LT(SemverCompare("1.3.0-rc+build", "1.3.0"), 0)
        << "dropping the metadata must not stop it being a pre-release";
    EXPECT_LT(SemverCompare("1.3.0-alpha+b", "1.3.0-beta"), 0);
    // A numeric gap still dominates the suffix: 1.4.0-beta is newer than 1.3.0.
    EXPECT_GT(SemverCompare("1.4.0-beta", "1.3.0"), 0);
}
