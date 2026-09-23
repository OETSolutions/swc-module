#include "Maintenance/MaintenanceHttp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Maintenance/WebPage.h"

namespace {

// Whether the request path names an API endpoint rather than the page. Used to
// decide 404-vs-index: an unknown PATH under `/api/` is a typo'd call and must
// NOT be answered with HTML, which would make a broken script look like it
// succeeded and then fail on a parse. That is the same reasoning
// `WebPageFind`'s comment gives for returning nullptr.
bool IsApiPath(const char *path) {
    return path != nullptr && strncmp(path, "/api/", 5) == 0;
}

// Exact equality for the method. A null method (a hand-written or truncated
// request) matches nothing, so it cannot reach a mutating branch by default.
bool MethodIs(const char *method, const char *expected) {
    if (method == nullptr) return false;
    return strcmp(method, expected) == 0;
}

// The JSON body for GET /api/status (spec 8.4: "fw version, device id, uptime,
// config state, WiFi state").
//
// Written by hand rather than through cJSON: the payload is a flat object of
// scalars with no nesting and no untrusted input, and the one string that IS
// untrusted (the joined SSID) is escaped below. cJSON would add an allocation on
// the request path for no benefit, and the device's HTTP handler runs on a task
// with a small stack.
//
// The temperature uses the SAME integer split as the USB `status` frame
// (CommandRouter::EmitStatusBody), because the newlib-nano `printf` on xtensa has
// `%f` disabled -- a `%f` here would print nothing on the device and work on the
// host, the exact host/device divergence this project keeps finding. One unit,
// one spelling.
int StatusBody(char *out, size_t cap, const MaintenanceFacts &f) {
    // A quote or backslash in an SSID must not close the JSON string early. The
    // SSID comes from whatever network the device joined, which is not this
    // device's data, so it is escaped rather than trusted.
    //
    // Sized from the 802.11 SSID maximum (32 bytes) with room for a backslash
    // before every character: escaping can at most DOUBLE the string, and the
    // driver cannot hand back an SSID longer than the standard allows.
    char ssid[2 * 32 + 1];
    size_t o = 0;
    for (const char *p = (f.wifi_ssid != nullptr) ? f.wifi_ssid : ""; *p != '\0' && o + 2 < sizeof(ssid); ++p) {
        if (*p == '"' || *p == '\\') ssid[o++] = '\\';
        ssid[o++] = *p;
    }
    ssid[o] = '\0';

    char temp[24];
    if (f.have_temp) {
        const int whole = f.temp_tenths_c / 10;
        int frac = f.temp_tenths_c % 10;
        const char *sign = "";
        if (f.temp_tenths_c < 0) {
            if (frac < 0) frac = -frac;
            if (whole == 0) sign = "-";
        }
        snprintf(temp, sizeof(temp), "%s%d.%d", sign, whole, frac);
    } else {
        // `null`, never a fabricated 0.0 -- the sentinel means "nothing was
        // measured" and a page showing 0.0 C would be asserting a reading.
        snprintf(temp, sizeof(temp), "null");
    }

    const int n = snprintf(out, cap,
                           "{"
                           "\"fw_version\":\"%s\","
                           "\"device_id\":\"%s\","
                           "\"uptime_ms\":%llu,"
                           "\"config_state\":\"%s\","
                           "\"wifi_connected\":%s,"
                           "\"wifi_ssid\":\"%s\","
                           "\"temp_c\":%s,"
                           "\"heap_free\":%u"
                           "}",
                           (f.fw_version != nullptr) ? f.fw_version : "unknown",
                           (f.device_id != nullptr) ? f.device_id : "",
                           static_cast<unsigned long long>(f.uptime_ms),
                           (f.config_state != nullptr) ? f.config_state : "unknown",
                           f.wifi_connected ? "true" : "false",
                           ssid, temp, static_cast<unsigned>(f.heap_free));
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    return n;
}

// A one-line JSON status/error body, the shape the page's `api()` helper turns
// into a thrown Error on a non-2xx.
int SimpleBody(char *out, size_t cap, const char *msg) {
    const int n = snprintf(out, cap, "{\"message\":\"%s\"}", msg);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    return n;
}

}  // namespace

HttpOutcome MaintenanceHttpRoute(const HttpRequest &req, const MaintenanceFacts &facts,
                                 const MaintenanceInfo &info, char *out, size_t out_cap) {
    HttpOutcome r{};
    r.status = 500;
    r.content_type = "application/json";
    r.body_len = 0;
    r.action = MaintenanceAction::kNone;

    if (out == nullptr || out_cap == 0) return r;
    out[0] = '\0';

    // The token gate FIRST, before the path is even looked at. One rule for the
    // whole server: an unauthenticated request learns nothing, not even which
    // paths exist, so a scanner cannot map the API by watching 404 vs 401.
    if (!WebTokenMatches(req.token_header, info.token)) {
        r.status = 401;
        const int n = SimpleBody(out, out_cap, "missing or bad token");
        r.body_len = (n < 0) ? 0 : static_cast<size_t>(n);
        return r;
    }

    const char *path = (req.path != nullptr) ? req.path : "";

    // --- the static page ---------------------------------------------------
    if (!IsApiPath(path)) {
        if (!MethodIs(req.method, "GET")) {
            r.status = 405;
            const int n = SimpleBody(out, out_cap, "GET only");
            r.body_len = (n < 0) ? 0 : static_cast<size_t>(n);
            return r;
        }
        const WebAsset *asset = WebPageFind(path);
        if (asset == nullptr) {
            // A path that is not the page and not under /api/ is a 404, NOT the
            // index: serving the index for every URL makes a typo'd API call
            // return HTML, which the script then fails to parse with no clue why.
            r.status = 404;
            r.content_type = "text/plain; charset=utf-8";
            const int n = snprintf(out, out_cap, "not found\n");
            r.body_len = (n < 0) ? 0 : static_cast<size_t>(n);
            return r;
        }
        // BORROWED, not copied: the asset is a flash-resident constant whose
        // lifetime is the whole program, and the page is ~5 KB -- larger than any
        // buffer this handler should hold on its task's stack. The caller sends
        // straight from flash. See `HttpOutcome::borrowed`.
        r.status = 200;
        r.content_type = WebPageContentType();
        r.body_len = asset->len;
        r.borrowed = asset->body;
        return r;
    }

    // --- the API -----------------------------------------------------------
    // Every API endpoint is a POST except the status read. Policing the method
    // here rather than in each branch is what keeps a GET to a mutating endpoint
    // from reaching the action dispatch and doing nothing silently.
    const bool is_status = (strcmp(path, "/api/status") == 0);

    if (is_status) {
        if (!MethodIs(req.method, "GET")) {
            r.status = 405;
            const int n = SimpleBody(out, out_cap, "GET only");
            r.body_len = (n < 0) ? 0 : static_cast<size_t>(n);
            return r;
        }
        const int n = StatusBody(out, out_cap, facts);
        if (n < 0) {
            r.status = 500;
            r.content_type = "text/plain; charset=utf-8";
            r.body_len = 0;
            return r;
        }
        r.status = 200;
        r.body_len = static_cast<size_t>(n);
        return r;
    }

    if (!MethodIs(req.method, "POST")) {
        r.status = 405;
        const int n = SimpleBody(out, out_cap, "POST only");
        r.body_len = (n < 0) ? 0 : static_cast<size_t>(n);
        return r;
    }

    struct Endpoint {
        const char       *path;
        MaintenanceAction action;
    };
    static const Endpoint kEndpoints[] = {
        {"/api/wifi",       MaintenanceAction::kJoinWifi},
        {"/api/ota/upload", MaintenanceAction::kOtaUpload},
        {"/api/ota/check",  MaintenanceAction::kOtaCheck},
        {"/api/ota/pull",   MaintenanceAction::kOtaPull},
        {"/api/reboot",     MaintenanceAction::kReboot},
    };

    for (const Endpoint &e : kEndpoints) {
        if (strcmp(path, e.path) != 0) continue;

        /*
         * The upload is validated HERE, before the action is dispatched, because
         * the two headers are the gate's whole precondition. A missing or
         * malformed one is a 400 the page can act on, rather than an action that
         * starts and then has nothing to verify with.
         *
         * The checks are structural only -- presence, and that the size parses --
         * so the DIGEST ITSELF is left to `ImageVerifyBegin`, the one gate both
         * OTA paths share (spec 9.4). Re-checking the hash format here would be a
         * second implementation of "is this a valid digest", which is exactly the
         * duplication the shared gate exists to prevent.
         */
        if (e.action == MaintenanceAction::kOtaUpload) {
            const bool have_hash = (req.sha256_header != nullptr) && (req.sha256_header[0] != '\0');
            const char *sz = (req.size_header != nullptr) ? req.size_header : "";

            /*
             * A strict decimal parse, not `strtoul`.
             *
             * `strtoul` ACCEPTS a leading minus and wraps it: `strtoul("-5")` is
             * `ULONG_MAX - 4`, which is > 0, so a negative size would pass a
             * `> 0` check and be handed to the OTA gate as a ~4 exabyte image. It
             * also accepts leading whitespace and a `+` sign. The size is a
             * header a peer controls, so it is parsed here rather than trusted:
             * digits only, at least one, and the value must fit the slot the gate
             * will check against anyway.
             */
            bool size_ok = (sz[0] != '\0');
            for (const char *p = sz; *p != '\0'; ++p) {
                if (*p < '0' || *p > '9') {
                    size_ok = false;
                    break;
                }
            }
            // Reject a leading zero run that would parse to nothing useful, and
            // reject zero itself: an empty image is refused by the gate anyway,
            // and a 400 here names the request rather than the verifier.
            if (size_ok && strtoul(sz, nullptr, 10) == 0) size_ok = false;

            if (!have_hash || !size_ok) {
                r.status = 400;
                const int n = SimpleBody(out, out_cap,
                                         "upload needs X-SWC-Sha256 and X-SWC-Size");
                r.body_len = (n < 0) ? 0 : static_cast<size_t>(n);
                return r;
            }
        }

        r.action = e.action;
        // The reply is written BEFORE the action runs, so the page gets a 2xx
        // and the adapter then performs the work. `kJoinWifi` and `kOtaPull`
        // both restart the radio and drop the connection, so a body they wrote
        // after the fact would never arrive -- the page's own comment says the
        // reboot drop "is not a failure worth alarming the user about".
        const char *msg = "accepted";
        switch (e.action) {
            case MaintenanceAction::kJoinWifi:   msg = "joining"; break;
            case MaintenanceAction::kOtaUpload:  msg = "upload accepted"; break;
            case MaintenanceAction::kOtaCheck:   msg = "checking"; break;
            case MaintenanceAction::kOtaPull:    msg = "installing"; break;
            case MaintenanceAction::kReboot:     msg = "rebooting"; break;
            case MaintenanceAction::kNone:       break;
        }
        const int n = SimpleBody(out, out_cap, msg);
        if (n < 0) {
            r.status = 500;
            r.action = MaintenanceAction::kNone;
            r.body_len = 0;
            return r;
        }
        r.status = 200;
        r.body_len = static_cast<size_t>(n);
        return r;
    }

    // An unknown path under /api/ is a 404 with a JSON body -- JSON, not the
    // plain-text 404 the page paths get, because the page's `api()` helper parses
    // every API response and would render a plain-text 404 as a raw string.
    r.status = 404;
    r.content_type = "application/json";
    const int n = SimpleBody(out, out_cap, "unknown endpoint");
    r.body_len = (n < 0) ? 0 : static_cast<size_t>(n);
    return r;
}
