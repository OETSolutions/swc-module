#include "Util/Sha256.h"

#include <string.h>

namespace {

// FIPS 180-4, section 4.2.2: the first 32 bits of the fractional parts of the
// cube roots of the first 64 primes.
constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t Ror(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

void Compress(uint32_t st[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t big_s1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + big_s1 + ch + kK[i] + w[i];
        const uint32_t big_s0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = big_s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

Sha256Stream::Sha256Stream() : bitlen_(0), buflen_(0), finalized_(false) {
    state_[0] = 0x6a09e667u; state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u; state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu; state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu; state_[7] = 0x5be0cd19u;
    memset(buf_, 0, sizeof(buf_));
}

void Sha256Stream::Update(const uint8_t *data, size_t len) {
    if (finalized_ || len == 0) return;
    bitlen_ += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        size_t take = sizeof(buf_) - buflen_;
        if (take > len) take = len;
        memcpy(buf_ + buflen_, data, take);
        buflen_ += take;
        data += take;
        len -= take;
        if (buflen_ == sizeof(buf_)) {
            Compress(state_, buf_);
            buflen_ = 0;
        }
    }
}

void Sha256Stream::Final(uint8_t out[32]) {
    if (!finalized_) {
        const uint64_t bits = bitlen_;
        buf_[buflen_++] = 0x80;
        if (buflen_ > 56) {
            while (buflen_ < sizeof(buf_)) buf_[buflen_++] = 0;
            Compress(state_, buf_);
            buflen_ = 0;
        }
        while (buflen_ < 56) buf_[buflen_++] = 0;
        for (int i = 7; i >= 0; --i) {
            buf_[buflen_++] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
        }
        Compress(state_, buf_);
        buflen_ = 0;
        finalized_ = true;
    }
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<uint8_t>(state_[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
}

bool Sha256Hex(const uint8_t *data, size_t len, char out_hex[65]) {
    if (out_hex == nullptr) return false;
    if (data == nullptr && len != 0) return false;

    Sha256Stream s;
    s.Update(data, len);
    uint8_t digest[32];
    s.Final(digest);

    static const char kDigits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out_hex[i * 2]     = kDigits[digest[i] >> 4];
        out_hex[i * 2 + 1] = kDigits[digest[i] & 0x0F];
    }
    out_hex[64] = '\0';
    return true;
}

bool Sha256FromHex(const char *hex, uint8_t out[32]) {
    if (hex == nullptr || out == nullptr) return false;

    // Bounded scan rather than strlen: a caller handing us a short string (the
    // tests do, deliberately) must not send us reading past its NUL.
    size_t n = 0;
    while (n <= 64 && hex[n] != '\0') ++n;
    if (n != 64) return false;

    uint8_t tmp[32];
    for (int i = 0; i < 32; ++i) {
        const int hi = HexVal(hex[i * 2]);
        const int lo = HexVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        tmp[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    // Only write on success: a rejected parse must leave the caller's buffer
    // untouched, not half-filled.
    memcpy(out, tmp, sizeof(tmp));
    return true;
}
