#include "Util/Sha256.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {
std::string Hex(const std::string &data) {
    char hex[65];
    EXPECT_TRUE(Sha256Hex(reinterpret_cast<const uint8_t *>(data.data()), data.size(), hex));
    return std::string(hex, 64);
}
}  // namespace

// The empty-string digest is a published constant. Pinning it is the cheapest
// possible check that the padding, the length field and the final block are all
// right -- a SHA-256 that is wrong only in the tail still hashes "abc" plausibly.
TEST(Sha256, MatchesThePublishedDigestOfTheEmptyString) {
    EXPECT_EQ(Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, MatchesThePublishedDigestOfAbc) {
    EXPECT_EQ(Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, MatchesThePublishedDigestOfTheLongMultiBlockVector) {
    // 56 chars: the boundary where the length no longer fits the final block, so
    // a second block is required. This is the classic off-by-one in a streaming
    // hash and a single-block vector cannot see it.
    EXPECT_EQ(Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, OneMillionAsIsTheOtherClassicVector) {
    const std::string a(1000000, 'a');
    EXPECT_EQ(Hex(a), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, StreamingInArbitraryChunksMatchesHashingInOneGo) {
    // Chunk boundaries are where a streaming hash breaks. The config run feeds
    // this 512 bytes at a time and the OTA path feeds it whatever the link
    // delivers, so the boundaries are real, not hypothetical.
    std::string data;
    for (int i = 0; i < 10000; ++i) data.push_back(static_cast<char>(i * 31 % 256));
    const std::string whole = Hex(data);
    for (size_t chunk : {1u, 7u, 63u, 64u, 65u, 512u, 4096u}) {
        Sha256Stream s;
        for (size_t off = 0; off < data.size(); off += chunk) {
            const size_t n = std::min(chunk, data.size() - off);
            s.Update(reinterpret_cast<const uint8_t *>(data.data() + off), n);
        }
        uint8_t d[32];
        s.Final(d);
        char hex[65];
        // snprintf, not sprintf: macOS marks sprintf deprecated and -Werror is
        // project-wide, so the plan's original line does not compile here.
        for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
        EXPECT_EQ(std::string(hex, 64), whole) << "chunk size " << chunk;
    }
}

TEST(Sha256, AMalformedHexStringIsRefusedRatherThanBecomingAllZeros) {
    uint8_t out[32];
    EXPECT_FALSE(Sha256FromHex("not-a-hash", out));
    EXPECT_FALSE(Sha256FromHex("aabb", out));            // too short
    EXPECT_FALSE(Sha256FromHex(std::string(65, 'a').c_str(), out));  // too long
    EXPECT_FALSE(Sha256FromHex("zz00000000000000000000000000000000000000000000000000000000000000", out));
    // Valid, so the refusals above are not vacuous.
    EXPECT_TRUE(Sha256FromHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", out));
    EXPECT_EQ(out[0], 0xba);
    EXPECT_EQ(out[31], 0xad);
}
