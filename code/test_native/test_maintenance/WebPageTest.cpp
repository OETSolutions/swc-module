#include "Maintenance/BleProvisioning.h"   // PopDerive, for the salt-differs test
#include "Maintenance/WebPage.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

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

TEST(WebPageFind, TheTimeoutCopyDoesNotClaimAnInactivityBehaviourTheFirmwareLacks) {
    // Spec 8.2 and FR-38 describe the close as "5 minutes of INACTIVITY", but the
    // firmware closes on a FIXED deadline from entry -- nothing calls
    // `NoteActivity`, so no request or PoP entry extends the window (open item
    // N-35). The served page claimed the inactivity behaviour anyway, which tells
    // the user a promise the device does not keep, on the one screen where a
    // mid-provision close-out is the symptom. The copy must describe what the
    // firmware DOES, and must not pin a literal duration the config can change.
    const WebAsset *a = WebPageFind("/");
    ASSERT_NE(a, nullptr);
    const std::string body(a->body, a->len);
    EXPECT_EQ(body.find("inactivity"), std::string::npos)
        << "the page must not promise an inactivity timeout the firmware does not implement";
    EXPECT_EQ(body.find("5 minutes"), std::string::npos)
        << "the window is a SETTING (maintenance_timeout_ms); a literal 5 minutes can be wrong";
    EXPECT_NE(body.find("configured timeout"), std::string::npos)
        << "the page must still tell the user the window closes on its own";
}

// The generated header is what the device actually serves; `assets/index.html` is
// what a human edits. Nothing tied them together except `extra_assets.py`, which
// is a DEVICE build hook -- the host suite compiles the checked-in header and
// never runs it, and no CI step regenerated it (the contract has such a step;
// this asset did not). So the two could diverge and the served page would quietly
// stop being the page in the repository. That is the same shape as N-33 (the
// workflows at a path Actions never read): a generator whose output nothing
// checks. The assertions below compare the two BYTES, which is stronger than
// regenerating and diffing because it also catches a hand edit to the header.
TEST(WebPageFind, TheServedPageIsByteForByteThePageInAssets) {
    namespace fs = std::filesystem;

    // Located from this file rather than from the working directory: PlatformIO
    // runs the binary with the project root as cwd, but a bare `./test_native/...`
    // build or a different runner would not.
    fs::path here = fs::path(__FILE__).parent_path();          // test_native/test_maintenance
    fs::path source = here.parent_path().parent_path() / "assets" / "index.html";
    ASSERT_TRUE(fs::exists(source))
        << "cannot find " << source.string() << " -- this test must not silently pass "
        << "by failing to locate the asset it is supposed to be checking";

    std::ifstream in(source, std::ios::binary);
    ASSERT_TRUE(in.good());
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(raw.empty());

    const WebAsset *a = WebPageFind("/");
    ASSERT_NE(a, nullptr);
    const std::string served(a->body, a->len);

    // Mirror the generator's own line handling: drop CR, then terminate every
    // piece of a "\n"-split with "\n". A source ending in a newline therefore
    // yields one extra blank piece, which is what the header really contains --
    // the served page is one byte longer than the file, and that is the current,
    // intended output rather than something to normalise away here.
    std::string expected;
    for (const std::string &piece : [&] {
             std::vector<std::string> v;
             std::string cur;
             for (const char c : raw) {
                 if (c == '\r') continue;
                 if (c == '\n') { v.push_back(cur); cur.clear(); continue; }
                 cur.push_back(c);
             }
             v.push_back(cur);
             return v;
         }()) {
        expected += piece;
        expected += '\n';
    }

    EXPECT_EQ(served, expected)
        << "WebPageAssets.h is stale: run `python3 tools/gen_assets.py` and commit. "
        << "The device serves the header, so this is the page users would get.";
}

// The asset table is a hand-written tail on generated content: `gen_assets.py`
// appends the "/" alias for `index.html` unconditionally, naming that file
// literally. If the page were renamed or removed the alias would name an
// undeclared symbol (a compile error, so that half is safe) -- but the reverse
// is not: adding a SECOND html file produces an entry with no "/" alias and no
// test would notice, so a browser hitting the root would 404 on a device whose
// page exists. This asserts every alias points at a real asset and that the root
// resolves.
TEST(WebPageFind, EveryTableEntryIsResolvableAndTheRootHasAnAlias) {
    for (size_t i = 0; i < kWebAssetCount; ++i) {
        const WebAsset &e = kWebAssets[i];
        ASSERT_NE(e.path, nullptr);
        ASSERT_NE(e.body, nullptr) << "entry " << e.path << " has no body";
        EXPECT_GT(e.len, 0u) << "entry " << e.path << " is empty";
        // Look-up must find exactly the entry that is in the table, by its own
        // path -- a table entry the finder cannot reach is dead weight.
        EXPECT_EQ(WebPageFind(e.path), &e) << "the finder cannot reach " << e.path;
        // The declared length must match the literal, or the server would send a
        // truncated page (or read past it).
        EXPECT_EQ(e.len, std::string(e.body).size()) << "wrong len for " << e.path;
    }
    EXPECT_NE(WebPageFind("/"), nullptr) << "the root must resolve, or the page is unreachable";
}
