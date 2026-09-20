#include "Util/Base64.h"

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Index of a valid alphabet character, or -1. Deliberately rejects everything
// else, including '=' -- padding is handled structurally, not as a character.
int SextetVal(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

size_t Base64Encode(const uint8_t *in, size_t len, char *out, size_t out_len) {
    if (out == nullptr) return 0;
    if (in == nullptr && len != 0) return 0;

    // Four characters per three bytes, rounded up.
    const size_t el = 4 * ((len + 2) / 3);
    if (el + 1 > out_len) return 0;   // the result AND its NUL must both fit

    size_t oi = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                           (static_cast<uint32_t>(in[i + 1]) << 8) |
                           static_cast<uint32_t>(in[i + 2]);
        out[oi++] = kAlphabet[(v >> 18) & 0x3F];
        out[oi++] = kAlphabet[(v >> 12) & 0x3F];
        out[oi++] = kAlphabet[(v >> 6) & 0x3F];
        out[oi++] = kAlphabet[v & 0x3F];
    }

    const size_t rem = len - i;
    if (rem == 1) {
        const uint32_t v = static_cast<uint32_t>(in[i]) << 16;
        out[oi++] = kAlphabet[(v >> 18) & 0x3F];
        out[oi++] = kAlphabet[(v >> 12) & 0x3F];
        out[oi++] = '=';
        out[oi++] = '=';
    } else if (rem == 2) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                           (static_cast<uint32_t>(in[i + 1]) << 8);
        out[oi++] = kAlphabet[(v >> 18) & 0x3F];
        out[oi++] = kAlphabet[(v >> 12) & 0x3F];
        out[oi++] = kAlphabet[(v >> 6) & 0x3F];
        out[oi++] = '=';
    }

    out[oi] = '\0';
    return oi;
}

bool Base64Decode(const char *in, size_t len, uint8_t *out, size_t out_len,
                  size_t *out_written) {
    if (in == nullptr || out == nullptr || out_written == nullptr) return false;
    *out_written = 0;

    if (len % 4 != 0) return false;
    if (len == 0) return true;

    // Padding may only appear in the last group, and only in its last one or two
    // positions. Anything else -- "Y=Jj", "=YWJ" -- is malformed, and catching it
    // structurally is what stops a mid-string '=' from silently decoding as zero.
    size_t pad = 0;
    if (in[len - 1] == '=') {
        pad = 1;
        if (in[len - 2] == '=') pad = 2;
    }
    for (size_t i = 0; i < len - pad; ++i) {
        if (in[i] == '=') return false;
    }

    const size_t nout = (len / 4) * 3 - pad;
    if (nout > out_len) return false;

    // Validate the whole input BEFORE writing anything: a malformed tail must not
    // leave the caller holding a half-decoded buffer it might go on to use.
    for (size_t i = 0; i < len - pad; ++i) {
        if (SextetVal(in[i]) < 0) return false;
    }

    size_t oi = 0;
    for (size_t i = 0; i < len; i += 4) {
        const bool last = (i + 4 == len);
        const size_t nbytes = last ? (3 - pad) : 3;

        int s[4];
        for (int j = 0; j < 4; ++j) {
            const char c = in[i + static_cast<size_t>(j)];
            // '=' only reaches here at the validated pad positions; treat it as
            // zero so the shift below stays well-defined. The pad BITS are not
            // required to be zero: every caller is our own encoder, and a
            // non-canonical tail with a verified digest is harmless.
            s[j] = (c == '=') ? 0 : SextetVal(c);
        }
        const uint32_t v = (static_cast<uint32_t>(s[0]) << 18) |
                           (static_cast<uint32_t>(s[1]) << 12) |
                           (static_cast<uint32_t>(s[2]) << 6) |
                           static_cast<uint32_t>(s[3]);
        for (size_t b = 0; b < nbytes; ++b) {
            out[oi++] = static_cast<uint8_t>((v >> (16 - 8 * b)) & 0xFF);
        }
    }

    *out_written = oi;
    return true;
}
