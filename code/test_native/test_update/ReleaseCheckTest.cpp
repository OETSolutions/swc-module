#include "Update/ReleaseCheck.h"

#include <gtest/gtest.h>

#include <string>

namespace {
// Spec 9.5's manifest shape, verbatim: nested with semver strings. The top-level
// `latest_version` and `firmware.version` must agree, which is why both appear.
const char *kManifest = R"({
  "latest_version": "0.12.0",
  "channel": "stable",
  "firmware": {
    "version": "0.12.0",
    "url": "https://github.com/oetsolutions/swc-module/releases/download/v0.12.0/firmware.bin",
    "size_bytes": 1543210,
    "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
  },
  "assets": null,
  "min_from_version": "0.5.0",
  "release_date": "2026-09-18T00:00:00Z"
})";
}  // namespace

TEST(ReleaseCheck, ANewerVersionIsOfferedAndItsFieldsAreParsed) {
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, /*current=*/"0.11.0"),
              ReleaseCheckResult::kNewer);
    EXPECT_STREQ(info.latest_version, "0.12.0");
    EXPECT_STREQ(info.firmware_version, "0.12.0");
    EXPECT_EQ(info.size_bytes, 1543210u);
    EXPECT_STREQ(info.min_from_version, "0.5.0");
    EXPECT_STREQ(info.channel, "stable");
    EXPECT_EQ(std::string(info.sha256_hex).size(), 64u);
}

TEST(ReleaseCheck, TheSameVersionIsUpToDateRatherThanAnUpdate) {
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, "0.12.0"), ReleaseCheckResult::kUpToDate);
}

TEST(ReleaseCheck, AnOlderVersionIsNeverOfferedAsAnUpgrade) {
    // A downgrade is not a lesser update; it is a rollback an attacker can
    // induce to put a known-vulnerable image back on the device.
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, "0.13.0"), ReleaseCheckResult::kNotNewer);
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, "1.0.0"), ReleaseCheckResult::kNotNewer);
}

TEST(ReleaseCheck, AVersionBelowTheDeclaredFloorIsRefusedWithItsOwnReason) {
    // min_from_version exists for migrations that need an intermediate step: the
    // image is newer, but applying it directly from this version is not
    // supported.
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, "0.4.0"),
              ReleaseCheckResult::kTooOldToUpgradeFrom);
    // And exactly AT the floor is allowed -- the boundary belongs to the caller.
    EXPECT_EQ(ReleaseCheckParse(kManifest, &info, "0.5.0"), ReleaseCheckResult::kNewer);
}

TEST(ReleaseCheck, AManifestWithNoFloorOffersAnyNewerVersion) {
    const char *m = R"({"latest_version":"1.0.0",
      "firmware":{"version":"1.0.0","url":"https://x/y.bin","size_bytes":10,
                  "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}})";
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(m, &info, "0.1.0"), ReleaseCheckResult::kNewer);
    EXPECT_STREQ(info.min_from_version, "") << "absent floor stays empty, not fabricated";
}

TEST(ReleaseCheck, AMalformedManifestIsRefusedNotPartlyApplied) {
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse("{ not json", &info, "0.11.0"), ReleaseCheckResult::kMalformed);
    EXPECT_EQ(ReleaseCheckParse("{}", &info, "0.11.0"), ReleaseCheckResult::kMalformed);
    EXPECT_EQ(ReleaseCheckParse("[]", &info, "0.11.0"), ReleaseCheckResult::kMalformed);
    // A manifest missing the hash must not be accepted as "no hash to check".
    EXPECT_EQ(ReleaseCheckParse(R"({"latest_version":"1.0.0",
        "firmware":{"version":"1.0.0","url":"https://x/y.bin","size_bytes":10}})",
        &info, "0.11.0"), ReleaseCheckResult::kMalformed);
    // Neither must one missing the size, or reporting zero of it.
    EXPECT_EQ(ReleaseCheckParse(R"({"latest_version":"1.0.0",
        "firmware":{"version":"1.0.0","url":"https://x/y.bin","size_bytes":0,
                    "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}})",
        &info, "0.11.0"), ReleaseCheckResult::kMalformed);
}

TEST(ReleaseCheck, AFractionalSizeIsRefusedRatherThanTruncated) {
    // `size_bytes` is a BYTE COUNT, and every sibling reader of an integer field
    // in this tree refuses a fraction rather than truncating it -- `ConfigCodec`'s
    // `ReadU32`/`ReadU64` and `CommandRouter`'s `NumToU32`/`NumToU8` all carry the
    // same rule with the same reasoning: truncating 750.9 to 750 accepts a value
    // the sender did not write and the receiver then acts on. `ReadSize` was the
    // one reader that cast a bare `static_cast<size_t>`, so `size_bytes: 1543210.9`
    // was accepted as 1543210.
    //
    // The consequence is not a wrong-accepted image (OtaEnd still requires an exact
    // byte count AND the digest) but a permanent, unexplained failure: the manifest
    // declares a size no real image has, so every download of a correct image ends
    // in `kSizeMismatch`, which names neither the manifest nor the field.
    const char *m = R"({"latest_version":"1.0.0",
      "firmware":{"version":"1.0.0","url":"https://x/y.bin","size_bytes":1543210.9,
                  "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}})";
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(m, &info, "0.1.0"), ReleaseCheckResult::kMalformed)
        << "a byte count carrying a fraction is a malformed manifest, not 1543210";
}

TEST(ReleaseCheck, ANonHttpsUrlIsRefused) {
    // Plain http is not a lesser preference, it is a downgrade attack: an
    // attacker on the path substitutes the image, and the SHA-256 does not help
    // because the hash came over the same hijacked channel.
    std::string m = kManifest;
    const std::string https = "https://";
    const size_t pos = m.find(https);
    ASSERT_NE(pos, std::string::npos);
    m.replace(pos, https.size(), "http://");
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(m.c_str(), &info, "0.11.0"), ReleaseCheckResult::kMalformed)
        << "the OTA image must come over TLS";
}

TEST(ReleaseCheck, AManifestWhoseTwoVersionsDisagreeIsRefused) {
    // The pipeline publishes the version twice: at the top level and inside
    // `firmware`. If they disagree the manifest is self-contradictory, and
    // silently picking one is how a device installs a version it did not choose.
    const char *m = R"({"latest_version":"9.9.9",
      "firmware":{"version":"1.0.0","url":"https://x/y.bin","size_bytes":10,
                  "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}})";
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(m, &info, "0.1.0"), ReleaseCheckResult::kMalformed);
}

TEST(ReleaseCheck, ANumericVersionGapUsesSemverNotStringComparison) {
    // The case a flat version_code hid entirely and a string compare gets
    // backwards: 0.10.0 IS newer than 0.9.0.
    const char *m = R"({"latest_version":"0.10.0",
      "firmware":{"version":"0.10.0","url":"https://x/y.bin","size_bytes":10,
                  "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}})";
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(m, &info, "0.9.0"), ReleaseCheckResult::kNewer)
        << "0.10.0 must be offered to a device on 0.9.0";
}

TEST(ReleaseCheck, AHostileUrlIsRefusedRatherThanPassedThrough) {
    // The URL is later handed to an HTTP client. A scheme that is not https, or a
    // string long enough to overflow the field, must be refused at PARSE time --
    // this is the boundary where untrusted input arrives.
    ReleaseInfo info{};
    EXPECT_EQ(ReleaseCheckParse(R"({"latest_version":"1.0.0",
        "firmware":{"version":"1.0.0","url":"file:///etc/passwd","size_bytes":10,
                    "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}})",
        &info, "0.1.0"), ReleaseCheckResult::kMalformed);
}
