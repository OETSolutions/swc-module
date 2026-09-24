#include "Maintenance/MaintenanceRadio.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "Maintenance/MaintenanceHttp.h"
#include "Maintenance/WebPage.h"
#include "Update/OtaUsb.h"
#include "Update/OtaWifi.h"
#include "Util/FwVersion.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "swc-maint";

/*
 * The maintenance radio (spec 8.1/8.2/8.3/8.4, FR-32/FR-34). This is the THIRD
 * translation unit the host build excludes, for the same reason as `EspHal.cpp`
 * and `UsbLink.cpp`: it names NimBLE, the WiFi driver and the HTTP server, none
 * of which exist for the native target.
 *
 * **Everything that could be host-tested was pulled out first**, so this file is
 * as thin as the hardware lets it be:
 *   - routing, the token gate, the API bodies  -> `MaintenanceHttp`   (tested)
 *   - the PoP / token derivation, Sec0 gate   -> `BleProvisioning`,
 *                                                `WebPage`            (tested)
 * What remains here is only the bring-up, the socket, and the teardown.
 *
 * **Every bring-up step is paired with a teardown that frees EXACTLY what was
 * created.** FR-32 is a hard requirement -- a leaked stack is a device with a
 * live radio while it drives a car's steering wheel -- and the failure paths
 * below bail out partway, so a flat "free everything" teardown would free
 * resources that never came up. Each resource therefore has its own flag.
 */

namespace {

bool     g_active = false;
uint32_t g_failures = 0;
// FR-38's activity source: bumped once per HTTP request, read by the poll loop to
// see whether anything is using the window (see `MaintenanceRadioRequestCount`).
uint32_t g_requests = 0;

// What this module created. One flag each, because the creation path can stop at
// any point and freeing something that was never created is itself a fault.
bool           g_netif_inited = false;
bool           g_event_loop = false;
bool           g_wifi_inited = false;
bool           g_prov_inited = false;
bool           g_time_started = false;
esp_netif_t   *g_sta_netif = nullptr;
esp_netif_t   *g_ap_netif = nullptr;
httpd_handle_t g_server = nullptr;

MaintenanceInfo g_info;

// The config state word the page reports, owned by the caller (the orchestrator's
// `ConfigStateWord()` returns a string literal, so the pointer is stable). This
// module cannot reach the orchestrator -- it is device-only and the orchestrator
// is not -- so the value is injected and refreshed each poll tick.
const char *g_config_state = "unknown";

/*
 * The generated-body buffer. The page itself is BORROWED from flash (see
 * `HttpOutcome::borrowed`), so this only holds the small JSON bodies -- a couple
 * of hundred bytes.
 *
 * It is file-scope rather than a stack local deliberately: the HTTP handler runs
 * on the server's own task, whose stack is small, and this project has already
 * put an 8 KB `Config` on a 3.5 KB task stack once. One shared buffer is safe
 * because the server handles one request at a time by default
 * (`CONFIG_HTTPD_MAX_REQ_HDR_LEN` aside, `max_open_sockets` is 2 and the handler
 * does not yield between filling and sending).
 */
char g_body[512];

}  // namespace

// --- teardown helpers ------------------------------------------------------

namespace {

/*
 * Start SNTP so the wall clock becomes VALID once the station joins a network.
 *
 * **Why this is required for the OTA fetch, not a nicety.** `OtaWifiCheck` and
 * `OtaWifiInstall` use VERIFIED TLS (spec 9.5, and `OtaWifi.h`'s "never
 * `setInsecure`"). mbedTLS validates the server certificate's validity window,
 * and this device has NO RTC: a fresh boot's clock is the epoch (1970), so the
 * modern server cert reads as "not yet valid" and the handshake fails -- with
 * `OtaWifiCheck` mapping that to `kMalformed` ("the release manifest could not be
 * read"), which points nowhere near the clock. SNTP fixes the clock the moment
 * an IP is obtained, which is the first instant the fetch is even possible.
 *
 * **It is started on `IP_EVENT_STA_GOT_IP`, not at bring-up**, because before the
 * station has an IP there is nothing to sync against; the DHCP-assigned DNS
 * server comes with that event, so the hostnames resolve. This costs one event
 * handler for the window's lifetime and is stopped in `TearDown`.
 */
void StartSntp()
{
    if (g_time_started) return;
    // Pool addresses cover the common "first NTP blocked, second works" case.
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    g_time_started = true;
    ESP_LOGI(TAG, "SNTP started; the clock is set once the station resolves a server");
}

// Handler for `IP_EVENT_STA_GOT_IP` (the station obtained an address).
void OnAnyEvent(void *, esp_event_base_t base, int32_t id, void *)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        StartSntp();
    }
}

// Undo whatever the bring-up managed to create, in reverse order. Safe to call
// with every flag clear.
void TearDown()
{
    if (g_time_started) {
        // Stop SNTP BEFORE the network goes away, so no timer fires against a
        // torn-down netif. `sntp_stop` is safe to call once `sntp_init` returned.
        esp_sntp_stop();
        g_time_started = false;
    }

    if (g_server != nullptr) {
        // Stops the server task and closes the sockets. Before `wifi_stop`,
        // because a socket outliving its netif is how the next start fails with a
        // stale bind.
        httpd_stop(g_server);
        g_server = nullptr;
    }

    if (g_prov_inited) {
        // `stop` then `deinit`, in that order: `deinit` releases the manager's
        // resources and the scheme's event handler frees the BLE controller's
        // memory on the DEINIT event. Calling `deinit` alone would rely on the
        // manager stopping provisioning internally, which is a documented
        // behaviour rather than the intended sequence.
        wifi_prov_mgr_stop_provisioning();
        wifi_prov_mgr_deinit();
        g_prov_inited = false;
    }

    if (g_wifi_inited) {
        esp_wifi_stop();
        esp_wifi_deinit();
        g_wifi_inited = false;
    }

    if (g_sta_netif != nullptr) {
        esp_netif_destroy_default_wifi(g_sta_netif);
        g_sta_netif = nullptr;
    }
    if (g_ap_netif != nullptr) {
        esp_netif_destroy_default_wifi(g_ap_netif);
        g_ap_netif = nullptr;
    }

    if (g_event_loop) {
        // Unregister this module's handler before the loop it lives on is gone.
        // `esp_event_handler_unregister` is safe even if the register failed (it
        // just returns ESP_ERR_NOT_FOUND), so no extra flag is needed.
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnAnyEvent);
        // Deleted only if THIS module created it, which is why the flag exists:
        // the device may one day have a default loop for another purpose, and
        // deleting someone else's loop is a use-after-free waiting to happen.
        esp_event_loop_delete_default();
        g_event_loop = false;
    }
    if (g_netif_inited) {
        // There is no `esp_netif_deinit()`. The netif layer is initialised once
        // per boot in IDF and has no teardown API, so this flag records that the
        // call was made rather than that anything must be undone -- the
        // destruction that matters is per-netif, above.
        g_netif_inited = false;
    }

    memset(&g_info, 0, sizeof(g_info));
    g_active = false;
    // `g_failures` is deliberately NOT cleared here -- `MaintenanceRadioStop`
    // owns it, because a start that FAILED also runs this teardown and must leave
    // its failure reported. See the header.
}

}  // namespace

// --- WiFi and the page's AP ------------------------------------------------

namespace {

void ConfigureAp()
{
    if (g_ap_netif == nullptr) return;

    // The short device id keeps two units on a bench distinguishable, the same
    // reason the BLE advertised name carries it.
    char short_id[16] = "0000";
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        DeviceIdShort(mac, short_id, sizeof(short_id));
    }

    wifi_config_t ap{};
    snprintf(reinterpret_cast<char *>(ap.ap.ssid), sizeof(ap.ap.ssid), "SWC-%s", short_id);
    ap.ap.ssid_len = static_cast<uint8_t>(strlen(reinterpret_cast<const char *>(ap.ap.ssid)));
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    /*
     * The AP is OPEN, and that is a deliberate choice rather than an oversight.
     * It exists only for the maintenance window and it carries nothing secret:
     * the PAGE is token-gated (spec 8.4), so an attacker who joins the AP still
     * cannot read or change anything. Requiring a second password to reach a page
     * whose entire purpose is "help me get onto WiFi" is friction that makes a
     * user give up at exactly the step that is supposed to help them, and a
     * per-device AP password would be a THIRD secret to display over USB.
     */
    ap.ap.authmode = WIFI_AUTH_OPEN;

    const esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP config failed: %s", esp_err_to_name(err));
    }
}

/*
 * Put the driver into AP+STA and bring up the page's AP.
 *
 * **Called AFTER the provisioning manager starts, not before.** `wifi_prov_mgr_
 * start_provisioning` sets station mode itself -- it needs it for the scan it
 * answers the Espressif app with -- and would clobber an APSTA set earlier. Once
 * credentials arrive the manager switches to its scheme's mode (STA for the BLE
 * scheme), which turns the AP off; that is the intended transition, because the
 * page then moves to the joined network and the token is what protects it there.
 */
void EnterApSta()
{
    const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "APSTA mode failed: %s; the page is reachable only after joining",
                 esp_err_to_name(err));
        return;
    }
    ConfigureAp();
}

}  // namespace

// --- the page's facts ------------------------------------------------------

namespace {

void FillFacts(MaintenanceFacts *f)
{
    *f = MaintenanceFacts{};
    f->fw_version = FwVersionString();
    f->device_id = g_info.ble_name;
    // Microseconds since boot; the field is named for what it is.
    f->uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    f->config_state = g_config_state;
    f->heap_free = static_cast<uint32_t>(esp_get_free_heap_size());

    // The station's state, not the AP's: "connected" answers the question the
    // page's WiFi section is about, which is whether the device joined a network.
    wifi_ap_record_t ap_info{};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        f->wifi_connected = true;
        f->wifi_ssid = reinterpret_cast<const char *>(ap_info.ssid);
    } else {
        f->wifi_connected = false;
        f->wifi_ssid = "";
    }

    // The NTC is not read by this module: the orchestrator already samples it and
    // owns the "last good reading" hold (N-67). Leaving `have_temp` false is the
    // honest report -- the page says "no reading" rather than a fabricated 0.0 --
    // and it is recorded as a gap rather than papered over.
    f->have_temp = false;
    f->temp_tenths_c = 0;
}

esp_err_t SendOutcome(httpd_req_t *req, const HttpOutcome &o)
{
    if (o.content_type != nullptr) httpd_resp_set_type(req, o.content_type);
    if (o.status != 200) {
        // A refused request needs its own status line. Without it a 401 reaches
        // the page as a 2xx carrying an error body, and the script's `r.ok` check
        // would treat that as success -- the exact "reported as success"
        // confusion the token gate exists to prevent.
        char status[8];
        snprintf(status, sizeof(status), "%d", o.status);
        httpd_resp_set_status(req, status);
    }
    const char *body = (o.borrowed != nullptr) ? o.borrowed : g_body;
    return httpd_resp_send(req, body, o.body_len);
}

// --- the OTA upload --------------------------------------------------------

/*
 * `POST /api/ota/upload`: the raw body IS the image, and its size and SHA-256
 * arrive in the two metadata headers (`X-SWC-Size`, `X-SWC-Sha256`).
 *
 * **Why a raw body rather than a multipart form.** `OtaBegin` -- the shared gate
 * BOTH OTA paths go through (spec 9.4: "the checksum, slot-writing, rollback and
 * health-confirmation logic exists exactly once") -- needs the declared size and
 * digest BEFORE a byte is written. The page can compute the digest in JavaScript
 * (a small SHA-256, because `crypto.subtle` is undefined outside a secure context
 * and this page is plain HTTP on a LAN address), but a `FormData` body cannot
 * carry a value the firmware reads without a multipart parser. Putting the two
 * values in headers removes that parser entirely, and the digest the page
 * computes is exactly what the device verifies the bytes it received against --
 * which is what makes the upload a verified install rather than a hopeful write.
 *
 * The stream is fed to the SAME `OtaChunk` the USB path uses, so what is written
 * to the inactive slot is what was verified, in one pass (spec 9.4's "transports
 * are thin adapters").
 */
esp_err_t HandleUpload(httpd_req_t *req)
{
    char sha[80];
    char size_str[24];
    if (httpd_req_get_hdr_value_str(req, kMaintenanceSha256Header, sha, sizeof(sha)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing X-SWC-Sha256");
    }
    if (httpd_req_get_hdr_value_str(req, kMaintenanceSizeHeader, size_str, sizeof(size_str)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing X-SWC-Size");
    }
    const unsigned long declared = strtoul(size_str, nullptr, 10);
    if (declared == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad X-SWC-Size");
    }

    // The gate, before anything is written: a bad digest, an empty image or one
    // larger than the slot is refused here rather than after megabytes of flash.
    const OtaResult began = OtaBegin(static_cast<size_t>(declared), sha, kAppSlotBytes);
    if (began != OtaResult::kOk) {
        ESP_LOGW(TAG, "OTA upload refused at begin");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image refused by the verifier");
    }

    // Stream the body. A short read or a write failure ABORTS the run, so a
    // partial image is never left as a candidate boot image.
    //
    // `size_t`, not `int`: `httpd_req_t::content_len` is a `size_t`, and narrowing
    // a peer-controlled length to `int` first is the cast-before-bounds family
    // (N-50) -- a body over 2 GB would go negative and the loop would silently
    // stream NOTHING, so the run would fail closed but report "install failed"
    // rather than "truncated". `OtaBegin` above bounds the DECLARED size to the
    // slot; this bounds the stream itself.
    size_t remaining = req->content_len;
    char buf[1024];
    while (remaining > 0) {
        const int want = (remaining < sizeof(buf)) ? static_cast<int>(remaining)
                                                   : static_cast<int>(sizeof(buf));
        const int got = httpd_req_recv(req, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;   // the socket stalled; retry
        if (got <= 0) {
            OtaAbort();
            ESP_LOGE(TAG, "OTA upload: the body ended early (%u bytes unread)",
                     static_cast<unsigned>(remaining));
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload truncated");
        }
        if (OtaChunk(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(got)) != OtaResult::kOk) {
            OtaAbort();
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image failed the digest check");
        }
        remaining -= static_cast<size_t>(got);
    }

    // Verify, then switch the boot partition. Nothing is installed unless the
    // verify passed, and the switch happens only inside `OtaEnd`.
    const OtaResult done = OtaEnd();
    if (done != OtaResult::kOk) {
        ESP_LOGW(TAG, "OTA upload did not commit");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "install failed");
    }

    ESP_LOGI(TAG, "OTA upload verified and committed; reboot to run it");
    return httpd_resp_send(req, "{\"message\":\"installed\"}", HTTPD_RESP_USE_STRLEN);
}

// --- the WiFi join ---------------------------------------------------------

/*
 * `POST /api/wifi`: apply the SSID and passphrase the page collected, then
 * connect. The body is the JSON the page sends, read from the request here.
 *
 * A successful join turns the AP off (the driver leaves AP+STA for STA when the
 * BLE scheme's credentials arrive, and this follows the same path), so the reply
 * is sent BEFORE the connect: the page says "the device will join and then restart
 * the radio", and a reply written after the AP went down would never arrive.
 */
esp_err_t HandleJoinWifi(httpd_req_t *req)
{
    // The body is small (two short strings) and the handler's stack is limited, so
    // it is read into a fixed buffer with an explicit bound rather than trusted.
    // Clamped in `size_t` before the cast, for the same N-50 reason as the upload:
    // narrowing `content_len` to `int` first would make a body over 2 GB read as
    // negative and be reported as "empty" rather than "too long".
    char body[256];
    const size_t want = (req->content_len < sizeof(body) - 1) ? req->content_len
                                                             : sizeof(body) - 1;
    if (want == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    int got = 0;
    while (static_cast<size_t>(got) < want) {
        const int n = httpd_req_recv(req, body + got, static_cast<int>(want) - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short read");
        got += n;
    }
    body[got] = '\0';

    /*
     * Parsed with cJSON, the same parser the rest of the firmware uses for peer
     * JSON -- NOT a hand-rolled scan for `"ssid":"…"`.
     *
     * A substring scan is wrong for a value that can legally contain the parser's
     * own delimiters: the page's `JSON.stringify` escapes a quote or backslash in
     * an SSID as `\"` and `\\`, so a scan would stop at the first escaped quote
     * and hand the driver a TRUNCATED network name or passphrase -- which
     * surfaces as "wrong password" and points nowhere near the cause. cJSON is
     * already linked for `ConfigCodec`, so using it also avoids a second,
     * weaker JSON reader.
     */
    cJSON *root = cJSON_Parse(body);
    if (root == nullptr) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "malformed JSON");
    }
    // Frees `root` on EVERY exit below, including the refusal paths -- a leak on
    // the request path is a leak per request, and a user clicking Join twice is
    // enough to notice.
    struct Guard {
        cJSON *j;
        ~Guard() { cJSON_Delete(j); }
    } guard{root};

    const cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass_j = cJSON_GetObjectItemCaseSensitive(root, "passphrase");
    if (!cJSON_IsString(ssid_j) || ssid_j->valuestring == nullptr) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    }
    // The passphrase is REQUIRED to be a string but may be EMPTY: an open network
    // is a legal thing to join, and `cJSON_IsString` on `""` is still true.
    if (!cJSON_IsString(pass_j) || pass_j->valuestring == nullptr) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing passphrase");
    }

    // Sized to the driver's OWN fields (`wifi_sta_config_t`), so the copy into the
    // config below cannot truncate: `ssid` is 32 bytes + NUL and `password` is 64
    // + NUL, and a buffer smaller than the destination is how a passphrase is
    // silently cut short and the join then fails as "wrong password".
    wifi_config_t cfg = {};
    snprintf(reinterpret_cast<char *>(cfg.sta.ssid), sizeof(cfg.sta.ssid), "%s",
             ssid_j->valuestring);
    snprintf(reinterpret_cast<char *>(cfg.sta.password), sizeof(cfg.sta.password), "%s",
             pass_j->valuestring);
    if (cfg.sta.ssid[0] == '\0') {
        // An SSID the driver would silently reject, reported as the bad request it
        // is rather than as a join that never happens.
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty ssid");
    }
    // WPA2 is the floor this device accepts; an open network is a deliberate
    // choice the firmware does not make for the user (spec 8.4's "no default
    // password, no admin/admin" is about the PAGE, but a device silently joining
    // an open AP is the same class of default).
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // Reply FIRST: the connect below can drop the AP, and a reply sent after that
    // would never reach the page.
    const esp_err_t sent = httpd_resp_send(req, "{\"message\":\"joining\"}", HTTPD_RESP_USE_STRLEN);

    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi config rejected");
        return sent;
    }
    // STA-only, which is what turns the page's AP off: the page moves to the
    // joined network, where the token still guards it (spec 8.4).
    esp_wifi_set_mode(WIFI_MODE_STA);
    const esp_err_t conn = esp_wifi_connect();
    if (conn != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed to start: %s", esp_err_to_name(conn));
    } else {
        ESP_LOGI(TAG, "joining the network the page supplied");
    }
    return sent;
}

// --- the two network update actions ----------------------------------------

/*
 * Where the release manifest lives (spec 9.5's git-release scheme). The same URL
 * the app uses -- GitHub Releases' `latest/download` alias resolves to the newest
 * release's asset, so it is stable across versions. It is a constant rather than
 * a setting because there is exactly one release channel today; the manifest
 * carries a `channel` for when a beta path is added.
 */
constexpr const char *kReleaseManifestUrl =
    "https://github.com/oetsolutions/swc-module/releases/latest/download/version_manifest.json";

/*
 * The manifest URL used by `HandleOtaCheck`/`HandleOtaPull`. Normally the release
 * constant above; a BENCH build can override it with `-D
 * SWC_BENCH_MANIFEST_URL=\"https://...\"` to exercise the fetch against a
 * bring-up server (a Cloudflare quick tunnel serving a manifest + image) without
 * publishing a release. The override is behind its own define, so a SHIPPED build
 * cannot be repointed at a server that is not the release channel -- the same
 * guard the other `SWC_BENCH_*` switches use. The value must be a `https://` URL
 * (a bench cannot exercise the verified-TLS path over plain http, and `OtaWifiCheck`
 * refuses one anyway).
 */
const char *ManifestUrl() {
#ifdef SWC_BENCH_MANIFEST_URL
    // Referenced so the compiled-in release URL stays a live symbol even when the
    // bench override is in use (a namespace-scope constant referenced by nothing
    // trips `-Wunused-const-variable` under this project's `-Werror`).
    (void)kReleaseManifestUrl;
    return SWC_BENCH_MANIFEST_URL;
#else
    return kReleaseManifestUrl;
#endif
}

/*
 * `POST /api/ota/check`: fetch the manifest and decide (spec 9.5).
 *
 * The manifest is fetched over VERIFIED TLS (`OtaWifiCheck` refuses a plain-http
 * URL outright, so a downgrade cannot be requested by a header this device
 * trusts). The running version is the same string `hello` reports, so the page
 * and the app agree about what is installed.
 */
esp_err_t HandleOtaCheck(httpd_req_t *req)
{
    ReleaseInfo info{};
    const ReleaseCheckResult r = OtaWifiCheck(ManifestUrl(), FwVersionString(), &info);

    const char *word = nullptr;
    switch (r) {
        case ReleaseCheckResult::kUpToDate:          word = "up to date"; break;
        case ReleaseCheckResult::kNewer:             word = "an update is available"; break;
        case ReleaseCheckResult::kNotNewer:          word = "the device is newer than the release"; break;
        case ReleaseCheckResult::kTooOldToUpgradeFrom:
            word = "this version is too old to update directly";
            break;
        case ReleaseCheckResult::kWrongBoard:        word = "the release is for different hardware"; break;
        case ReleaseCheckResult::kMalformed:         word = "the release manifest could not be read"; break;
    }

    char out[256];
    if (r == ReleaseCheckResult::kNewer) {
        // The version and the size are what the page needs to say what it found;
        // the URL is NOT included, because the install path takes it from the
        // manifest again rather than from the browser (a URL from a page would be
        // an attacker-supplied download if the token leaked).
        snprintf(out, sizeof(out), "{\"message\":\"an update is available: %s (%u bytes)\"}",
                 info.latest_version, static_cast<unsigned>(info.size_bytes));
    } else {
        snprintf(out, sizeof(out), "{\"message\":\"%s\"}", word);
    }
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

/*
 * `POST /api/ota/pull`: fetch the manifest and install what it names (spec 9.5).
 *
 * **It re-checks the manifest rather than trusting the page's previous check.**
 * The two requests are separate, and a manifest that changed in between would
 * otherwise be installed on the strength of a decision made about a different
 * release. The re-check is also what keeps the size and digest in the gate's
 * hands rather than the browser's.
 *
 * The image goes through the SAME gate as USB and upload (`OtaWifiInstall` calls
 * `OtaBegin`/`OtaChunk`/`OtaEnd`), so no path is weaker than another (spec 9.4).
 */
esp_err_t HandleOtaPull(httpd_req_t *req)
{
    ReleaseInfo info{};
    const ReleaseCheckResult r = OtaWifiCheck(ManifestUrl(), FwVersionString(), &info);
    if (r != ReleaseCheckResult::kNewer) {
        const char *why = (r == ReleaseCheckResult::kUpToDate)
                              ? "the device is already up to date"
                              : "the release could not be used";
        char out[128];
        snprintf(out, sizeof(out), "{\"message\":\"%s\"}", why);
        return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    }

    // Reply first: the install takes time and the page's own comment says the
    // connection can drop, which is not a failure worth alarming the user about.
    const esp_err_t sent = httpd_resp_send(req, "{\"message\":\"installing\"}",
                                           HTTPD_RESP_USE_STRLEN);

    const OtaResult installed = OtaWifiInstall(info, kAppSlotBytes, nullptr, nullptr);
    if (installed != OtaResult::kOk) {
        // Reported on the console: the page has already been answered, and the
        // next `/api/status` or a reboot is where a user would look.
        ESP_LOGE(TAG, "WiFi OTA install did not commit");
    } else {
        ESP_LOGI(TAG, "WiFi OTA installed %s; reboot to run it", info.latest_version);
    }
    return sent;
}

// --- the request handler ---------------------------------------------------

esp_err_t HandleRequest(httpd_req_t *req)
{
    // FR-38: a request IS activity, so the window's clock is bumped for every one
    // -- including a `/api/status` poll, because the page refreshing its own
    // status is a user sitting in front of it. Counted here (the one entry point)
    // rather than per endpoint, so an endpoint added later is covered by default.
    ++g_requests;

    HttpRequest r{};
    r.method = (req->method == HTTP_GET)    ? "GET"
               : (req->method == HTTP_POST) ? "POST"
                                            : "OTHER";

    /*
     * `req->uri` INCLUDES the query string, so the path is split off here. The
     * page's first load carries the token in the query (its script cannot send a
     * header before it has loaded), and every later call carries it in the
     * header.
     *
     * Sized to the server's own URI cap (`CONFIG_HTTPD_MAX_URI_LEN` = 512) plus
     * the terminator, so this buffer cannot be the thing that truncates a URI the
     * server already accepted -- the over-length case is refused explicitly below
     * instead, which is diagnosable rather than a silently different path.
     */
    static char path[513];
    const char *q = strchr(req->uri, '?');
    if (q != nullptr) {
        const size_t n = static_cast<size_t>(q - req->uri);
        if (n >= sizeof(path)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "uri too long");
        }
        memcpy(path, req->uri, n);
        path[n] = '\0';
        r.query = q + 1;
    } else {
        snprintf(path, sizeof(path), "%s", req->uri);
        r.query = nullptr;
    }
    r.path = path;

    static char token[64];
    if (httpd_req_get_hdr_value_str(req, kMaintenanceTokenHeader, token, sizeof(token)) != ESP_OK) {
        token[0] = '\0';
        if (r.query != nullptr) {
            char qtok[64];
            if (httpd_query_key_value(r.query, "token", qtok, sizeof(qtok)) == ESP_OK) {
                snprintf(token, sizeof(token), "%s", qtok);
            }
        }
    }
    r.token_header = token;

    static char ctype[160];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK) {
        ctype[0] = '\0';
    }
    r.content_type = ctype;

    // The upload's two metadata headers (see `HttpRequest`), read here so the
    // router can decide whether the action is even well-formed.
    static char sha[80];
    if (httpd_req_get_hdr_value_str(req, kMaintenanceSha256Header, sha, sizeof(sha)) != ESP_OK) {
        sha[0] = '\0';
    }
    r.sha256_header = sha;
    static char size_hdr[24];
    if (httpd_req_get_hdr_value_str(req, kMaintenanceSizeHeader, size_hdr, sizeof(size_hdr)) != ESP_OK) {
        size_hdr[0] = '\0';
    }
    r.size_header = size_hdr;

    MaintenanceFacts facts;
    FillFacts(&facts);

    const HttpOutcome o = MaintenanceHttpRoute(r, facts, g_info, g_body, sizeof(g_body));

    // The upload is the one request whose BODY is the action, so it is answered
    // before the reply is sent. `MaintenanceHttp` has already checked the token
    // AND the two metadata headers, so a non-200 here is a refusal to report.
    if (o.action == MaintenanceAction::kOtaUpload) {
        if (o.status != 200) return SendOutcome(req, o);
        return HandleUpload(req);
    }
    // The join reads its own body, so it is answered here too rather than after a
    // generic send that would have consumed nothing.
    if (o.action == MaintenanceAction::kJoinWifi) {
        if (o.status != 200) return SendOutcome(req, o);
        return HandleJoinWifi(req);
    }
    // The two network-update actions answer the page themselves (both reply before
    // doing their slow work), so they are dispatched the same way.
    if (o.action == MaintenanceAction::kOtaCheck) {
        if (o.status != 200) return SendOutcome(req, o);
        return HandleOtaCheck(req);
    }
    if (o.action == MaintenanceAction::kOtaPull) {
        if (o.status != 200) return SendOutcome(req, o);
        return HandleOtaPull(req);
    }

    const esp_err_t err = SendOutcome(req, o);
    if (err != ESP_OK) return err;

    // The reboot runs AFTER the reply is out: `esp_restart` never returns, so
    // restarting first would drop the response the page is waiting for. The
    // delay lets the TCP stack flush it.
    if (o.action == MaintenanceAction::kReboot) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    return ESP_OK;
}

}  // namespace

// --- the HTTP server -------------------------------------------------------

namespace {

bool StartHttpServer()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /*
     * ONE wildcard handler rather than six URI registrations.
     *
     * The routing decision belongs in `MaintenanceHttp`, which is host-tested --
     * six handlers here would put the token gate and the 404-vs-index rule in a
     * file no host build compiles, checkable only by reading it. The wildcard
     * also means the token is checked in ONE place instead of six.
     *
     * `uri_match_fn` must be set explicitly: `HTTPD_DEFAULT_CONFIG` leaves it
     * NULL, and a NULL matcher with a wildcard template would compare the literal
     * string "/" + "*" against every URI and match nothing.
     */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 4;
    // Two sockets is enough for a phone's browser and its own parallel asset
    // fetches; more costs RAM this device does not have.
    cfg.max_open_sockets = 4;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;

    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        g_server = nullptr;
        return false;
    }

    static const httpd_uri_t wildcard = {
        .uri = "/*",
        // `HTTP_ANY` is `http_method` 0, and the designated initializer needs the
        // enum's own type rather than the int literal.
        .method = static_cast<httpd_method_t>(HTTP_ANY),
        .handler = HandleRequest,
        .user_ctx = nullptr,
    };
    if (httpd_register_uri_handler(g_server, &wildcard) != ESP_OK) {
        ESP_LOGE(TAG, "registering the wildcard handler failed");
        httpd_stop(g_server);
        g_server = nullptr;
        return false;
    }
    return true;
}

}  // namespace

// --- the public entry points -----------------------------------------------

namespace {

/*
 * Bring up the two stack layers. Split out of `MaintenanceRadioStart` so the
 * bring-up reads as a sequence of steps with one shared failure exit, rather than
 * a single function whose every step is a nested `if` -- and so no `goto` has to
 * jump over a live initialization, which C++ forbids.
 *
 * Each step sets its own flag BEFORE the next begins, which is what lets
 * `TearDown` free exactly what exists.
 */
bool BringUpStacks()
{
    if (!g_netif_inited) {
        if (esp_netif_init() != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_init failed");
            return false;
        }
        g_netif_inited = true;
    }
    if (!g_event_loop) {
        // Created by THIS module, so teardown may delete it -- the flag is what
        // makes that safe. ESP_ERR_INVALID_STATE means a default loop already
        // exists, which is not a failure; this module simply must not delete it.
        const esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(e));
            return false;
        }
        g_event_loop = true;
    }

    // Start the clock once the station gets an address (see `StartSntp`). Without
    // it the verified-TLS OTA fetch fails against a modern server cert because the
    // RTC-less clock reads 1970. Unregistered in `TearDown` before the loop goes.
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnAnyEvent,
                                   nullptr) != ESP_OK) {
        ESP_LOGW(TAG, "could not register the time-sync handler; the OTA over WiFi "
                      "may fail TLS certificate validation");
    }

    g_sta_netif = esp_netif_create_default_wifi_sta();
    g_ap_netif = esp_netif_create_default_wifi_ap();
    if (g_sta_netif == nullptr || g_ap_netif == nullptr) {
        ESP_LOGE(TAG, "netif creation failed");
        return false;
    }

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wcfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed");
        return false;
    }
    g_wifi_inited = true;
    return true;
}

// The BLE provisioning session, Sec1 with the MAC-derived PoP. Split out for the
// same reason as `BringUpStacks`.
bool StartProvisioning(const MaintenanceInfo &info)
{
    wifi_prov_mgr_config_t pcfg = {};
    pcfg.scheme = wifi_prov_scheme_ble;
    /*
     * `FREE_BT` -- release CLASSIC BT at init, and do NOT release the BLE/BTDM
     * pool at deinit.
     *
     * **`FREE_BTDM` was the obvious choice and it makes the window ONE-SHOT per
     * boot.** That handler calls `esp_bt_mem_release(ESP_BT_MODE_BTDM)` on
     * WIFI_PROV_DEINIT, and IDF's own contract for that call is that it "cannot
     * be reversed. This means you cannot use the Bluetooth Controller mode that
     * you have released" (`esp_bt.h`). So the SECOND `wifi_prov_mgr_init` in a
     * boot re-initialises the controller on memory that was handed back to the
     * heap and the device PANICS AND REBOOTS. Measured on the bench 2026-09-24:
     * enter -> exit -> enter reboots the DUT every time (the app link drops and
     * `uptime_ms` restarts), while a user has no reason to expect a second trip
     * to maintenance to restart their adapter.
     *
     * The trade is real and deliberately taken: `FREE_BTDM` returns a few KB
     * more heap at the end of a window, and `FREE_BT` keeps the BLE controller
     * pool reserved for the boot. **FR-32 is not weakened by this** -- what it
     * forbids is a radio INITIALIZED in normal operation, and after `deinit` the
     * controller is de-initialized and idle either way; a reserved-but-idle pool
     * is not a radio. A reboot on the second entry, by contrast, is a hard
     * failure of the feature. Classic BT is still released at init, because this
     * device never uses it and that is the memory that actually matters here
     * (spec 9.2's 4 MB/no-PSRAM budget).
     */
    pcfg.scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT;

    if (wifi_prov_mgr_init(pcfg) != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init failed");
        return false;
    }
    g_prov_inited = true;

    // The advertised name carries the short device id (spec 8.3), so several
    // units in a scan list are distinguishable. Sized for the "SWC-" prefix plus
    // a full-length id, so the snprintf cannot truncate.
    char service_name[kMaintenanceNameLen + 8];
    snprintf(service_name, sizeof(service_name), "SWC-%s", info.ble_name);

    // A copy, because `start_provisioning` borrows the PoP for the session's
    // lifetime -- `info` is copied into `g_info` later and must not be the
    // lifetime the manager depends on. Sec1 (spec 8.3): Sec0 is rejected as a
    // default and is not offered here at all.
    char pop_copy[kPopLen];
    snprintf(pop_copy, sizeof(pop_copy), "%s", info.pop);

    if (wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, pop_copy,
                                         service_name, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "starting provisioning failed");
        return false;
    }
    return true;
}

}  // namespace

bool MaintenanceRadioStart(MaintenanceInfo *out)
{
    if (g_active) {
        // Idempotent: a second call while the radio is up reports the same
        // window rather than tearing it down and rebuilding it under a user who
        // may be mid-provision.
        if (out != nullptr) *out = g_info;
        return true;
    }

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        // Without the MAC neither secret can be derived, and a session with an
        // empty PoP would fail later as a misleading "wrong password" -- exactly
        // what `PopDerive`'s refusal exists to avoid. Refuse the whole start.
        //
        // The failure count is ASSIGNED here and below rather than incremented:
        // it reports whether THIS window's radio came up, so `TearDown` clears it
        // and every failure path sets it. A cumulative count would keep an old
        // failure alive into a window that worked (see `TearDown`).
        ESP_LOGE(TAG, "cannot read the factory MAC; refusing to open a provisioning window");
        g_failures = 1;
        return false;
    }

    // The secrets, from the host-tested derivations rather than from this file.
    MaintenanceInfo info;
    info.active = true;
    if (!PopDerive(mac, info.pop, sizeof(info.pop)) ||
        !WebTokenDerive(mac, info.token, sizeof(info.token))) {
        ESP_LOGE(TAG, "secret derivation failed; refusing to open a window");
        g_failures = 1;
        return false;
    }
    DeviceIdShort(mac, info.ble_name, sizeof(info.ble_name));

    if (!BringUpStacks() || !StartProvisioning(info)) {
        // A half-started radio is exactly the state FR-32 forbids, so tear down
        // whatever did come up rather than leaving a partial stack resident.
        TearDown();
        g_failures = 1;
        ESP_LOGE(TAG, "maintenance radio bring-up failed; torn down");
        return false;
    }

    // AP+STA AFTER provisioning is up, so the manager's own mode change cannot
    // clobber it -- see `EnterApSta`.
    EnterApSta();

    if (!StartHttpServer()) {
        TearDown();
        g_failures = 1;
        ESP_LOGE(TAG, "maintenance web server failed; radio torn down");
        return false;
    }

    // The URL a user taps, with the token in the query so it does not have to be
    // retyped. IDF's default AP netif is always at 192.168.4.1.
    snprintf(info.page_url, sizeof(info.page_url), "http://192.168.4.1/?token=%s", info.token);

    g_info = info;
    g_active = true;
    if (out != nullptr) *out = g_info;
    ESP_LOGI(TAG, "maintenance radio up: ble=\"SWC-%s\" page=%s", info.ble_name, info.page_url);
    return true;
}

void MaintenanceRadioStop(void)
{
    // The failure count is cleared for EVERY window that closes, including one
    // whose start failed and therefore left nothing to tear down -- hence before
    // the early return, not inside `TearDown`.
    //
    // It is the answer to "did THIS window's radio come up", which is the only
    // question the app asks of it, and the app asks it FIRST on the card. Left
    // standing, one failed bring-up would make every LATER window render "its
    // radio did not come up, so there is no setup page" while `page_url` in the
    // same frame says the page is right there.
    g_failures = 0;
    if (!g_active && !g_prov_inited && g_server == nullptr) return;
    TearDown();
    ESP_LOGI(TAG, "maintenance radio down; radio stacks freed");
}

bool MaintenanceRadioActive(void) { return g_active; }

uint32_t MaintenanceRadioRequestCount(void) { return g_requests; }

void MaintenanceRadioDescribe(MaintenanceInfo *out)
{
    if (out == nullptr || !g_active) return;
    // Only the display fields: `active` is the orchestrator's window state, which
    // the caller already knows, and overwriting it here would make this module the
    // second home for "is the window open".
    snprintf(out->pop, sizeof(out->pop), "%s", g_info.pop);
    snprintf(out->token, sizeof(out->token), "%s", g_info.token);
    snprintf(out->page_url, sizeof(out->page_url), "%s", g_info.page_url);
    snprintf(out->ble_name, sizeof(out->ble_name), "%s", g_info.ble_name);
}

uint32_t MaintenanceRadioFailures(void) { return g_failures; }

void MaintenanceRadioSetConfigState(const char *word)
{
    g_config_state = (word != nullptr) ? word : "unknown";
}
