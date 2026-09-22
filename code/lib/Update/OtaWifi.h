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
 * **Be precise about what the trust anchor is, because an earlier version of this
 * comment was not.** The code attaches IDF's `esp_crt_bundle_attach`, whose bundle
 * is the stock Mozilla root set (~200 CAs), NOT a pinned CA. That authenticates
 * the channel and defeats a passive attacker, but any of those roots -- or a CA
 * that can be coerced into issuing for the release host -- can vouch for the
 * manifest and image. Spec 9.5's wording is stronger than the implementation, and
 * the two are not the same property; pinning a CA via
 * `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE` or a literal `cert_pem` is open item
 * N-62. An earlier revision of this comment also said "the CA is a compiled-in
 * PEM"; there is no PEM in this tree, so that described a design the code does not
 * implement. The device has no filesystem (spec 9.2), which is true and is why a
 * bundle is used at all -- it is just not a *pinned* one.
 *
 * **`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` is what makes this compile and link at
 * all** (`components/mbedtls/CMakeLists.txt` gates both the
 * `esp_crt_bundle/include` include path and `esp_crt_bundle.c` on it). It defaults
 * `y`, so the build works today, but it is named in neither `sdkconfig.defaults`
 * nor `check_sdkconfig_keys.py` -- so a clean build under a changed default would
 * lose the symbol and this file would stop linking with an error naming neither
 * the key nor spec 9.5. Pinning it is part of N-62.
 */

struct OtaWifiProgress {
    size_t bytes_read;
    size_t bytes_total;
};

// The CA bundle identifier IDF's `esp-tls` uses for its built-in roots. Pinned by
// name so a future edit cannot silently swap in `setInsecure`.
//
// **What this pins is the NAME of the mechanism, not the CA.** It returns the
// bundle's identifier, so a test or a log can record that the bundle path -- not
// `setInsecure` -- was taken. It cannot tell you the bundle is a *pinned* one; it
// is not (N-62). A caller that wants to assert the trust anchor is a specific CA
// needs the pinning to exist first.
//
// It has NO CALLER anywhere in the tree (grep-confirmed), so the guarantee this
// comment describes is currently asserted by nothing.
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
// the rule today. It also cannot see the weaker property N-62 records: a session
// over the default bundle is "verified" by this flag while not being pinned, so a
// future caller must not read it as "pinned". Wire it to a self-test when the
// `ota_*` path is wired to `OtaUsb` (N-14).
bool OtaWifiLastSessionWasVerified();
