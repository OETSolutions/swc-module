#include "Update/ImageVerify.h"

#include <string.h>

namespace {

bool run_open_ = false;
Sha256Stream stream_;
uint8_t expected_[32];
size_t expected_size_ = 0;
size_t bytes_ = 0;

}  // namespace

VerifyResult ImageVerifyBegin(const char *expected_sha256_hex, size_t expected_size,
                              size_t max_size) {
    ImageVerifyReset();

    if (expected_size == 0) return VerifyResult::kEmpty;
    // Refuse up front rather than after streaming megabytes into flash. The two
    // checks are ordered so an oversized image is never mistaken for an empty or
    // malformed one.
    if (expected_size > max_size) return VerifyResult::kTooLarge;
    // Uses Task 14b's parser rather than a second hex reader. A hash that cannot
    // be parsed is REFUSED, never treated as all-zeros: an unparseable hash
    // silently becoming "no check" is how a verification system becomes
    // decorative.
    if (!Sha256FromHex(expected_sha256_hex, expected_)) return VerifyResult::kMalformedHash;

    expected_size_ = expected_size;
    bytes_ = 0;
    run_open_ = true;
    return VerifyResult::kOk;
}

VerifyResult ImageVerifyChunk(const uint8_t *data, size_t len) {
    if (!run_open_) return VerifyResult::kMalformedHash;
    if (data == nullptr && len != 0) return VerifyResult::kMalformedHash;
    stream_.Update(data, len);
    bytes_ += len;
    return VerifyResult::kOk;
}

VerifyResult ImageVerifyEnd() {
    if (!run_open_) return VerifyResult::kMalformedHash;

    // Size FIRST. A truncated image has a wrong digest too, so checking the
    // digest first would report the vaguer of the two causes.
    if (bytes_ != expected_size_) {
        run_open_ = false;
        return VerifyResult::kSizeMismatch;
    }

    uint8_t got[32];
    stream_.Final(got);
    run_open_ = false;
    // Constant-time-ish compare is not needed here (both sides are public), but
    // comparing all 32 bytes rather than stopping at the first difference keeps
    // the function's cost independent of where they differ.
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= static_cast<uint8_t>(got[i] ^ expected_[i]);
    return diff == 0 ? VerifyResult::kOk : VerifyResult::kChecksumMismatch;
}

size_t ImageVerifyBytesSoFar() { return bytes_; }

void ImageVerifyReset() {
    stream_ = Sha256Stream();
    memset(expected_, 0, sizeof(expected_));
    expected_size_ = 0;
    bytes_ = 0;
    run_open_ = false;
}

int SemverCompare(const char *a, const char *b) {
    if (a == nullptr || b == nullptr) return 0;
    const char *pa = a;
    const char *pb = b;
    for (int i = 0; i < 4; ++i) {
        // Parse one numeric component from each side; anything non-numeric ends
        // that component at zero rather than aborting the whole comparison, so
        // "1.2.0" against "1.2" compares equal.
        long va = 0;
        long vb = 0;
        while (*pa >= '0' && *pa <= '9') va = va * 10 + (*pa++ - '0');
        while (*pb >= '0' && *pb <= '9') vb = vb * 10 + (*pb++ - '0');
        if (va != vb) return (va < vb) ? -1 : 1;
        if (*pa == '.') ++pa;
        if (*pb == '.') ++pb;
        if (*pa == '\0' && *pb == '\0') return 0;
    }
    return 0;
}
