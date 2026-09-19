#pragma once

#include <stdint.h>

#include "Config/ConfigCodec.h"
#include "HAL/IHAL.h"

enum class ConfigLoadResult {
    kLoaded,                 // the newest valid slot
    kNoConfig,               // nothing stored at all -> pass-through mode (FR-25)
    kRecoveredFromBackup,    // the newest slot was torn; the other was used
    kFellBackToDefaults,     // no slot yielded a usable config (FR-24)
};

/*
 * Two alternating slots with a monotonic sequence, so the older copy is always
 * intact while the newer one is written. A torn write is detected by the blob
 * CRC (Task 8), not by a length guess.
 *
 * Sequence is stored under its own key and written LAST, so a config only
 * becomes the "newest" once its payload has completely landed.
 *
 * The newest slot is DERIVED from the sequence's parity rather than stored. A
 * cached slot index would be a second home for a fact the sequence already
 * determines, and the two would drift the first time a write failed partway.
 */
class ConfigStore {
public:
    explicit ConfigStore(IHAL *hal);

    ConfigLoadResult Load(Config *out);

    // Returns false without touching the previously stored config on any error.
    bool Save(const Config &c);

    uint32_t LoadedSequence() const { return loaded_seq_; }

private:
    // A slot is `cfg_<slot>_0..n`. Both helpers report the slot's total blob
    // length through *out_len so the caller does not re-read the header chunk.
    bool ReadSlot(char slot, uint8_t *blob, size_t blob_cap, size_t *out_len);
    bool WriteSlot(char slot, const uint8_t *blob, size_t blob_len);

    IHAL *hal_;
    uint32_t loaded_seq_ = 0;
};
