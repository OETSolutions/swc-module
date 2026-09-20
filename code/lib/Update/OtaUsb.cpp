#include "Update/OtaUsb.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_ota_ops.h"
#include "esp_partition.h"
#endif

namespace {

bool     run_open_ = false;
size_t   declared_size_ = 0;
size_t   written_ = 0;
OtaResult last_ = OtaResult::kOk;

#ifdef ESP_PLATFORM
const esp_partition_t *target_ = nullptr;
esp_ota_handle_t       handle_ = 0;
#endif

}  // namespace

bool OtaSupported() {
#ifdef ESP_PLATFORM
    return true;
#else
    // The host build has no partitions to write. Saying so is required: a host
    // caller that got kOk would believe an image was installed.
    return false;
#endif
}

OtaResult OtaBegin(size_t image_size, const char *sha256_hex, size_t max_size) {
    if (run_open_) return OtaResult::kAlreadyStarted;

    // Same up-front gate as the WiFi path. `ImageVerifyBegin` owns the hash
    // validation, the size bound and the empty check, so neither path can forget
    // one of them.
    const VerifyResult vr = ImageVerifyBegin(sha256_hex, image_size, max_size);
    if (vr != VerifyResult::kOk) {
        // Map the verifier's reasons onto this path's, preserving the distinction
        // rather than collapsing them into one "bad image".
        switch (vr) {
            case VerifyResult::kTooLarge: return OtaResult::kTooLarge;
            default:                      return OtaResult::kVerifyFailed;
        }
    }

    declared_size_ = image_size;
    written_ = 0;
    last_ = OtaResult::kOk;

#ifdef ESP_PLATFORM
    if (!OtaSupported()) return OtaResult::kNotSupported;
    // The NON-RUNNING slot: writing to the running one would corrupt the
    // executing image.
    target_ = esp_ota_get_next_update_partition(nullptr);
    if (target_ == nullptr) return OtaResult::kNotSupported;
    if (esp_ota_begin(target_, image_size, &handle_) != ESP_OK) {
        return OtaResult::kFlashFailed;
    }
#endif

    run_open_ = true;
    return OtaResult::kOk;
}

OtaResult OtaChunk(const uint8_t *data, size_t len) {
    if (!run_open_) return OtaResult::kNotStarted;
    if (data == nullptr && len != 0) {
        // A malformed frame, and it must CLOSE the run like every other failure
        // here. Returning a failure while staying open leaves the caller with a
        // run the device has already rejected but will keep accepting chunks
        // into, which is not a state any of the other exit paths can produce.
        OtaAbort();
        return OtaResult::kVerifyFailed;
    }

    // Verify and write from the SAME buffer. Two passes over data that could
    // change between them is how a device verifies one image and flashes another.
    const VerifyResult vr = ImageVerifyChunk(data, len);
    if (vr != VerifyResult::kOk) {
        OtaAbort();
        return OtaResult::kVerifyFailed;
    }

#ifdef ESP_PLATFORM
    if (esp_ota_write(handle_, data, len) != ESP_OK) {
        OtaAbort();
        return OtaResult::kFlashFailed;
    }
#endif

    written_ += len;
    return last_;
}

OtaResult OtaEnd() {
    if (!run_open_) return OtaResult::kNotStarted;
    if (written_ != declared_size_) {
        OtaAbort();
        return OtaResult::kVerifyFailed;
    }

    const VerifyResult vr = ImageVerifyEnd();
    if (vr != VerifyResult::kOk) {
        OtaAbort();
        return OtaResult::kVerifyFailed;
    }
    // Verified. Only now may anything be committed.
    return OtaCommit();
}

OtaResult OtaCommit() {
#ifdef ESP_PLATFORM
    if (!run_open_) return OtaResult::kNotStarted;
    if (esp_ota_end(handle_) != ESP_OK) {
        run_open_ = false;
        return OtaResult::kFlashFailed;
    }
    run_open_ = false;
    if (esp_ota_set_boot_partition(target_) != ESP_OK) return OtaResult::kSetBootFailed;
    return OtaResult::kOk;
#else
    // The host build cannot commit. Returning kOk would be a lie a test could
    // build on.
    run_open_ = false;
    return OtaResult::kNotSupported;
#endif
}

void OtaAbort() {
#ifdef ESP_PLATFORM
    if (run_open_ && OtaSupported()) {
        esp_ota_abort(handle_);
    }
#endif
    run_open_ = false;
    ImageVerifyReset();
    written_ = 0;
    declared_size_ = 0;
}

size_t OtaBytesWritten() { return written_; }
bool OtaInProgress() { return run_open_; }
