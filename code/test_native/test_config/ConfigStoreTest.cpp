#include "Config/ConfigStore.h"
#include "ConfigFixtures.h"
#include "MockHAL.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

using swctest::MakeConfig;

namespace {

// Slot keys are internal; the test drives failure through MockHal's injected
// write fault, which is the realistic failure (power cut mid-write). Only the
// first chunk of each slot is named here, because only chunk 0 is corrupted
// directly (it carries the header, so corrupting it is what makes the whole slot
// unreadable) -- the sequence key is reached through the fault injector instead,
// so naming it would be an unused constant.
constexpr const char *kNvsA = "cfg_a_0";
constexpr const char *kNvsB = "cfg_b_0";

// Scratch for the tests that need to look at raw NVS values. Sized from the
// codec's own bound, not a literal.
uint8_t g_scratch[ConfigMaxBlobSize()];

}  // namespace

TEST(ConfigStore, EmptyNvsReportsNoConfigRatherThanDefaults) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config out{};
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kNoConfig)
        << "no config is a distinct state: it selects pass-through mode (FR-25)";
}

TEST(ConfigStore, SaveThenLoadRoundTrips) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(MakeConfig()));

    Config out{};
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0001");
}

TEST(ConfigStore, AlternatesSlotsSoThePreviousCopyStaysIntact) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    ASSERT_TRUE(store.Save(c));
    std::strncpy(c.device_id, "SWC-0002", sizeof(c.device_id) - 1);
    ASSERT_TRUE(store.Save(c));

    // Both slots now exist; the store must prefer the newer sequence.
    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0002");
}

TEST(ConfigStore, ATornWriteIsDetectedAndTheBackupIsUsed) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    ASSERT_TRUE(store.Save(c));                       // slot A, seq 1
    std::strncpy(c.device_id, "SWC-0002", sizeof(c.device_id) - 1);

    // Truncate the PAYLOAD write, not the sequence write. The store writes the
    // slot first and cfg_seq last, so the realistic torn case -- power lost
    // while the payload is landing -- is the slot write. NvsSet here both lands
    // a short value AND reports failure, which is exactly what a brown-out
    // mid-write looks like: garbage on the medium, and a driver that says so.
    hal.TruncateNextNvsWriteAt(16);
    EXPECT_FALSE(store.Save(c)) << "a failed write must report failure";

    Config out{};
    // The sequence was never advanced, so slot A is still the newest slot and
    // is still intact: this is kLoaded, not kRecoveredFromBackup. Reaching the
    // backup path needs a TORN SEQUENCE write, which the next test covers --
    // the distinction matters because kLoaded means "nothing was wrong".
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0001")
        << "a torn payload write must leave the previous good config, not a partial one";
}

TEST(ConfigStore, ATornSequenceWriteLeavesTheBackupAuthoritative) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    ASSERT_TRUE(store.Save(c));                        // slot A, seq 1
    std::strncpy(c.device_id, "SWC-0002", sizeof(c.device_id) - 1);
    ASSERT_TRUE(store.Save(c));                        // slot B, seq 2
    std::strncpy(c.device_id, "SWC-0003", sizeof(c.device_id) - 1);

    // The other half of the tear: the payload chunks land, the sequence write is
    // the one that is lost. Save has already written slot A (the non-newest one)
    // with SWC-0003, so seq stays 2 -> slot B (SWC-0002) is still the newest
    // and still valid. This is the case the write-order protocol exists for.
    //
    // It must target the key, not "the next write": a slot is several chunks,
    // so the next write is a payload chunk and the sequence write is never
    // reached. That is why Task 9 adds TruncateNvsWriteTo.
    hal.TruncateNvsWriteTo("cfg_seq", 2);
    EXPECT_FALSE(store.Save(c));

    Config out{};
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0002")
        << "an unlanded sequence write must not promote the payload to newest";
}

TEST(ConfigStore, BothSlotsCorruptFallsBackToDefaultsNotToHalfAConfig) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    ASSERT_TRUE(store.Save(c));                        // slot A, seq 1
    std::strncpy(c.device_id, "SWC-0002", sizeof(c.device_id) - 1);
    ASSERT_TRUE(store.Save(c));                        // slot B, seq 2
    // Corrupt BOTH slots -- that is what the test name says, and with only one
    // corrupted the store would simply use the other and this would be a
    // duplicate of ATornWriteIsDetectedAndTheBackupIsUsed.
    hal.CorruptNvsValue(kNvsA, 3);
    hal.CorruptNvsValue(kNvsB, 3);
    Config out{};
    const ConfigLoadResult r = store.Load(&out);
    EXPECT_TRUE(r == ConfigLoadResult::kFellBackToDefaults ||
                r == ConfigLoadResult::kNoConfig)
        << "never a partial config";
}

TEST(ConfigStore, SequenceNumbersAreMonotonicAcrossSaves) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    Config c = MakeConfig();
    for (int i = 0; i < 5; ++i) {
        std::strncpy(c.device_id, ("SWC-000" + std::to_string(i)).c_str(),
                     sizeof(c.device_id) - 1);
        ASSERT_TRUE(store.Save(c));
    }
    EXPECT_GE(store.LoadedSequence(), 5u);
}

TEST(ConfigStore, AFailedWriteReportsFailureAndLeavesTheOldConfigLoadable) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(MakeConfig()));
    hal.FailNextNvsWrite();
    Config c2 = MakeConfig();
    std::strncpy(c2.device_id, "SWC-9999", sizeof(c2.device_id) - 1);
    EXPECT_FALSE(store.Save(c2));

    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-0001");
}

TEST(ConfigStore, AMultiChunkConfigRoundTripsThroughEveryChunkKey) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    const Config big = swctest::MakeBigConfig();

    // Assert the fixture really is multi-chunk, or this test silently degenerates
    // into a second copy of SaveThenLoadRoundTrips -- which is exactly how the
    // chunked path went untested until this test existed.
    const size_t blob_len = ConfigEncodeBlob(big, g_scratch, sizeof(g_scratch));
    ASSERT_GT(blob_len, kConfigChunkBytes)
        << "the fixture must span more than one chunk for this test to mean anything";
    const int chunks = ConfigChunkCountFor(blob_len);
    ASSERT_GT(chunks, 1);

    ASSERT_TRUE(store.Save(big));

    // Every chunk key must exist, not just chunk 0. A store that wrote only the
    // header would still load *something*; this is what makes the gap visible.
    for (int i = 0; i < chunks; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "cfg_a_%d", i);
        EXPECT_GE(hal.NvsGet(key, g_scratch, sizeof(g_scratch)), 0)
            << "missing chunk key " << key;
    }

    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_STREQ(out.device_id, "SWC-BIG");
    // Spot-check a field that lives in a LATE chunk, so a truncating read is
    // caught: the last binding's action target is past the first chunk.
    ASSERT_EQ(out.binding_count, 8);
    EXPECT_STREQ(out.bindings[7].actions[0].target, "com.oetsolutions.swc.ACTION_NAVIGATE");
    EXPECT_EQ(out.channels[0].ladder.count, kLadderMaxButtons);
}

TEST(ConfigStore, AMissingMiddleChunkRejectsTheWholeSlotRatherThanDecodingPartially) {
    MockHal hal;
    ConfigStore store(&hal.InterfaceRef());
    const Config big = swctest::MakeBigConfig();
    ASSERT_TRUE(store.Save(big));

    // Destroy a LATER chunk of the newest slot. The header (chunk 0) is intact,
    // so the slot's declared length still promises the full set -- this is the
    // case where a store that trusted the header alone would decode a short
    // buffer. The slot must be rejected whole.
    hal.CorruptNvsValue("cfg_a_1", 0);
    hal.ClearNvsKey("cfg_a_1");

    Config out{};
    const ConfigLoadResult r = store.Load(&out);
    // Slot A is now unreadable; there is no slot B yet, so nothing valid remains.
    EXPECT_TRUE(r == ConfigLoadResult::kFellBackToDefaults ||
                r == ConfigLoadResult::kNoConfig)
        << "a partial chunk set must never decode into a config";
}
