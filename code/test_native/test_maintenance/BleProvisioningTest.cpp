#include "Maintenance/BleProvisioning.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

TEST(PopDerive, IsDeterministicForAGivenDevice) {
    // The app reads the PoP over USB and the user types it into the Espressif
    // app. If the two derivations disagreed, provisioning could never succeed --
    // and it would fail as "wrong password", which points nowhere near the bug.
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
    char a[32], b[32];
    ASSERT_TRUE(PopDerive(mac, a, sizeof(a)));
    ASSERT_TRUE(PopDerive(mac, b, sizeof(b)));
    EXPECT_STREQ(a, b);
}

TEST(PopDerive, DiffersAcrossDevicesAndIsNotTheMacItself) {
    // Two failure modes at once: a PoP that collides between units (so one
    // device's code provisions another), and a PoP that IS the MAC in the
    // clear -- which is guessable by anyone in radio range.
    std::set<std::string> seen;
    for (uint8_t i = 0; i < 16; ++i) {
        uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, i};
        char pop[32];
        ASSERT_TRUE(PopDerive(mac, pop, sizeof(pop))) << "i=" << static_cast<int>(i);
        seen.insert(pop);
        EXPECT_NE(std::string(pop).size(), 0u);
    }
    EXPECT_EQ(seen.size(), 16u) << "a PoP must be per-device";
}

TEST(PopDerive, RefusesRatherThanReturningATrivialPopWhenTheBufferIsTooSmall) {
    // A truncated PoP is worse than a refusal: it is short, so it looks like it
    // works, and the Espressif app rejects it with a generic failure.
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    char tiny[2];
    EXPECT_FALSE(PopDerive(mac, tiny, sizeof(tiny)));
}

TEST(PopDerive, ATwoByteBufferCannotHoldTheDigestPrefix) {
    // The boundary the size check is actually about: 6 hex digits need 7 bytes
    // with the terminator, and a check that allowed out_len == 6 would write
    // past a 6-byte buffer.
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    char six[6];
    EXPECT_FALSE(PopDerive(mac, six, sizeof(six)));
    char seven[7];
    EXPECT_TRUE(PopDerive(mac, seven, sizeof(seven)));
    EXPECT_EQ(std::strlen(seven), 6u);
}

TEST(PopDerive, ThePopIsNotMerelyTheMacRenderedAsText) {
    // The salt is what makes this true. Without it, the digest of a known MAC is
    // still per-device but the PoP would be a pure function anyone could compute
    // from a passively-observed MAC.
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
    char pop[32];
    ASSERT_TRUE(PopDerive(mac, pop, sizeof(pop)));
    EXPECT_NE(std::string(pop), "246F28ABCDEF");
    EXPECT_EQ(std::strlen(pop), 6u) << "short enough to read aloud off a screen";
}

TEST(DeviceIdShort, IsShortAndDistinguishesUnits) {
    // Spec 8.3: the advertised name includes a short device id so multiple units
    // are distinguishable in the Espressif app's scan list.
    std::set<std::string> ids;
    for (uint8_t i = 0; i < 8; ++i) {
        uint8_t mac[6] = {0x24, 0x6F, 0x28, 0x00, 0x00, i};
        char id[16];
        DeviceIdShort(mac, id, sizeof(id));
        EXPECT_GT(std::strlen(id), 0u);
        EXPECT_LT(std::strlen(id), 12u) << "must fit an advertising name";
        ids.insert(id);
    }
    EXPECT_EQ(ids.size(), 8u) << "two units must not advertise the same name";
}

TEST(ProvisioningAllowsSec0, Sec0IsOffUnlessExplicitlyRequested) {
    // Spec 8.3 option 3: Sec0 is REJECTED as a default and offered only behind
    // an explicit flag, and only for bench use. An unauthenticated provisioning
    // window is a radio-range takeover, so the default is asserted, not assumed.
    EXPECT_FALSE(ProvisioningAllowsSec0(/*allow_insecure_provisioning=*/false));
    EXPECT_TRUE(ProvisioningAllowsSec0(/*allow_insecure_provisioning=*/true));
}

TEST(BleProvisioningSession, StartWithADerivedPopSucceedsAndExposesBothValues) {
    BleProvisioning p;
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
    ProvSecurity sec;
    ASSERT_TRUE(p.Start(mac, sec));
    EXPECT_TRUE(p.Active());
    EXPECT_EQ(std::strlen(p.Pop()), 6u);
    EXPECT_STREQ(p.AdvertisedName(), "CDEF");
}

TEST(BleProvisioningSession, StopClearsThePopSoAStaleSecretIsNotLeftReadable) {
    BleProvisioning p;
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
    ASSERT_TRUE(p.Start(mac, ProvSecurity{}));
    ASSERT_GT(std::strlen(p.Pop()), 0u);
    p.Stop();
    EXPECT_FALSE(p.Active());
    EXPECT_EQ(std::strlen(p.Pop()), 0u) << "a stopped session must not keep advertising a secret";
}

TEST(BleProvisioningSession, ABenchFixedPopIsSixDigitsAndStillNamesTheUnit) {
    BleProvisioning p;
    p.SetMode(PopMode::kFixedBench);
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
    ASSERT_TRUE(p.Start(mac, ProvSecurity{}));
    EXPECT_STREQ(p.Pop(), "123456");
    // The short name still comes from the MAC, so two bench units stay distinct.
    EXPECT_STREQ(p.AdvertisedName(), "CDEF");
}

TEST(BleProvisioningSession, ANullMacCannotArmASession) {
    BleProvisioning p;
    EXPECT_FALSE(p.Start(nullptr, ProvSecurity{}));
    EXPECT_FALSE(p.Active()) << "an unarmed session must not advertise";
}

TEST(BleProvisioningSession, Sec0IsRefusedUnlessTheFlagPermitsIt) {
    // **The gate was inert: `allow_insecure` was stored and never read**, so a
    // bench build that set it behaved exactly like a Sec1 build while looking like
    // it honoured the choice. Both fields decide something now:
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};

    // Sec1 with no flag: the product default. Armed, and WITH a PoP.
    BleProvisioning sec1;
    ProvSecurity s1;
    ASSERT_TRUE(sec1.Start(mac, s1));
    EXPECT_FALSE(sec1.Sec0());
    EXPECT_EQ(std::strlen(sec1.Pop()), 6u) << "Sec1 requires a Proof-of-Possession";

    // Sec0 requested AND permitted: armed, and with NO PoP -- Sec0 has none.
    BleProvisioning sec0;
    ProvSecurity s0;
    s0.sec1 = false;
    s0.allow_insecure = true;
    ASSERT_TRUE(sec0.Start(mac, s0));
    EXPECT_TRUE(sec0.Sec0());
    EXPECT_EQ(std::strlen(sec0.Pop()), 0u) << "Sec0 has no Proof-of-Possession";
    EXPECT_GT(std::strlen(sec0.AdvertisedName()), 0u)
        << "the short name is still needed to tell bench units apart";

    // Neither mode usable: refused rather than armed with an unchecked PoP.
    BleProvisioning none;
    ProvSecurity neither;
    neither.sec1 = false;
    neither.allow_insecure = false;
    EXPECT_FALSE(none.Start(mac, neither))
        << "no security mode available must refuse, never arm silently";
    EXPECT_FALSE(none.Active());

    // The flag alone is not enough: `ProvisioningAllowsSec0` is the one decision
    // point, so Sec0 cannot be enabled by a code path that skips it.
    EXPECT_FALSE(ProvisioningAllowsSec0(false));
}
