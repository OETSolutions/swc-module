#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Update/ImageVerify.h"

/*
 * OTA over USB (spec 9.3), driven by the `ota_begin`/`ota_chunk`/`ota_end` frames
 * the CommandRouter carries.
 *
 * **Both OTA paths share ONE gate.** The check that an image may be installed is
 * `ImageVerify` (Task 17) and the decision to switch the boot partition is
 * `OtaCommit`; neither the WiFi nor the USB path re-implements either. Two
 * implementations of "is this image safe" is how one path ends up weaker than the
 * other, and the weaker one is the one an attacker uses.
 */

enum class OtaResult {
    kOk = 0,
    kNotStarted,        // a chunk arrived with no run open
    kAlreadyStarted,     // a second ota_begin while one is open
    kTooLarge,
    kVerifyFailed,      // see ImageVerify for which check
    kFlashFailed,
    kSetBootFailed,
    kNotSupported,      // no OTA support compiled in for this target
};

/*
 * The image-size ceiling: one app slot, 0x1E0000 = 1920 KiB (`partitions.csv`).
 *
 * ONE definition, because three callers need the same bound and they must not
 * disagree: the USB path (a peer-declared `size`), the WiFi path (`size_bytes`
 * from the manifest), and `check_size.py` (the built `firmware.bin`). A second
 * copy is how one path accepts an image another refuses -- the classic
 * one-side-weaker defect the shared verify gate exists to prevent.
 *
 * It is a COMPILE-TIME constant rather than a runtime `partition->size` read
 * because the host build has no partition table, and the bound is what the host
 * tests exercise. On the device the slot really is this size, so the two agree;
 * `esp_ota_begin` independently refuses anything larger, so this is the earlier
 * of two checks rather than the only one.
 *
 * Written as a single literal (1,920 × 1024 = 1,966,080) because the
 * `check_app_limits.py` gate reads this declaration to pin the app's mirror; a
 * product expression there would not parse.
 */
constexpr size_t kAppSlotBytes = 1966080;

// Starts a run. Validates the size and hash up front, exactly as ImageVerify
// does, so a bad image is refused before anything is written.
OtaResult OtaBegin(size_t image_size, const char *sha256_hex, size_t max_size);

// One chunk, appended to the inactive slot and fed to the verifier in the same
// call, so what is written and what is verified are the same bytes -- not two
// passes over data that could differ.
OtaResult OtaChunk(const uint8_t *data, size_t len);

// Verifies, then switches the boot partition. Nothing is installed unless the
// verify passed, and the switch happens only here.
OtaResult OtaEnd();

// Abandon a run without switching anything. Used on disconnect and on any error.
void OtaAbort();

size_t OtaBytesWritten();
bool OtaInProgress();

/*
 * The single commit point. Declared here because both paths call it and it must
 * exist exactly once: it is the line that decides whether a bad image becomes the
 * boot image.
 */
OtaResult OtaCommit();

// Whether this build has OTA compiled in at all. On the host target it does not,
// and a caller must be told that rather than silently getting kOk.
bool OtaSupported();
