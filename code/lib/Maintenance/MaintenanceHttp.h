#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Maintenance/MaintenanceInfo.h"

/*
 * The maintenance page's REQUEST ROUTER (spec 8.4) -- the decision half of the
 * HTTP server, split out from the socket so it is host-testable.
 *
 * **Why this split exists.** The obvious shape is one `esp_http_server` handler
 * per URI, all of them in `MaintenanceRadio.cpp`, which no host build compiles.
 * That would put the token gate -- a SECURITY property, spec 8.4's "no default
 * password, no admin/admin" -- in a file with no tests, checkable only by reading
 * it. `WebPage.h` states the same rule for the token's derivation ("deliberately
 * separate from the HTTP server so that the derivation and the comparison are
 * testable without a socket"); this is that rule applied to the routing.
 *
 * So this file is pure: given a request and the device's facts, it decides the
 * status, the content type, the body and (for the mutating endpoints) WHICH
 * action the caller must perform. `MaintenanceRadio.cpp` is the thin adapter that
 * reads a real request into an `HttpRequest`, performs the `MaintenanceAction`,
 * and writes the reply. Every branch below -- the token gate, 404 vs index,
 * method policing, the API bodies -- runs in `test/test_maintenance`, with no
 * radio and no socket.
 *
 * **The action is RETURNED rather than performed**, because the actions are the
 * device-only ones (join WiFi, install an image, reboot). Keeping them as an enum
 * means the router can be asked "what should happen?" and the answer asserted,
 * which is the part that rots when a URI is edited.
 */

/*
 * What the caller must DO after the reply is sent. `kNone` is the read-only
 * endpoints, which is most of them.
 *
 * **The order of these values is not load-bearing**; nothing casts to or from an
 * integer, and a wire value is never derived from one.
 */
enum class MaintenanceAction {
    kNone = 0,
    kJoinWifi,      // POST /api/wifi: apply the credentials in the request body
    kOtaUpload,     // POST /api/ota/upload: stream the raw body to the updater
    kOtaCheck,      // POST /api/ota/check: fetch the manifest and compare versions
    kOtaPull,       // POST /api/ota/pull: download and install the release asset
    kReboot,        // POST /api/reboot: restart into the installed image
};

// One parsed request. Every pointer is borrowed for the call and must not be
// retained; null is legal for the optional ones and means "absent", not "empty
// string", so a caller cannot conflate a missing header with an empty one.
struct HttpRequest {
    const char *method;         // "GET" / "POST"
    const char *path;           // "/", "/api/status", ...
    const char *query;          // the raw query string after '?', or null
    const char *token_header;   // the `X-SWC-Token` header value, or null
    const char *content_type;   // the request's Content-Type, or null
    /*
     * `POST /api/ota/upload`'s two metadata headers. The body is the raw image
     * bytes, and these carry what the shared OTA gate needs BEFORE it writes a
     * byte (spec 9.4: `OtaBegin` validates the size and hash up front, so an
     * unacceptable image is refused before 4 MB of it reaches flash).
     *
     * **Why headers rather than a multipart form.** The page computes the digest
     * in JavaScript -- `crypto.subtle` is undefined outside a secure context and
     * this page is plain HTTP on a LAN address -- and a `FormData` body cannot
     * carry a field the firmware reads without a multipart parser to find it. A
     * raw body with the digest in a header removes the parser entirely, and the
     * digest is exactly what the device needs to verify what it received.
     *
     * Both are required for an upload and their absence is a 400, not a silent
     * accept: without them the gate cannot run, and "uploaded and verified" for an
     * image nothing verified is the failure this refusal exists to prevent.
     */
    const char *sha256_header;  // the `X-SWC-Sha256` header value, or null
    const char *size_header;    // the `X-SWC-Size` header value, or null
};

/*
 * The device's facts the page reports, so the router never reaches for global
 * state. Filled by the caller from the orchestrator, the HAL and the WiFi driver.
 *
 * `have_temp` is separate from `temp_tenths_c` rather than folded into a sentinel
 * because the sentinel is `0` (`SystemOrchestrator::kTempNotMeasuredTenths`) and
 * `0` is a legitimate reading of 0.0 C. A single field would make a real 0 C
 * indistinguishable from "no sensor", which is the confusion that sentinel was
 * introduced to avoid.
 */
struct MaintenanceFacts {
    const char   *fw_version;
    const char   *device_id;
    uint64_t      uptime_ms;
    const char   *config_state;
    bool          wifi_connected;
    const char   *wifi_ssid;     // "" when not connected
    bool          have_temp;
    int           temp_tenths_c;
    uint32_t      heap_free;
};

struct HttpOutcome {
    int               status;          // 200, 400, 401, 404, 405, 413, 500
    const char       *content_type;    // never null on a success
    size_t            body_len;        // bytes to send
    MaintenanceAction action;

    /*
     * When non-null, the body IS this pointer and `body_len` bytes of it, and the
     * caller's own buffer was not written. When null, the body is in the caller's
     * buffer.
     *
     * **Why a borrow exists at all.** The maintenance page is generated into
     * flash as a ~5 KB string constant (`WebPageAssets.h`), and the HTTP
     * handler's task has a small stack -- copying the page into a stack buffer
     * to send it would be exactly the `sizeof(Config)`-on-a-task-stack mistake
     * this project already made once (`swc-device-stack-overflow-config`). Since
     * the asset already lives in flash and its lifetime is the whole program, the
     * reply can point straight at it. The one caller-side cost is that it must
     * choose which pointer to send from, which is a single branch.
     */
    const char       *borrowed;
};

/*
 * Decide the reply. Writes at most `out_cap` bytes of a generated body into `out`
 * (NUL terminated), OR sets `outcome.borrowed` to a flash-resident asset and
 * leaves `out` untouched -- see `HttpOutcome::borrowed`. `info` is the OPEN
 * window's secrets; the token in it is what the request's token is checked
 * against.
 *
 * Returns 401 without touching `out` when the token does not match, and that is
 * the FIRST check for every path, including the page itself: the page is a static
 * asset with nothing secret in it, but gating it as well keeps one rule
 * ("everything on this server needs the token") rather than two, and the page's
 * own script reads the token from its query string precisely so a user can open
 * it once and have the session hold.
 *
 * A generated response that will not fit in `out_cap` is reported as 500 rather
 * than sent truncated: a truncated JSON body is a parse error on the device's own
 * page, which points nowhere near the real cause.
 */
HttpOutcome MaintenanceHttpRoute(const HttpRequest &req, const MaintenanceFacts &facts,
                                 const MaintenanceInfo &info, char *out, size_t out_cap);

/*
 * The reason string a 401 carries, exposed so the device's wire path and the
 * tests name the same token-header spelling. Spec 8.4's header is
 * `X-SWC-Token` and the page's own script sends exactly that; a server looking
 * for a different name would answer 401 to its own page with nothing to explain
 * why.
 */
constexpr const char *kMaintenanceTokenHeader = "X-SWC-Token";

/*
 * The upload's two metadata headers. Exposed beside the token header for the same
 * reason: the page's script and the firmware's reader must name the same
 * spellings, and a mismatch is an upload refused with "missing size" while the
 * page believes it sent one.
 */
constexpr const char *kMaintenanceSha256Header = "X-SWC-Sha256";
constexpr const char *kMaintenanceSizeHeader   = "X-SWC-Size";
