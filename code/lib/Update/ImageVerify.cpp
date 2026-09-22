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

/*
 * Compare two runs of digits as non-negative integers of UNBOUNDED width, exactly
 * and without overflow. Strips leading zeros (so "01" and "1" compare equal),
 * then compares by digit-count and finally digit-by-digit -- which orders
 * arbitrary-length numerals correctly.
 *
 * **This is not a micro-optimisation for a `long`; it is what makes the result
 * correct and platform-independent.** The previous version accumulated into a
 * `long`, and `long` is 64-bit on the host but **32-bit on xtensa**, so the same
 * version string could compare differently on the device than in the test that
 * blessed it -- and neither was right: a component past 19 digits overflows the
 * host's `long` and past 9 the device's, which is undefined behaviour and in
 * practice wraps. It is reachable with nothing more exotic than a date-stamped
 * tag (`20260923` is 8 digits and already within one digit of the device limit;
 * `202609231` overflows a 32-bit `long`), and `kReleaseVersionLen` is 24, so a
 * manifest the parser accepts can carry a component that overflows either width.
 * A wrapped component can invert an ordering, so the device would refuse a real
 * upgrade or accept a downgrade. Comparing the digits themselves has no width to
 * exceed. Measured pre-fix with UBSan: `99999999999999999999.0.0` against the
 * neighbouring value reported "signed integer overflow: 999999999999999999 * 10
 * cannot be represented in type 'long'".
 */
static int CompareNumericRuns(const char *a, size_t na, const char *b, size_t nb) {
    size_t i = 0, j = 0;
    while (i < na && a[i] == '0') ++i;
    while (j < nb && b[j] == '0') ++j;
    const size_t la = na - i, lb = nb - j;
    if (la != lb) return (la < lb) ? -1 : 1;
    if (la == 0) return 0;   // both are an empty/zero run
    const int c = memcmp(a + i, b + j, la);
    return (c < 0) ? -1 : (c > 0 ? 1 : 0);
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
        // The same unbounded numeric compare the version components use, so
        // "1.3.0-2" vs "1.3.0-10" and a 20-digit build number cannot disagree on
        // width. One home for "compare two numerals".
        return CompareNumericRuns(a, na, b, nb);
    }
    if (a_num != b_num) return a_num ? -1 : 1;   // numeric < alphanumeric
    const size_t m = na < nb ? na : nb;
    const int c = memcmp(a, b, m);
    if (c != 0) return (c < 0) ? -1 : 1;
    return (na == nb) ? 0 : ((na < nb) ? -1 : 1);
}

// semver 2.0.0 rule 11: a pre-release sorts BELOW the associated release, and two
// pre-releases compare identifier by identifier. `na`/`nb` are the pre-release
// LENGTHS (0 = that side is a release, i.e. carries no '-'). Length-bounded
// rather than NUL-terminated so the caller can pass a slice with the '+build'
// metadata already excluded -- rule 10 drops metadata from a pre-release too.
static int PrereleaseCompareN(const char *a, size_t na, const char *b, size_t nb) {
    const bool has_a = na > 0;
    const bool has_b = nb > 0;
    if (!has_a && !has_b) return 0;
    if (!has_a) return 1;    // a is the release, b is a pre-release -> a newer
    if (!has_b) return -1;
    size_t ia = 0, ib = 0;
    while (ia < na || ib < nb) {
        if (ia >= na) return -1;   // a ran out first -> fewer identifiers -> older
        if (ib >= nb) return 1;
        size_t ea = ia;
        while (ea < na && a[ea] != '.') ++ea;
        size_t eb = ib;
        while (eb < nb && b[eb] != '.') ++eb;
        const int c = PrereleaseIdentCompare(a + ia, ea - ia, b + ib, eb - ib);
        if (c != 0) return c;
        ia = (ea < na && a[ea] == '.') ? ea + 1 : ea;
        ib = (eb < nb && b[eb] == '.') ? eb + 1 : eb;
    }
    return 0;
}

int SemverCompare(const char *a, const char *b) {
    if (a == nullptr || b == nullptr) return 0;
    const char *pa = a;
    const char *pb = b;
    // Semver 2.0.0's three numeric components: major.minor.patch.
    //
    // **This loop used to run FOUR times and that off-by-one was a silent
    // "never offer the update".** With 4 iterations it exited with both pointers
    // still ON a 4th component, and the code below compares only the pre-release
    // and the build metadata -- neither of which a plain "1.2.3.4" has. So
    // `SemverCompare("1.2.3.4.5", "1.2.3.4.6")` returned **0**: two different
    // versions read as EQUAL and the device is told it is up to date forever.
    // Measured before the fix, against the real function.
    //
    // Semver has no 4th field, so a 4-component string is not strictly valid --
    // but it is exactly what a date- or build-stamped tag produces, and the
    // pipeline publishes whatever the maintainer tags. The extension loop below
    // therefore keeps comparing numeric components rather than dropping them.
    for (int i = 0; i < 3; ++i) {
        // Parse one numeric component from each side; anything non-numeric ends
        // that component at zero rather than aborting the whole comparison, so
        // "1.2.0" against "1.2" compares equal. The component is compared as the
        // DIGITS it is, not as an accumulated integer -- see
        // `CompareNumericRuns`, which is also what keeps this correct on xtensa
        // where a `long` is 32 bits.
        const char *sa_ = pa;
        while (*pa >= '0' && *pa <= '9') ++pa;
        const char *sb_ = pb;
        while (*pb >= '0' && *pb <= '9') ++pb;
        const int c = CompareNumericRuns(sa_, static_cast<size_t>(pa - sa_),
                                         sb_, static_cast<size_t>(pb - sb_));
        if (c != 0) return c;
        // The dot is consumed only BETWEEN components. Consuming it on the last
        // iteration too would step past a 4th component's separator, which is
        // how the extension below would then fail to see it.
        if (i < 2) {
            if (*pa == '.') ++pa;
            if (*pb == '.') ++pb;
        }
    }

    // A 4th or later numeric component, compared numerically rather than
    // truncated. "1.2.3.9" must sort BELOW "1.2.3.10" -- a text compare would
    // invert that pair -- so this repeats the numeric step, unbounded, for as
    // long as EITHER side still has a dotted component. A component one side
    // does not have reads as zero, so "1.2.3" still equals "1.2.3.0".
    for (;;) {
        if (*pa != '.' && *pb != '.') break;
        if (*pa == '.') ++pa;
        if (*pb == '.') ++pb;
        const char *sa_ = pa;
        while (*pa >= '0' && *pa <= '9') ++pa;
        const char *sb_ = pb;
        while (*pb >= '0' && *pb <= '9') ++pb;
        const int c = CompareNumericRuns(sa_, static_cast<size_t>(pa - sa_),
                                         sb_, static_cast<size_t>(pb - sb_));
        if (c != 0) return c;
    }

    // The numeric parts are equal (or missing, treated as zero). If either side
    // carries a pre-release suffix, semver 2.0.0 makes the release NEWER than its
    // pre-release and orders pre-releases among themselves. Without this the
    // comparison returned 0 for "1.3.0-rc1" against "1.3.0", so a device running
    // the pre-release would never be offered the final release -- the exact
    // "release after the tenth minor is invisible forever" failure the numeric
    // compare above exists to prevent, one field over.
    const char *sa = (*pa == '-') ? pa + 1 : nullptr;
    const char *sb = (*pb == '-') ? pb + 1 : nullptr;
    // A '+' build-metadata suffix is ignored for precedence (semver rule 10).
    // It must be dropped from a PRE-release too, not only from a bare release:
    // "1.3.0-rc+build" kept "+build" inside the compared prerelease and read as
    // newer than "1.3.0-rc". The suffix runs to end-of-string, so the truncation
    // is done by bounded length rather than by writing a terminator.
    size_t na = (sa != nullptr) ? strcspn(sa, "+") : 0;
    size_t nb = (sb != nullptr) ? strcspn(sb, "+") : 0;
    return PrereleaseCompareN(sa, na, sb, nb);
}
