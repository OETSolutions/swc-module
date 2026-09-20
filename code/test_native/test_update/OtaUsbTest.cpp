#include "Update/OtaUsb.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

/*
 * USB OTA (spec 9.3, FR-36/FR-41) -- the state machine behind the `ota_*` frames.
 *
 * **This suite did not exist, and the module had shipped without one.** The whole
 * of `OtaUsb.cpp` -- the gate that decides whether an image may be written to
 * flash and whether the boot partition moves -- was reachable only from a device
 * build, and the CommandRouter nacked its frames as `not_implemented`, so nothing
 * exercised it on the host either. A bricking-risk path with no test is the
 * defect; the tests below are the fix.
 *
 * What CANNOT be tested here is the flash write itself: `OtaSupported()` is false
 * on the host by design (the module refuses to pretend a write happened), so
 * `OtaBegin`/`OtaCommit` return `kNotSupported` past the verification gate. These
 * tests therefore pin the VERIFICATION half -- which is the half spec 9.3 says
 * must never be skipped ("never trust a size-only check") -- plus the state
 * machine's refusals, which are pure logic and complete on the host.
 */

namespace {
// The payload's digest, through the same stream the device uses (as
// ImageVerifyTest does) -- a second hash implementation is how a device ends up
// accepting one digest and rejecting another.
std::string PayloadHash(const std::string &data) {
    Sha256Stream s;
    s.Update(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    uint8_t d[32];
    s.Final(d);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
    return std::string(hex, 64);
}

constexpr size_t kSlot = 1920u * 1024;   // app0/app1 in partitions.csv
constexpr int kChunk = 512;              // kConfigWireChunkBytes

// Feed a whole image in wire-sized chunks, returning the result of the last one.
OtaResult FeedWhole(const std::string &img) {
    OtaResult r = OtaResult::kOk;
    for (size_t off = 0; off < img.size(); off += kChunk) {
        const size_t n = (img.size() - off < kChunk) ? (img.size() - off) : kChunk;
        r = OtaChunk(reinterpret_cast<const uint8_t *>(img.data() + off), n);
        if (r != OtaResult::kOk) return r;
    }
    return r;
}
}  // namespace

TEST(OtaUsb, AChunkArrivingWithNoRunOpenIsRefused) {
    // The frame vocabulary is a closed set, but the ORDER is not enforced by it:
    // a chunk can arrive first if the app's state machine is wrong. Accepting it
    // would hash bytes into a run that was never begun.
    OtaAbort();   // a clean slate, since the module keeps its state in statics
    EXPECT_EQ(OtaChunk(reinterpret_cast<const uint8_t *>("x"), 1), OtaResult::kNotStarted);
    EXPECT_FALSE(OtaInProgress());
}

TEST(OtaUsb, EndingWithNoRunOpenIsRefused) {
    OtaAbort();
    EXPECT_EQ(OtaEnd(), OtaResult::kNotStarted);
}

TEST(OtaUsb, ANonHexOrWrongLengthHashIsRefusedBeforeAnyDataArrives) {
    // FR-36's refusal, at the earliest possible point: a malformed digest is
    // knowable from the ota_begin frame alone, so no byte of a 1.75 MB image
    // should reach the verifier first.
    OtaAbort();
    EXPECT_EQ(OtaBegin(65536, "not-a-hash", kSlot), OtaResult::kVerifyFailed);
    EXPECT_EQ(OtaBegin(65536, "00", kSlot), OtaResult::kVerifyFailed)
        << "a 2-character digest is not a SHA-256, whatever it parses as";
    EXPECT_FALSE(OtaInProgress());
}

TEST(OtaUsb, AnImageLargerThanTheSlotIsRefusedBeforeAnyDataArrives) {
    // Spec 9.3: "The device MUST reject an image claiming the wrong size". The
    // slot bound is what stops a 4 MB image from filling the inactive slot and
    // then failing at the end, having already written past what can be committed.
    OtaAbort();
    const std::string img(1024, 'A');
    EXPECT_EQ(OtaBegin(kSlot + 1, PayloadHash(img).c_str(), kSlot), OtaResult::kTooLarge);
    EXPECT_FALSE(OtaInProgress());
}

TEST(OtaUsb, AZeroLengthImageIsRefused) {
    // A zero-length run would pass a size check trivially and commit a slot with
    // no image in it -- the device would then boot nothing.
    OtaAbort();
    EXPECT_EQ(OtaBegin(0, PayloadHash("").c_str(), kSlot), OtaResult::kVerifyFailed);
    EXPECT_FALSE(OtaInProgress());
}

TEST(OtaUsb, ASingleFlippedBitIsCaughtByTheDigestNotTheSize) {
    // The core of FR-36 and the reason spec 9.3 insists on an incremental SHA-256:
    // a corrupted image that is exactly the right length must still be refused.
    // A size-only check is precisely the weakening this test exists to prevent.
    //
    // The corruption is NOT caught while streaming -- spec 9.3 compares the digest
    // "at ota_end", and the chunks are only accumulated -- so the assertion is that
    // every chunk is accepted and the END refuses. (An earlier version of this test
    // expected the failing chunk to be refused, which is not how the two-stage gate
    // works. Measured, not assumed.)
    OtaAbort();
    const std::string img(64 * 1024, 'A');
    std::string corrupt = img;
    corrupt[40000] ^= 0x01;

    ASSERT_EQ(OtaBegin(corrupt.size(), PayloadHash(img).c_str(), kSlot), OtaResult::kOk);
    EXPECT_EQ(FeedWhole(corrupt), OtaResult::kOk)
        << "the size and every chunk are fine; only the digest is wrong";
    EXPECT_EQ(OtaEnd(), OtaResult::kVerifyFailed)
        << "a right-length image with a flipped bit must be refused";
    EXPECT_FALSE(OtaInProgress()) << "a failed verify must close the run";
}

TEST(OtaUsb, ATruncatedRunIsRefusedAtTheEnd) {
    // FR-36: a truncated image must be refused, and the run closed so the next
    // one cannot inherit its bytes.
    //
    // **Be precise about which gate catches this, because the code has two and
    // they overlap.** `OtaUsb::OtaEnd` compares its own `written_` against
    // `declared_size_`, and `ImageVerifyEnd` independently checks the byte count
    // it accumulated from the same chunks. Both compare numbers incremented by the
    // same calls, so deleting `OtaEnd`'s check changes no observable result --
    // `ImageVerifyEnd` returns `kSizeMismatch` and the mapping below produces the
    // same `kVerifyFailed`. I tried to write a mutation that fails when that check
    // is deleted and could not, which is the honest reason this comment describes
    // it as belt-and-braces rather than pointing at a test that pins it. Removing
    // it would be a change to a bricking path in exchange for nothing.
    OtaAbort();
    const std::string img(4096, 'B');
    ASSERT_EQ(OtaBegin(img.size(), PayloadHash(img).c_str(), kSlot), OtaResult::kOk);
    EXPECT_EQ(FeedWhole(img.substr(0, 2048)), OtaResult::kOk);
    EXPECT_EQ(OtaEnd(), OtaResult::kVerifyFailed)
        << "4096 declared but 2048 streamed is a truncated image (FR-36)";
    EXPECT_FALSE(OtaInProgress()) << "the run must close so it cannot leak into the next";
}

TEST(OtaUsb, ANullChunkWithALengthIsRefusedRatherThanDereferenced) {
    // `OtaChunk(nullptr, 1)` would hand a null pointer and a nonzero length to the
    // digest stream, which reads it. The guard is cheap and the alternative is a
    // crash inside the one path that writes flash.
    OtaAbort();
    const std::string img(1024, 'E');
    ASSERT_EQ(OtaBegin(img.size(), PayloadHash(img).c_str(), kSlot), OtaResult::kOk);
    EXPECT_EQ(OtaChunk(nullptr, 1), OtaResult::kVerifyFailed);
    EXPECT_FALSE(OtaInProgress()) << "a malformed frame must not leave a run open";
}

TEST(OtaUsb, AnAbandonedRunDoesNotLeakItsByteCountIntoTheNext) {
    // The "leak into the next run" case `ImageVerifyReset` exists to prevent,
    // asserted at the layer a caller sees. Without it a second, shorter run could
    // satisfy the size check with the first run's bytes.
    OtaAbort();
    const std::string img(4096, 'F');
    ASSERT_EQ(OtaBegin(img.size(), PayloadHash(img).c_str(), kSlot), OtaResult::kOk);
    EXPECT_EQ(FeedWhole(img.substr(0, 2048)), OtaResult::kOk);
    EXPECT_EQ(OtaBytesWritten(), 2048u);

    OtaAbort();
    EXPECT_EQ(OtaBytesWritten(), 0u) << "an aborted run must not leave a count behind";
    EXPECT_FALSE(OtaInProgress());

    // A fresh run of the SAME declared size must still be refused at the end when
    // only half the bytes arrive, which it could not be if the count survived.
    ASSERT_EQ(OtaBegin(img.size(), PayloadHash(img).c_str(), kSlot), OtaResult::kOk);
    EXPECT_EQ(FeedWhole(img.substr(0, 2048)), OtaResult::kOk);
    EXPECT_EQ(OtaEnd(), OtaResult::kVerifyFailed);
    EXPECT_FALSE(OtaInProgress());
}

TEST(OtaUsb, AHostBuildRefusesToCommitRatherThanClaimingSuccess) {
    // `OtaSupported()` is false off-target, and the module's own comment says a
    // host caller that got kOk "would believe an image was installed". This pins
    // that: the verification gate is reached and passed, and the commit still
    // reports that this build cannot install anything.
    //
    // This is also what makes the suite honest about its own coverage -- the flash
    // write is NOT exercised here, and saying so is better than a green test that
    // appears to cover it.
    OtaAbort();
    ASSERT_FALSE(OtaSupported()) << "the host build has no partitions to write";
    const std::string img(4096, 'C');
    ASSERT_EQ(OtaBegin(img.size(), PayloadHash(img).c_str(), kSlot), OtaResult::kOk);
    ASSERT_EQ(FeedWhole(img), OtaResult::kOk);
    EXPECT_EQ(OtaEnd(), OtaResult::kNotSupported)
        << "the host must not report a successful install";
    EXPECT_FALSE(OtaInProgress());
}

TEST(OtaUsb, ASecondBeginWhileARunIsOpenIsRefused) {
    // Two interleaved runs would share one digest stream, so the second image's
    // bytes would be hashed into the first's -- and whichever commit landed would
    // install an image neither run verified.
    OtaAbort();
    const std::string img(1024, 'D');
    ASSERT_EQ(OtaBegin(img.size(), PayloadHash(img).c_str(), kSlot), OtaResult::kOk);
    EXPECT_EQ(OtaBegin(img.size(), PayloadHash(img).c_str(), kSlot), OtaResult::kAlreadyStarted);
    OtaAbort();
}
