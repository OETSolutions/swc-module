#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Config/ConfigModel.h"

// Structural and semantic checks. Returns false for anything that would make
// classification ambiguous or the device unable to serve input (FR-26).
bool ConfigValidate(const Config &c);

// Is this single action runnable: a known kind, carrying the field its kind
// requires? Declared (not file-local) because Task 11's BindingResolver refuses
// an un-executable action, and two copies of this predicate is exactly the
// two-homes defect this plan keeps re-discovering. Task 11's ActionIsExecutable
// is a thin alias for this, not a re-derivation.
bool ActionIsWellFormed(const Action &a);

// JSON form: the Android-facing and backup form (FR-27). Returns bytes written
// excluding the terminator, or 0 on overflow.
size_t ConfigEncodeJson(const Config &c, char *out, size_t out_len);
bool   ConfigDecodeJson(const char *json, size_t len, Config *out);

// NVS blob form: version header + CRC32 over the JSON payload (FR-23). The
// payload is JSON, the same bytes ConfigEncodeJson produces -- one codec, so the
// two forms cannot drift -- and the CRC is what makes a torn write detectable.
size_t ConfigEncodeBlob(const Config &c, uint8_t *out, size_t out_len);
bool   ConfigDecodeBlob(const uint8_t *in, size_t len, Config *out);

// The header on its own, for callers that need the payload length before
// decoding -- Task 9 reads it to learn how many chunks a stored slot occupies
// without having to decode the whole blob first. Returns the TOTAL blob length
// (header included), or 0 if the bytes are not a well-formed header.
size_t ConfigBlobTotalLength(const uint8_t *in, size_t len);

// CRC-32 (IEEE, reflected, poly 0xEDB88320), over arbitrary bytes.
//
// Declared here rather than kept file-local in the .cpp because it has THREE
// callers and they must agree exactly: ConfigEncodeBlob stamps it, Task 9 checks
// it on load, and Task 15's config_begin carries it for the chunked transfer
// (spec 4.2). Two implementations of a checksum is the two-homes defect with a
// silent failure mode -- a mismatch reports as a corrupt config, not as a bug.
uint32_t Crc32(const uint8_t *data, size_t len);

// Worst-case serialized size, asserted against the NVS budget (spec 10.5).
//
// DEFINED HERE, INLINE, and that is load-bearing rather than stylistic. The tests
// size their encode buffers from it (`constexpr size_t kScratch =
// ConfigMaxSerializedSize();`), which is only legal if the definition is visible
// in that translation unit: a `constexpr` function DECLARED in this header and
// DEFINED in ConfigCodec.cpp does not compile there -- "undefined function cannot
// be used in a constant expression". An earlier revision of this header declared
// both functions and defined them in the .cpp, which would have failed the moment
// Task 8's own tests were written. Inline in the header is the fix.
//
// It is a MEASURED NUMBER, not a cJSON call, for two reasons: cJSON cannot report
// a size without building and printing the tree (neither constexpr nor cheap), and
// this must be usable in a constant expression. The number is spec 3.5's
// measurement -- every string field at its declared width, 2 channels x 16
// buttons, 3 AUX, 32 bindings x 2 actions = 22,407 B of JSON -> 11 chunks.
//
// A number can go stale, so it is gated twice, and both gates are real:
//   * the static_asserts below are compile-time and catch a value that no longer
//     fits two slots, or one small enough to make Task 9's chunked path dead code;
//   * Task 8's SerializedSizeFitsTheNvsPartitionBudget encodes the same two limits
//     and additionally proves the constant is a real bound on the encoder's output.
// **If any width in ConfigModel.h changes, re-measure this and spec 3.5 together.**
// They are one fact with two homes, which is this plan's most common defect.
inline constexpr size_t ConfigMaxSerializedSize() { return 22407; }

// The blob is that JSON preceded by a fixed-size header (BlobHeader, defined in
// ConfigCodec.cpp, which static_asserts its size against this). Task 9 sizes its
// slot buffer from ConfigMaxBlobSize(), so the header's width has exactly one
// home -- a second `+ 16` written by hand in the store is the two-homes defect
// this plan keeps re-finding, and it would be wrong the moment the header grows.
constexpr size_t kBlobHeaderBytes = 16;
inline constexpr size_t ConfigMaxBlobSize() { return ConfigMaxSerializedSize() + kBlobHeaderBytes; }

// The fixed chunk size the store writes. Must be < 4000 to leave entry
// overhead, and is a compile-time constant so the key count is bounded.
// Declared BEFORE ConfigChunkCountFor, which uses it -- order matters here.
constexpr size_t kConfigChunkBytes = 2048;

// How many NVS keys a blob of this size needs, at a fixed chunk size well under
// the 4000-byte single-value cap. A slot is written as `cfg_a_0..n` with the
// count and a per-slot CRC in the header chunk (spec 3.8), because a real config
// is 10-15 KB and CANNOT be one NVS value. Inline for the same reason as above.
inline constexpr int ConfigChunkCountFor(size_t blob_len) {
    return static_cast<int>((blob_len + kConfigChunkBytes - 1) / kConfigChunkBytes);
}

// NVS's usable entry space: 12 pages x 126 entries x 32 B (spec 3.8), and each
// chunk key costs 32 B of metadata + the payload + a 32 B BLOB_IDX entry -- so
// 2,112 B per chunk, NOT 2,048. Counting only the payload understates the budget.
constexpr size_t kUsableEntryBytes   = 32u * 126u * 12u;              // 48,384
constexpr size_t kEntryBytesPerChunk = 32u + kConfigChunkBytes + 32u; // 2,112
constexpr size_t kSequenceKeyBytes   = 32u;

static_assert(ConfigMaxSerializedSize() > 4000u,
              "the worst case must force chunking, or Task 9's chunked path is dead code");
static_assert(2u * static_cast<size_t>(ConfigChunkCountFor(ConfigMaxSerializedSize())) *
                      kEntryBytesPerChunk + kSequenceKeyBytes <= kUsableEntryBytes,
              "two slots + cfg_seq no longer fit the nvs partition: re-measure spec 3.5");
