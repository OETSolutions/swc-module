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
 * **The trust anchor is a PINNED CA (N-62 fixed, N-92 corrected).** The code sets
 * `cfg.cert_pem = kReleaseCaPem` (`Update/ReleaseCa.h` -- the two roots the GitHub
 * release chain traverses: Sectigo E46 for `github.com` and ISRG Root X1 for the
 * `release-assets.` redirect target), NOT IDF's `esp_crt_bundle_attach` default
 * bundle. This is spec 9.5's "pinned CA certificate, not `setInsecure()`" met
 * literally: the device trusts only those roots, so a different public root (or a
 * CA coerced into issuing for the release host) cannot vouch for the manifest or
 * the image. An earlier revision attached the ~200-root default bundle; that is the
 * defect N-62 records. **N-92 corrected the count from one root to two:** the
 * release URL 302-redirects to a CDN host signed by a DIFFERENT CA, and
 * `esp_http_client` reuses the `cert_pem` across the redirect, so a single-root pin
 * died on hop 2. **The pin does NOT self-update**, so it must be renewed before it
 * expires -- `tools/check_release_ca.py` guards the count, the identities, and both
 * expiries (fails on expiry, warns within 180 days).
 */

struct OtaWifiProgress {
    size_t bytes_read;
    size_t bytes_total;
};

// Names the verification mechanism the release fetch uses, so a test or a log can
// record it: **`"pinned_ca"` -- a `cert_pem` trust anchor (two roots: the release
// host and its redirect target), NOT the default bundle** (N-62 fixed, N-92
// corrected). A caller that wants to assert WHICH roots are pinned reads
// `Update/ReleaseCa.h`; this string names the mechanism, not the certificates.
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
// with N-62/N-92 fixed the anchors are the pinned roots in `Update/ReleaseCa.h`,
// and this flag does not name them, so a future caller must read that header, not
// this. Wire it to a self-test when the `ota_*` path is wired to `OtaUsb` (N-14).
bool OtaWifiLastSessionWasVerified();

// Why the last `OtaWifiCheck` returned `kMalformed` (empty string when it did not
// fail, or when it has not been called). N-85 recorded that the bare
// `kMalformed` result "names nothing near the cause": a TLS handshake failure
// caused by an unset clock, a DNS failure, a non-200 status and a genuinely
// unparseable body all reported identically as "the release manifest could not be
// read". This carries the transport error name and HTTP status, or a body-level
// reason, so the maintenance page and a bench tool can say WHAT failed. It is a
// diagnostic only -- no decision branches on it.
const char *OtaWifiLastError();
