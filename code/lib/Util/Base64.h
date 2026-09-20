#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Base64 for the chunked transports: spec 4.2's `config_chunk.data_b64` and
 * spec 9.3's `ota_chunk.data_b64`.
 *
 * Both functions take an explicit output bound and REFUSE rather than truncate.
 * A silent truncation here corrupts a transfer with no error anywhere -- the
 * receiver would see a short chunk and blame its own CRC, which is the worst
 * kind of bug to diagnose over a car's USB link.
 */

// Returns the number of characters written (excluding the NUL), or 0 if the
// result -- plus its NUL -- does not fit in out_len.
//
// Note the collision the callers must know about: a zero-length input also
// returns 0, because it correctly encodes to nothing. Both transports always
// have at least one byte, so this is documented rather than worked around.
size_t Base64Encode(const uint8_t *in, size_t len, char *out, size_t out_len);

// Decodes into `out`, writing the byte count to *out_written. False on anything
// malformed: a length that is not a multiple of 4, a character outside the
// alphabet, bad padding, or output that does not fit.
bool Base64Decode(const char *in, size_t len, uint8_t *out, size_t out_len,
                  size_t *out_written);

// The DECODED payload size for one config-run chunk (spec 4.2).
//
// Sized so the base64-ENCODED form still fits a single NDJSON frame: 512 bytes
// encode to 684 characters, which with the envelope, the offset and separators
// stays under the 1024-byte cap. Sizing this by the decoded length instead --
// or by a round 1024 -- is how a chunked transport ends up unable to send its
// own chunks.
constexpr size_t kConfigWireChunkBytes = 512;
