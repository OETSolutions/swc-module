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
 * TLS is VERIFIED -- never `setInsecure()`. Spec 9.5 records that the reference
 * project shipped with certificate validation disabled and flagged it as a known
 * gap: "SWC must not repeat that." An unverified TLS session makes the manifest
 * and the image attacker-supplied, and the SHA-256 does not help because the hash
 * arrives over the same hijacked channel.
 *
 * **The trust anchor is a PINNED CA (N-62 fixed).** The code sets
 * `cfg.cert_pem = kReleaseCaPem` (`Update/ReleaseCa.h` -- the single root that
 * signs GitHub Releases), NOT IDF's `esp_crt_bundle_attach` default bundle. This
 * is spec 9.5's "pinned CA certificate, not `setInsecure()`" met literally: the
 * device trusts exactly one root, so a different public root (or a CA coerced into
 * issuing for the release host) cannot vouch for the manifest or the image. An
 * earlier revision attached the ~200-root default bundle; that is the defect N-62
 * records. **The pin does NOT self-update**, so it must be renewed before it
 * expires -- `tools/check_release_ca.py` guards the count (fails on expiry, warns
 * within 180 days).
 */

struct OtaWifiProgress {
    size_t bytes_read;
    size_t bytes_total;
};

// Names the verification mechanism the release fetch uses, so a test or a log can
// record it: **`"pinned_ca"` -- a single `cert_pem` trust anchor, NOT the default
// bundle** (N-62 fixed). A caller that wants to assert WHICH root is pinned reads
// `Update/ReleaseCa.h`; this string names the mechanism, not the certificate.
//
// It has NO CALLER anywhere in the tree (grep-confirmed), so the guarantee this
// comment describes is currently asserted by nothing (the gate `check_release_ca.py`
// checks the SOURCE instead).
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
//
// **It has NO CALLER anywhere in the tree (grep-confirmed)**, so nothing checks
// the rule today. It reports that a session was verified, but not against WHAT:
// with N-62 fixed the anchor is a single pinned CA, and this flag does not name
// it, so a future caller must read `Update/ReleaseCa.h`, not this. Wire it to a
// self-test when the `ota_*` path is wired to `OtaUsb` (N-14).
bool OtaWifiLastSessionWasVerified();
