#include "Maintenance/BleProvisioning.h"   // PopDerive, for the salt-differs test
#include "Maintenance/WebPage.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

TEST(WebTokenDerive, IsDeterministicAndPerDevice) {
    // The user copies the token off the USB screen, so the two derivations must
    // agree; and it must differ per unit, or one device's token opens another's
    // maintenance page.
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
    char a[32], b[32];
    ASSERT_TRUE(WebTokenDerive(mac, a, sizeof(a)));
    ASSERT_TRUE(WebTokenDerive(mac, b, sizeof(b)));
    EXPECT_STREQ(a, b);

    std::set<std::string> seen;
    for (uint8_t i = 0; i < 16; ++i) {
        uint8_t m[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, i};
        char t[32];
        ASSERT_TRUE(WebTokenDerive(m, t, sizeof(t)));
        seen.insert(t);
    }
    EXPECT_EQ(seen.size(), 16u);
}

TEST(WebTokenDerive, IsNotTheBlePopEvenThoughBothComeFromTheMac) {
    // Two salts, two secrets. If the web token were the PoP -- or a prefix of it
    // -- then a token leaked into a browser history would be part of the
    // provisioning secret.
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
    char token[32];
    ASSERT_TRUE(WebTokenDerive(mac, token, sizeof(token)));
    EXPECT_EQ(std::strlen(token), kWebTokenLen);

    // Different lengths alone would be weak evidence, so this asserts the bytes
    // are not a prefix of the PoP's derivation either -- i.e. the salt differs.
    char pop[32];
    ASSERT_TRUE(PopDerive(mac, pop, sizeof(pop)));
    EXPECT_STRNE(token, pop);
    EXPECT_NE(std::string(token).substr(0, 6), std::string(pop));
}

TEST(WebTokenDerive, RefusesWhenTheBufferIsTooSmall) {
    // A truncated token is worse than a refusal: it would be accepted by nothing
    // and the failure would look like a wrong token.
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    char tiny[kWebTokenLen];   // one short of what is needed with the NUL
    EXPECT_FALSE(WebTokenDerive(mac, tiny, sizeof(tiny)));
    char exact[kWebTokenLen + 1];
    EXPECT_TRUE(WebTokenDerive(mac, exact, sizeof(exact)));
}

TEST(WebTokenMatches, ComparesExactStringsOnly) {
    EXPECT_TRUE(WebTokenMatches("ABCDEF123456", "ABCDEF123456"));
    EXPECT_FALSE(WebTokenMatches("ABCDEF123456", "ABCDEF123457"));
    EXPECT_FALSE(WebTokenMatches("", "ABCDEF123456"));
    EXPECT_FALSE(WebTokenMatches("ABCDEF1234567", "ABCDEF123456")) << "a longer one is not a match";
    EXPECT_FALSE(WebTokenMatches("ABCDEF12345", "ABCDEF123456")) << "nor is a prefix";
    EXPECT_FALSE(WebTokenMatches(nullptr, "ABCDEF123456"));
    EXPECT_FALSE(WebTokenMatches("ABCDEF123456", nullptr));
}

TEST(WebTokenMatches, AnEmptyExpectedTokenNeverMatches) {
    // A device that somehow had no token must not accept an empty one from the
    // network -- that would be an unauthenticated page, which spec 8.4 forbids.
    EXPECT_FALSE(WebTokenMatches("", ""));
    EXPECT_FALSE(WebTokenMatches("anything", ""));
}

TEST(WebPageFind, ServesTheIndexForRootAndByNameButNotForAnythingElse) {
    // The 404 case matters: serving the index page for every URL would make a
    // typo'd API call return HTML, so the client would parse a page as JSON.
    EXPECT_NE(WebPageFind("/"), nullptr);
    EXPECT_NE(WebPageFind("/index.html"), nullptr);
    EXPECT_EQ(WebPageFind("/api/status"), nullptr);
    EXPECT_EQ(WebPageFind("/etc/passwd"), nullptr);
    EXPECT_EQ(WebPageFind(nullptr), nullptr);
}

TEST(WebPageFind, TheServedPageIsTheRealOneAndIsDeclaredHtml) {
    const WebAsset *a = WebPageFind("/");
    ASSERT_NE(a, nullptr);
    EXPECT_GT(a->len, 0u);
    // The page must actually be the maintenance page rather than an empty stub:
    // a placeholder that served 200 with no content would look like a working
    // feature until someone opened it.
    EXPECT_NE(std::string(a->body, a->len).find("SWC adapter"), std::string::npos);
    EXPECT_NE(std::string(WebPageContentType()).find("text/html"), std::string::npos);
}
