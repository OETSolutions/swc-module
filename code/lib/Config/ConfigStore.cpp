#include "Config/ConfigStore.h"

#include <stdio.h>
#include <string.h>

namespace {

// The sequence key and the two slot names. The newest slot is DERIVED from the
// sequence's parity rather than stored -- see the header.
constexpr const char *kSeqKey = "cfg_seq";

// A slot's chunk keys are `cfg_<slot>_<i>`. Formatted rather than taken from a
// table, because the count is bounded by ConfigChunkCountFor() and a table would
// need a fixed maximum free to disagree with it.
void ChunkKey(char *out, size_t out_len, char slot, int index) {
    snprintf(out, out_len, "cfg_%c_%d", slot, index);
}

// The slot a save writes when the sequence it is about to store is `seq`.
// Odd -> 'a', even -> 'b'. seq 0 is never stored (0 means "nothing yet"), so the
// first save lands in 'a' and the newest slot is always SlotForSequence(seq).
char SlotForSequence(uint32_t seq) {
    return (seq % 2u == 1u) ? 'a' : 'b';
}

char OtherSlot(char slot) {
    return slot == 'a' ? 'b' : 'a';
}

// Sized from the codec's own bound, not from a literal: a buffer that is too
// small here would truncate a config silently, and the bound is the one place
// that knows how big a config can be.
uint8_t g_blob[ConfigMaxBlobSize()];

}  // namespace

ConfigStore::ConfigStore(IHAL *hal) : hal_(hal) {}

bool ConfigStore::WriteSlot(char slot, const uint8_t *blob, size_t blob_len) {
    const int chunks = ConfigChunkCountFor(blob_len);
    for (int i = 0; i < chunks; ++i) {
        const size_t off = static_cast<size_t>(i) * kConfigChunkBytes;
        const size_t remaining = blob_len - off;
        const size_t n = remaining < kConfigChunkBytes ? remaining : kConfigChunkBytes;
        char key[16];
        ChunkKey(key, sizeof(key), slot, i);
        if (hal_->nvs_set(hal_->ctx, key, blob + off, n) != 0) return false;
    }
    return true;
}

bool ConfigStore::ReadSlot(char slot, uint8_t *blob, size_t blob_cap, size_t *out_len) {
    // Chunk 0 carries the header, and the header carries the payload length, so
    // the slot needs no separate length key. That is why the header is the FIRST
    // chunk and not the last: the count of remaining chunks follows from it.
    char key[16];
    ChunkKey(key, sizeof(key), slot, 0);
    // Chunk 0 is read INTO THE CALLER'S BUFFER, not a local one. Its stored value
    // is a whole chunk (up to kConfigChunkBytes), and nvs_get_blob does not
    // truncate: given a buffer smaller than the value it returns
    // ESP_ERR_NVS_INVALID_LENGTH, which is not NOT_FOUND, so the HAL maps it to
    // -1 and this read fails. Reading the header's 16 bytes was exactly that, and
    // it made Load return kFellBackToDefaults on a real device for EVERY config
    // while passing on the host, where the mock truncated. A chunk never exceeds
    // a full chunk, so `blob_cap` (which the caller already guaranteed covers the
    // whole blob) is always large enough here, and no per-call chunk buffer is
    // needed on the device's small stack.
    const int hn = hal_->nvs_get(hal_->ctx, key, blob, blob_cap);
    if (hn < static_cast<int>(kBlobHeaderBytes)) return false;

    const size_t total = ConfigBlobTotalLength(blob, static_cast<size_t>(hn));
    if (total == 0 || total > blob_cap) return false;
    if (static_cast<size_t>(hn) > total) return false;

    const int chunks = ConfigChunkCountFor(total);
    // Chunk 0 is already in place from the read above; fetch the rest.
    size_t got = static_cast<size_t>(hn);
    for (int i = 1; i < chunks; ++i) {
        const size_t off = static_cast<size_t>(i) * kConfigChunkBytes;
        const size_t remaining = total - off;
        const size_t want = remaining < kConfigChunkBytes ? remaining : kConfigChunkBytes;
        ChunkKey(key, sizeof(key), slot, i);
        // A SHORT chunk is a slot failure, not a short config. A partial chunk
        // set must never decode, which is why every chunk's full length is
        // checked here rather than left to the CRC.
        const int n = hal_->nvs_get(hal_->ctx, key, blob + off, want);
        if (n != static_cast<int>(want)) return false;
        got += want;
    }
    if (got != total) return false;
    *out_len = total;
    return true;
}

ConfigLoadResult ConfigStore::Load(Config *out) {
    if (hal_ == nullptr || hal_->nvs_get == nullptr || out == nullptr) {
        return ConfigLoadResult::kFellBackToDefaults;
    }

    uint32_t seq = 0;
    const int seq_read = hal_->nvs_get(hal_->ctx, kSeqKey, &seq, sizeof(seq));
    // Absent is NOT corrupt. "Never configured" is what selects pass-through mode
    // (FR-25), and collapsing it into kFellBackToDefaults would silently change
    // the device's behaviour on a fresh board.
    if (seq_read != static_cast<int>(sizeof(seq)) || seq == 0) {
        return ConfigLoadResult::kNoConfig;
    }

    // The newest slot is the one the save that stored `seq` wrote, which is the
    // slot the next save will overwrite.
    const char newest = SlotForSequence(seq);
    size_t len = 0;
    if (ReadSlot(newest, g_blob, sizeof(g_blob), &len)) {
        Config c{};
        if (ConfigDecodeBlob(g_blob, len, &c)) {
            *out = c;
            loaded_seq_ = seq;
            return ConfigLoadResult::kLoaded;
        }
    }

    // The sequence advanced, so the newest slot was written whole once -- but its
    // CRC no longer holds. That is rot, not an interrupted write, which is why it
    // gets its own result: the torn cases below never advance the sequence, so
    // they read as kLoaded with the previous config.
    const char older = OtherSlot(newest);
    if (ReadSlot(older, g_blob, sizeof(g_blob), &len)) {
        Config c{};
        if (ConfigDecodeBlob(g_blob, len, &c)) {
            *out = c;
            loaded_seq_ = seq - 1u;
            return ConfigLoadResult::kRecoveredFromBackup;
        }
    }

    return ConfigLoadResult::kFellBackToDefaults;
}

bool ConfigStore::Save(const Config &c) {
    if (hal_ == nullptr || hal_->nvs_set == nullptr || hal_->nvs_get == nullptr) return false;

    const size_t blob_len = ConfigEncodeBlob(c, g_blob, sizeof(g_blob));
    if (blob_len == 0) return false;

    uint32_t seq = 0;
    const int seq_read = hal_->nvs_get(hal_->ctx, kSeqKey, &seq, sizeof(seq));
    if (seq_read != static_cast<int>(sizeof(seq))) seq = 0;   // absent: start from nothing

    const uint32_t next_seq = seq + 1u;
    const char target = SlotForSequence(next_seq);

    // The whole slot lands BEFORE the sequence that would promote it. That
    // ordering is the entire protocol: until cfg_seq advances this slot is not
    // the newest, so a tear anywhere above leaves the previous config in charge.
    if (!WriteSlot(target, g_blob, blob_len)) return false;

    const int wrote = hal_->nvs_set(hal_->ctx, kSeqKey, &next_seq, sizeof(next_seq));
    if (wrote != 0) return false;   // the payload landed, but it is never promoted

    loaded_seq_ = next_seq;
    return true;
}
