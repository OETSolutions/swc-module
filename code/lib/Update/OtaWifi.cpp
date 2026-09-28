#include "Update/OtaWifi.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#endif

/*
 * The trust anchors for the release fetch are the PINNED CAs (N-62/N-92, spec
 * 9.5's "pinned CA certificate, not `setInsecure()`") -- `kReleaseCaPem`, the two
 * roots GitHub Releases traverses: Sectigo E46 for `github.com` and ISRG Root X1
 * for the `release-assets.` redirect target (see `ReleaseCa.h` for why both are
 * required). A BENCH build substitutes a different pinned root with
 * `-D SWC_BENCH_RELEASE_CA`, which selects a generated `Update/BenchReleaseCa.h`
 * (written by `tools/bench_ota_release.py` from a bring-up host's chain). This
 * keeps the pin REAL on the bench too -- it is another CA, not a bundle and not
 * `setInsecure` -- so the ACCEPT/REJECT behaviour is exercised against an actual
 * pin without a release. A SHIPPED build never sets the define, so it can only
 * ever trust the release roots. Both headers define the same symbol, so exactly
 * one is included.
 */
#ifdef ESP_PLATFORM
#ifdef SWC_BENCH_RELEASE_CA
#include "Update/BenchReleaseCa.h"
#else
#include "Update/ReleaseCa.h"
#endif
#endif  // ESP_PLATFORM

namespace {

bool last_verified_ = false;

// The reason the last `OtaWifiCheck` failed, so a caller (the maintenance page)
// can say WHAT went wrong instead of only "the release manifest could not be
// read". N-85 recorded that the bare `kMalformed` result "names nothing near the
// cause" -- a TLS handshake failure caused by an unset clock, a DNS failure and a
// genuinely malformed body all reported identically. This carries the transport
// error name and HTTP status (or a body-level reason) to the surface.
char last_error_[160] = "";

void SetLastError(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error_, sizeof(last_error_), fmt, ap);
    va_end(ap);
}

#ifdef ESP_PLATFORM
constexpr size_t kManifestBufLen = 4096;
char  manifest_buf_[kManifestBufLen];
size_t manifest_len_ = 0;
bool   manifest_overflow_ = false;

esp_err_t ManifestHttpEvent(esp_http_client_event_t *e) {
    switch (e->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (e->data == nullptr || e->data_len <= 0) break;
            if (manifest_len_ + static_cast<size_t>(e->data_len) >= kManifestBufLen) {
                // A manifest larger than the buffer is refused rather than
                // truncated: a truncated JSON would fail to parse and be reported
                // as "malformed manifest", which points nowhere near the cause.
                manifest_overflow_ = true;
                break;
            }
            memcpy(manifest_buf_ + manifest_len_, e->data, static_cast<size_t>(e->data_len));
            manifest_len_ += static_cast<size_t>(e->data_len);
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Streams one image chunk into the shared OTA gate.
esp_err_t ImageHttpEvent(esp_http_client_event_t *e) {
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (e->data == nullptr || e->data_len <= 0) return ESP_OK;
    if (OtaChunk(static_cast<const uint8_t *>(e->data), static_cast<size_t>(e->data_len)) !=
        OtaResult::kOk) {
        // Abort the transfer rather than continuing to download an image that
        // will be refused: the write already failed, so the remaining bytes are
        // wasted and the connection is holding the radio up.
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif  // ESP_PLATFORM

}  // namespace

const char *OtaWifiCaBundleAttach() {
    // A NAME, not a mode. The purpose is that a test or a log can record which
    // verification path was used, so the choice is a checkable fact rather than a
    // comment someone may have deleted.
    //
    // **It now names a PINNED CA (N-62 resolved, N-92 corrected): the release
    // fetch sets a `cert_pem` trust anchor holding the two roots the release chain
    // traverses (github.com + the redirect target), not IDF's ~200-root default
    // bundle. The old return value `"esp_crt_bundle"` described the weaker anchor
    // this function used to stand beside.**
    return "pinned_ca";
}

ReleaseCheckResult OtaWifiCheck(const char *manifest_url, const char *current_version,
                                ReleaseInfo *out) {
    if (manifest_url == nullptr || current_version == nullptr || out == nullptr) {
        return ReleaseCheckResult::kMalformed;
    }
    if (strncmp(manifest_url, "https://", 8) != 0) {
        // Refused before any request: a plain-http manifest is a downgrade
        // attack, and the check belongs at the boundary where the URL arrives.
        SetLastError("the manifest URL is not https");
        return ReleaseCheckResult::kMalformed;
    }
    last_error_[0] = '\0';

#ifdef ESP_PLATFORM
    manifest_len_ = 0;
    manifest_overflow_ = false;

    esp_http_client_config_t cfg = {};
    cfg.url = manifest_url;
    cfg.event_handler = ManifestHttpEvent;
    // PINNED CAs (N-62/N-92), not the ~200-root default bundle: the device trusts
    // only the two roots the release chain traverses -- github.com (Sectigo E46)
    // and the 302 target release-assets.githubusercontent.com (ISRG Root X1). Both
    // are required because `esp_http_client` reuses this `cert_pem` for the
    // redirect connection. Not `setInsecure` either.
    cfg.cert_pem = kReleaseCaPem;
    cfg.timeout_ms = 10000;
    // A GitHub release URL redirects to a CDN whose signed query string makes the
    // request first line (`GET <path>?<sig> HTTP/1.1`) roughly **900 bytes**. IDF
    // renders that into `buffer_size_tx`, whose default is 512, and
    // `http_client_prepare_first_line` returns `-1` ("Out of buffer") when it does
    // not fit -- which surfaces as `ESP_FAIL` on the redirect hop, i.e. the fetch
    // works over a tunnel (one short hop) and fails against the real release host.
    // The image path already set this; the check path did not (N-92).
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        SetLastError("could not create the HTTP client");
        return ReleaseCheckResult::kMalformed;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        // Name the transport cause: the returned result is only `kMalformed`, and
        // the common cause here is TLS cert validation -- which for this RTC-less
        // device means an unset clock (see the SNTP note in MaintenanceRadio.cpp),
        // not a bad URL.
        ESP_LOGE("swc-ota", "manifest fetch failed: %s (http status %d)",
                 esp_err_to_name(err), status);
        SetLastError("the fetch failed: %s (http status %d)",
                     esp_err_to_name(err), status);
        return ReleaseCheckResult::kMalformed;
    }
    if (status != 200) {
        SetLastError("the release host answered HTTP %d", status);
        return ReleaseCheckResult::kMalformed;
    }
    if (manifest_overflow_) {
        SetLastError("the manifest was larger than %u bytes",
                     static_cast<unsigned>(kManifestBufLen));
        return ReleaseCheckResult::kMalformed;
    }
    if (manifest_len_ == 0) {
        SetLastError("the release host returned an empty body (http status %d)", status);
        return ReleaseCheckResult::kMalformed;
    }

    manifest_buf_[manifest_len_] = '\0';
    const ReleaseCheckResult r = ReleaseCheckParse(manifest_buf_, out, current_version);
    if (r == ReleaseCheckResult::kMalformed) {
        SetLastError("the manifest body did not parse (%u bytes)",
                     static_cast<unsigned>(manifest_len_));
    }
    return r;
#else
    // The host has no HTTP client. Saying "malformed" rather than inventing a
    // result keeps a host test from asserting on a fabricated decision.
    (void)out;
    return ReleaseCheckResult::kMalformed;
#endif
}

OtaResult OtaWifiInstall(const ReleaseInfo &info, size_t max_size,
                         void (*progress)(void *ctx, const OtaWifiProgress &), void *ctx) {
    last_verified_ = false;
    if (strncmp(info.url, "https://", 8) != 0) return OtaResult::kVerifyFailed;
    if (progress != nullptr) {
        OtaWifiProgress p{0, info.size_bytes};
        progress(ctx, p);
    }

    // The WiFi path drives the SAME gate as USB. Opening it here means both paths
    // share the hash validation, the size bound and the commit point.
    const OtaResult began = OtaBegin(info.size_bytes, info.sha256_hex, max_size);
    if (began != OtaResult::kOk) return began;

#ifdef ESP_PLATFORM
    esp_http_client_config_t cfg = {};
    cfg.url = info.url;
    cfg.event_handler = ImageHttpEvent;
    // PINNED CAs (N-62/N-92), not the default bundle: the image URL 302-redirects
    // to the same `release-assets.` host as the manifest, so the same two roots
    // are required here. Still not `setInsecure`.
    cfg.cert_pem = kReleaseCaPem;
    cfg.timeout_ms = 15000;
    // The image is up to ~1.5 MB; the default buffer is fine because the event
    // handler consumes each chunk as it arrives rather than buffering the image.
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        OtaAbort();
        return OtaResult::kFlashFailed;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        OtaAbort();
        return OtaResult::kFlashFailed;
    }

    if (progress != nullptr) {
        OtaWifiProgress p{info.size_bytes, info.size_bytes};
        progress(ctx, p);
    }
    // A completed, verified HTTP session over the CA bundle.
    last_verified_ = true;
#endif

    // OtaEnd verifies and then, and only then, calls the one commit point.
    return OtaEnd();
}

bool OtaWifiLastSessionWasVerified() { return last_verified_; }

const char *OtaWifiLastError() { return last_error_; }
