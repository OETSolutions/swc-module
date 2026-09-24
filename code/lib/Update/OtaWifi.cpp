#include "Update/OtaWifi.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#endif

namespace {

bool last_verified_ = false;

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
    // A NAME, not a mode. The whole point of returning a string is that a test or
    // a log can record which verification path was used, so "we use the bundle"
    // is a checkable fact rather than a comment someone may have deleted.
    //
    // **It names the default bundle, which is NOT a pinned CA.** See the header
    // and N-62 for what that does and does not buy; do not read this string as
    // "the trust anchor is pinned".
    return "esp_crt_bundle";
}

ReleaseCheckResult OtaWifiCheck(const char *manifest_url, const char *current_version,
                                ReleaseInfo *out) {
    if (manifest_url == nullptr || current_version == nullptr || out == nullptr) {
        return ReleaseCheckResult::kMalformed;
    }
    if (strncmp(manifest_url, "https://", 8) != 0) {
        // Refused before any request: a plain-http manifest is a downgrade
        // attack, and the check belongs at the boundary where the URL arrives.
        return ReleaseCheckResult::kMalformed;
    }

#ifdef ESP_PLATFORM
    manifest_len_ = 0;
    manifest_overflow_ = false;

    esp_http_client_config_t cfg = {};
    cfg.url = manifest_url;
    cfg.event_handler = ManifestHttpEvent;
    // VERIFIED TLS, but see the header: this is IDF's DEFAULT bundle, not a pinned
    // CA. Not `setInsecure` -- see spec 9.5, which records the reference project's
    // disabled validation as a gap SWC must not repeat -- but the anchor is the
    // ~200 stock Mozilla roots, and pinning one is open item N-62.
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return ReleaseCheckResult::kMalformed;
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
        return ReleaseCheckResult::kMalformed;
    }
    if (manifest_overflow_ || manifest_len_ == 0) return ReleaseCheckResult::kMalformed;

    manifest_buf_[manifest_len_] = '\0';
    return ReleaseCheckParse(manifest_buf_, out, current_version);
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
    // Verified against the default bundle -- see the header and N-62. Still not
    // `setInsecure`, and still not a pinned CA.
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
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
