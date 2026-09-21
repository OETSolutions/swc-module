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

// One dot-separated pre-release identifier, compared per semver 2.0.0 rule 11:
// numeric identifiers compare numerically, numeric sorts BELOW alphanumeric, and
// two alphanumerics compare byte-wise. Returns <0, 0 or >0.
static int PrereleaseIdentCompare(const char *a, size_t na, const char *b, size_t nb) {
    bool a_num = na > 0;
    bool b_num = nb > 0;
    for (size_t i = 0; i < na; ++i) if (!(a[i] >= '0' && a[i] <= '9')) a_num = false;
    for (size_t i = 0; i < nb; ++i) if (!(b[i] >= '0' && b[i] <= '9')) b_num = false;
    if (a_num && b_num) {
        // Skip leading zeros so "01" and "1" compare equal.
        size_t i = 0, j = 0;
        while (i < na && a[i] == '0') ++i;
        while (j < nb && b[j] == '0') ++j;
        const size_t la = na - i, lb = nb - j;
        if (la != lb) return (la < lb) ? -1 : 1;
        const int c = memcmp(a + i, b + j, la);
        return (c < 0) ? -1 : (c > 0 ? 1 : 0);
    }
    if (a_num != b_num) return a_num ? -1 : 1;   // numeric < alphanumeric
    const size_t m = na < nb ? na : nb;
    const int c = memcmp(a, b, m);
    if (c != 0) return (c < 0) ? -1 : 1;
    return (na == nb) ? 0 : ((na < nb) ? -1 : 1);
}

// semver 2.0.0 rule 11: a pre-release sorts BELOW the associated release, and two
// pre-releases compare identifier by identifier. `has_a`/`has_b` say which side
// (if either) carries a '-' suffix; the caller has already established that the
// numeric MAJOR.MINOR.PATCH parts are equal, so this is the whole remaining order.
static int PrereleaseCompare(const char *a, const char *b) {
    const bool has_a = (a != nullptr && *a != '\0');
    const bool has_b = (b != nullptr && *b != '\0');
    if (!has_a && !has_b) return 0;
    if (!has_a) return 1;    // a is the release, b is a pre-release -> a newer
    if (!has_b) return -1;
    while (*a != '\0' || *b != '\0') {
        if (*a == '\0') return -1;   // a ran out first -> fewer identifiers -> older
        if (*b == '\0') return 1;
        const char *ea = a;
        while (*ea != '\0' && *ea != '.') ++ea;
        const char *eb = b;
        while (*eb != '\0' && *eb != '.') ++eb;
        const int c = PrereleaseIdentCompare(a, static_cast<size_t>(ea - a), b,
                                             static_cast<size_t>(eb - b));
        if (c != 0) return c;
        a = (*ea == '.') ? ea + 1 : ea;
        b = (*eb == '.') ? eb + 1 : eb;
    }
    return 0;
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
    // The numeric parts are equal (or missing, treated as zero). If either side
    // carries a pre-release suffix, semver 2.0.0 makes the release NEWER than its
    // pre-release and orders pre-releases among themselves. Without this the
    // comparison returned 0 for "1.3.0-rc1" against "1.3.0", so a device running
    // the pre-release would never be offered the final release -- the exact
    // "release after the tenth minor is invisible forever" failure the numeric
    // compare above exists to prevent, one field over.
    const char *sa = (*pa == '-') ? pa + 1 : ((*pa == '+') ? pa : nullptr);
    const char *sb = (*pb == '-') ? pb + 1 : ((*pb == '+') ? pb : nullptr);
    // A '+' build-metadata suffix is ignored for precedence (semver rule 10).
    if (sa != nullptr && *sa == '+') sa = nullptr;
    if (sb != nullptr && *sb == '+') sb = nullptr;
    return PrereleaseCompare(sa, sb);
}
