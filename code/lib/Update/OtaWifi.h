#pragma once

#include <stddef.h>

#include "Config/ConfigModel.h"
#include "Update/OtaUsb.h"
#include "Update/ReleaseCheck.h"

/*
 * OTA over WiFi (spec 9.5): fetch the manifest, decide with `ReleaseCheck`, then
 * stream the image through the SAME gate the USB path uses (`OtaBegin`/
 * `OtaChunk`/`OtaEnd`), and commit through the SAME `OtaCommit`.
 *
 * **TLS is verified against a PEM CA in flash -- never `setInsecure()`.** Spec 9.5
 * records that the reference project shipped with certificate validation disabled
 * and flagged it as a known gap: "SWC must not repeat that." An unverified TLS
 * session makes the manifest and the image attacker-supplied, and the SHA-256 does
 * not help because the hash arrives over the same hijacked channel.
 *
 * The device has no filesystem (spec 9.2), so the CA is a compiled-in PEM.
 */

struct OtaWifiProgress {
    size_t bytes_read;
    size_t bytes_total;
};

// The CA bundle identifier IDF's `esp-tls` uses for its built-in roots. Pinned by
// name so a future edit cannot silently swap in `setInsecure`.
const char *OtaWifiCaBundleAttach();

// Fetches and parses the manifest. Returns the release decision; `out` receives
// the parsed fields on anything other than `kMalformed`.
ReleaseCheckResult OtaWifiCheck(const char *manifest_url, const char *current_version,
                                ReleaseInfo *out);

// Streams the image at `info.url` through the verifier and, on success, commits
// it as the boot partition. The progress callback is optional.
OtaResult OtaWifiInstall(const ReleaseInfo &info, size_t max_size,
                         void (*progress)(void *ctx, const OtaWifiProgress &), void *ctx);

// True only if the last install used a verified TLS session. Exposed so the
// "never setInsecure" rule is checkable at runtime rather than only by reading
// the source -- the failure it guards against is a silent config change.
bool OtaWifiLastSessionWasVerified();
