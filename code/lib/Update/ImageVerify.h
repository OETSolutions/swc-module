#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Util/Sha256.h"

/*
 * The gate an image must pass before it is written to the inactive slot
 * (FR-36, FR-41). Pure logic, fully host-testable -- which is the point, because
 * this gate protects a path that can brick the device.
 *
 * `Sha256Stream` is NOT declared or redefined here: it is Task 14b's
 * `lib/Util/Sha256.h`, consumed unchanged, so the config run (spec 4.2) and the
 * image verify (spec 9.3) provably hash identically. A second implementation is
 * how a device ends up accepting one digest and rejecting the other.
 */

enum class VerifyResult {
    kOk = 0,
    kSizeMismatch,       // the bytes streamed are not the size the manifest declared
    kChecksumMismatch,   // the digest differs
    kTooLarge,           // declared size exceeds the target partition
    kEmpty,              // zero-length image
    kMalformedHash,      // the expected hash is not 64 hex digits
};

// Validates the hash, the size against `max_size`, and rejects an empty image --
// all BEFORE any data is streamed, so an unacceptable image is refused before
// 4 MB of it reaches flash.
VerifyResult ImageVerifyBegin(const char *expected_sha256_hex, size_t expected_size,
                              size_t max_size);

// Feeds one chunk. Calling this before a successful Begin() is refused.
VerifyResult ImageVerifyChunk(const uint8_t *data, size_t len);

// Size first, then digest, and that ORDER is deliberate: a truncated image then
// reports the specific cause rather than the vaguer checksum failure.
VerifyResult ImageVerifyEnd();

// Bytes accepted so far, for a progress display.
size_t ImageVerifyBytesSoFar();

// Abandon a run, so a failed verify cannot leak into the next one.
void ImageVerifyReset();

// Compare two dotted version strings ("1.2.0" vs "1.10.3"). Returns <0, 0 or >0.
//
// Numeric per-component, NOT lexicographic: "1.10.0" is NEWER than "1.9.0", and
// a string compare gets that backwards. A missing component counts as zero, so
// "1.2" and "1.2.0" are equal.
int SemverCompare(const char *a, const char *b);
