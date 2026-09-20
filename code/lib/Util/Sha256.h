#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Streaming SHA-256, one implementation for host and device.
 *
 * **Why this is not a mbedTLS wrapper.** The plan's original intent was to call
 * mbedTLS on both sides so the config digest (spec 4.2) and the image digest
 * (spec 9.3) are provably one computation. That does not work here: IDF 5.5.5
 * bundles mbedTLS 3.6.6, and the PlatformIO registry's newest standalone mbedTLS
 * is 3.6.2 -- a third-party republish, while upstream Mbed-TLS ships no
 * library.json to git-pin instead (the reason cJSON next door IS git-pinned).
 * A registry dependency would therefore put the HOST on a different build of the
 * hashing code than the device ships, which is the exact divergence this task
 * exists to prevent -- and it would hand the host a republish, not the source.
 *
 * So the implementation lives here and both builds compile THIS file. The other
 * end of both digests is the Android app (Java MessageDigest("SHA-256")), so what
 * either side must conform to is FIPS 180-4, not any one library. The tests pin
 * the published vectors, including the multi-block boundary and the one-million
 * case; that is what makes the conformance checkable rather than asserted.
 */

class Sha256Stream {
public:
    Sha256Stream();

    void Update(const uint8_t *data, size_t len);

    // Writes the 32-byte digest. Idempotent: calling it twice yields the same 32
    // bytes, and calling Update() after it is a no-op. Reuse requires constructing
    // a new stream -- reusing a finalized hash silently is how a plausible wrong
    // digest gets produced.
    void Final(uint8_t out[32]);

private:
    uint32_t state_[8];
    uint64_t bitlen_;
    uint8_t  buf_[64];
    size_t   buflen_;
    bool     finalized_;
};

// Convenience: the hex digest of a whole buffer. `out_hex` must hold 65 bytes
// (64 digits + NUL).
bool Sha256Hex(const uint8_t *data, size_t len, char out_hex[65]);

// Parses exactly 64 hex digits into 32 bytes. False for anything else -- a
// malformed digest must never silently become all zeros, which would make every
// comparison against it accidentally succeed or fail depending on direction.
bool Sha256FromHex(const char *hex, uint8_t out[32]);
