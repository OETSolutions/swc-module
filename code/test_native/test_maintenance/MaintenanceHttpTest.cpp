#include "Maintenance/MaintenanceHttp.h"

#include "Maintenance/WebPage.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

// A window whose token is a fixed, known value, so a request either matches or
// does not without depending on the MAC derivation.
MaintenanceInfo WindowWithToken(const char *token) {
    MaintenanceInfo info;
    info.active = true;
    std::snprintf(info.token, sizeof(info.token), "%s", token);
    return info;
}

MaintenanceFacts SampleFacts() {
    MaintenanceFacts f{};
    f.fw_version = "1.2.3";
    f.device_id = "swc-abc";
    f.uptime_ms = 123456;
    f.config_state = "ok";
    f.wifi_connected = true;
    f.wifi_ssid = "home";
    f.have_temp = true;
    f.temp_tenths_c = 215;
    f.heap_free = 100000;
    return f;
}

// Route with a request and a generously sized output buffer. The body is read
// from whichever pointer the outcome names -- the caller's buffer for a generated
// body, or the flash asset for a borrowed one -- so a test exercises the same
// choice the device's adapter makes.
//
// The upload's two metadata headers are supplied by default, because almost every
// test is about something else and a 400 from a missing header would mask it. The
// tests that are ABOUT those headers pass their own.
HttpOutcome Route(const char *method, const char *path, const char *token,
                  const MaintenanceInfo &info, const MaintenanceFacts &facts,
                  std::string *body, const char *sha = "00", const char *size = "1000") {
    HttpRequest req{};
    req.method = method;
    req.path = path;
    req.token_header = token;
    req.sha256_header = sha;
    req.size_header = size;
    char out[4096];
    const HttpOutcome r = MaintenanceHttpRoute(req, facts, info, out, sizeof(out));
    if (body != nullptr) {
        const char *p = (r.borrowed != nullptr) ? r.borrowed : out;
        *body = std::string(p, r.body_len);
    }
    return r;
}

}  // namespace

// The token gate is the whole security model of the page (spec 8.4: "no default
// password, no admin/admin"), so it is asserted FIRST and on every path.
TEST(MaintenanceHttp, RejectsAMissingTokenOnEveryPath) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();
    const char *kPaths[] = {"/",     "/index.html",      "/api/status",
                            "/api/wifi", "/api/ota/upload", "/api/reboot"};

    for (const char *p : kPaths) {
        const char *method = (std::strcmp(p, "/api/status") == 0) ? "GET" : "POST";
        // No header at all.
        EXPECT_EQ(Route(method, p, nullptr, info, facts, nullptr).status, 401) << p;
        // A wrong token.
        EXPECT_EQ(Route(method, p, "WRONG", info, facts, nullptr).status, 401) << p;
        // An empty token.
        EXPECT_EQ(Route(method, p, "", info, facts, nullptr).status, 401) << p;
    }
}

// A near-miss must not pass: the check is not a prefix compare. A token that
// shares a prefix with the real one is the case a byte-at-a-time early return
// would accept.
TEST(MaintenanceHttp, RejectsATokenThatIsOnlyAPrefix) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    EXPECT_EQ(Route("GET", "/api/status", "ABCDEF12345", info, facts, nullptr).status, 401);
    EXPECT_EQ(Route("GET", "/api/status", "ABCDEF1234567", info, facts, nullptr).status, 401);
    // The exact token passes, proving the above are the near-misses and not a
    // broken happy path.
    EXPECT_EQ(Route("GET", "/api/status", "ABCDEF123456", info, facts, nullptr).status, 200);
}

// The page is served byte-for-byte from the asset table, with the asset's own
// content type -- so a browser gets HTML and not a JSON sniff. It is BORROWED
// from flash rather than copied into the caller's buffer, because the page is
// ~5 KB and the HTTP task's stack is small.
TEST(MaintenanceHttp, ServesTheIndexPageForRootAndTheExplicitPath) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    for (const char *p : {"/", "/index.html"}) {
        std::string body;
        const HttpOutcome r = Route("GET", p, "ABCDEF123456", info, facts, &body);
        EXPECT_EQ(r.status, 200) << p;
        EXPECT_STREQ(r.content_type, WebPageContentType()) << p;
        const WebAsset *asset = WebPageFind(p);
        ASSERT_NE(asset, nullptr) << p;
        // Borrowed, and the borrow IS the asset.
        ASSERT_NE(r.borrowed, nullptr) << p;
        EXPECT_EQ(r.borrowed, asset->body) << p;
        ASSERT_EQ(body.size(), asset->len) << p;
        EXPECT_EQ(std::memcmp(body.data(), asset->body, asset->len), 0) << p;
    }
}

// The page is served from a buffer far smaller than the page, proving the reply
// does not depend on the caller having room for it. This is the regression guard
// for the stack-overflow shape: copying a ~5 KB asset into a small handler
// buffer is how this project already put an 8 KB `Config` on a 3.5 KB task stack.
TEST(MaintenanceHttp, ServesThePageEvenWhenTheCallersBufferIsTiny) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    HttpRequest req{};
    req.method = "GET";
    req.path = "/";
    req.token_header = "ABCDEF123456";

    char tiny[8];
    const HttpOutcome r = MaintenanceHttpRoute(req, facts, info, tiny, sizeof(tiny));
    ASSERT_EQ(r.status, 200);
    ASSERT_NE(r.borrowed, nullptr);
    const WebAsset *asset = WebPageFind("/");
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(r.body_len, asset->len);
}

// An unknown NON-api path is a 404, NOT the index. Serving the index for every
// URL is what makes a typo'd API call return HTML, which the page's script then
// fails to parse with no clue why.
TEST(MaintenanceHttp, AnUnknownPagePathIsNotFoundRatherThanTheIndex) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    const HttpOutcome r = Route("GET", "/nope.html", "ABCDEF123456", info, facts, nullptr);
    EXPECT_EQ(r.status, 404);
}

// An unknown path UNDER /api/ is a JSON 404, because the page's `api()` helper
// parses every API response and would render a plain-text body as a raw string.
TEST(MaintenanceHttp, AnUnknownApiPathIsAJsonNotFound) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    std::string body;
    const HttpOutcome r = Route("POST", "/api/nope", "ABCDEF123456", info, facts, &body);
    EXPECT_EQ(r.status, 404);
    EXPECT_NE(std::string(r.content_type).find("json"), std::string::npos);
    EXPECT_EQ(body.front(), '{');
}

// GET /api/status reports the four facts spec 8.4 names, in JSON.
TEST(MaintenanceHttp, StatusReportsTheSpecsFourFactsAsJson) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    std::string body;
    const HttpOutcome r = Route("GET", "/api/status", "ABCDEF123456", info, facts, &body);
    ASSERT_EQ(r.status, 200);
    ASSERT_GT(r.body_len, 0u);
    // The named facts, each present and carrying its value.
    EXPECT_NE(body.find("\"fw_version\":\"1.2.3\""), std::string::npos);
    EXPECT_NE(body.find("\"device_id\":\"swc-abc\""), std::string::npos);
    EXPECT_NE(body.find("\"uptime_ms\":123456"), std::string::npos);
    EXPECT_NE(body.find("\"config_state\":\"ok\""), std::string::npos);
    EXPECT_NE(body.find("\"wifi_connected\":true"), std::string::npos);
    EXPECT_NE(body.find("\"temp_c\":21.5"), std::string::npos);
}

// The temperature is written with the SAME integer split the USB `status` frame
// uses, because the xtensa printf has `%f` disabled -- a `%f` would print nothing
// on the device and work on the host, the exact divergence this project keeps
// finding. A negative reading must render its sign correctly ("-0.5", not
// "0.-5").
TEST(MaintenanceHttp, TemperatureUsesTheSameIntegerSplitAsTheUsbFrame) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");

    MaintenanceFacts f = SampleFacts();
    f.temp_tenths_c = -5;      // -0.5 C
    std::string body;
    ASSERT_EQ(Route("GET", "/api/status", "ABCDEF123456", info, f, &body).status, 200);
    EXPECT_NE(body.find("\"temp_c\":-0.5"), std::string::npos) << body;

    f.temp_tenths_c = -15;     // -1.5 C
    ASSERT_EQ(Route("GET", "/api/status", "ABCDEF123456", info, f, &body).status, 200);
    EXPECT_NE(body.find("\"temp_c\":-1.5"), std::string::npos) << body;
}

// No reading is `null`, never a fabricated 0.0 -- the sentinel means "nothing was
// measured", and a page showing 0.0 C would assert a reading.
TEST(MaintenanceHttp, NoTemperatureReadingIsNullNotZero) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    MaintenanceFacts f = SampleFacts();
    f.have_temp = false;
    f.temp_tenths_c = 0;       // the sentinel, which is also a legal 0.0 C

    std::string body;
    ASSERT_EQ(Route("GET", "/api/status", "ABCDEF123456", info, f, &body).status, 200);
    EXPECT_NE(body.find("\"temp_c\":null"), std::string::npos) << body;
    EXPECT_EQ(body.find("\"temp_c\":0.0"), std::string::npos) << body;
}

// A quote or backslash in the joined SSID must not close the JSON string early.
// The SSID is not this device's data, so it is escaped rather than trusted.
TEST(MaintenanceHttp, EscapesAnSsidThatWouldBreakTheJson) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    MaintenanceFacts f = SampleFacts();
    f.wifi_ssid = "ev\"il\\net";

    std::string body;
    ASSERT_EQ(Route("GET", "/api/status", "ABCDEF123456", info, f, &body).status, 200);
    EXPECT_NE(body.find("\"wifi_ssid\":\"ev\\\"il\\\\net\""), std::string::npos) << body;
    // And the object still closes, i.e. the quote did not terminate the string.
    EXPECT_EQ(body.back(), '}');
}

// Each mutating endpoint must return its OWN action, or the page's "join" and
// "install" buttons would run the same code path.
TEST(MaintenanceHttp, EachEndpointReturnsItsOwnAction) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    struct Case {
        const char       *path;
        MaintenanceAction action;
    };
    const Case kCases[] = {
        {"/api/wifi",       MaintenanceAction::kJoinWifi},
        {"/api/ota/upload", MaintenanceAction::kOtaUpload},
        {"/api/ota/check",  MaintenanceAction::kOtaCheck},
        {"/api/ota/pull",   MaintenanceAction::kOtaPull},
        {"/api/reboot",     MaintenanceAction::kReboot},
    };
    for (const Case &c : kCases) {
        const HttpOutcome r = Route("POST", c.path, "ABCDEF123456", info, facts, nullptr);
        EXPECT_EQ(r.status, 200) << c.path;
        EXPECT_EQ(static_cast<int>(r.action), static_cast<int>(c.action)) << c.path;
    }
}

// A GET to a mutating endpoint is refused, so a browser navigating to one cannot
// trip an action it did not mean to take.
TEST(MaintenanceHttp, AGetToAMutatingEndpointIsRefusedAndRunsNothing) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    const HttpOutcome r = Route("GET", "/api/reboot", "ABCDEF123456", info, facts, nullptr);
    EXPECT_EQ(r.status, 405);
    EXPECT_EQ(static_cast<int>(r.action), static_cast<int>(MaintenanceAction::kNone));
}

// The upload's two metadata headers are the shared gate's whole precondition
// (spec 9.4: the size and digest are validated BEFORE a byte is written), so a
// request without them is refused rather than starting a run that cannot verify.
TEST(MaintenanceHttp, AnUploadWithoutItsMetadataHeadersIsRefused) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    // No digest.
    EXPECT_EQ(Route("POST", "/api/ota/upload", "ABCDEF123456", info, facts, nullptr,
                    nullptr, "1000").status, 400);
    // No size.
    EXPECT_EQ(Route("POST", "/api/ota/upload", "ABCDEF123456", info, facts, nullptr,
                    "ab", nullptr).status, 400);
    // An empty digest.
    EXPECT_EQ(Route("POST", "/api/ota/upload", "ABCDEF123456", info, facts, nullptr,
                    "", "1000").status, 400);
    // A zero or unparsable size: the gate would refuse it anyway, and a 400 here
    // points at the request rather than at a mysterious verifier failure.
    //
    // `-5` is the case that matters most and the one a `strtoul(sz) > 0` check
    // MISSES: `strtoul` accepts a leading minus and WRAPS it to `ULONG_MAX - 4`,
    // which is greater than zero -- so a negative size passed the check and went
    // to the OTA gate as a ~4 exabyte image. Leading whitespace and `+` are the
    // same family: accepted by `strtoul`, not by a size header.
    for (const char *bad : {"0", "", "xyz", "-5", " 100", "+100", "1e6", "1000x"}) {
        EXPECT_EQ(Route("POST", "/api/ota/upload", "ABCDEF123456", info, facts, nullptr,
                        "ab", bad).status, 400) << "size=" << bad;
    }
    // Both present is accepted, and this is what proves the above are refusals of
    // the metadata and not of the endpoint.
    const HttpOutcome ok = Route("POST", "/api/ota/upload", "ABCDEF123456", info, facts,
                                 nullptr, "ab", "1000");
    EXPECT_EQ(ok.status, 200);
    EXPECT_EQ(static_cast<int>(ok.action), static_cast<int>(MaintenanceAction::kOtaUpload));
}

// The metadata check must not come BEFORE the token check, or a request with no
// token learns the request shape from a 400 instead of a 401. One rule for the
// server: an unauthenticated request learns nothing.
TEST(MaintenanceHttp, AnUnauthenticatedUploadIsA401NotA400) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    // No token AND no metadata: the 401 must win.
    EXPECT_EQ(Route("POST", "/api/ota/upload", nullptr, info, facts, nullptr,
                    nullptr, nullptr).status, 401);
    EXPECT_EQ(Route("POST", "/api/ota/upload", "WRONG", info, facts, nullptr,
                    nullptr, nullptr).status, 401);
}

// The status read is a GET; a POST to it is refused rather than silently
// answering, so the method contract is symmetric.
TEST(MaintenanceHttp, APostToStatusIsRefused) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    EXPECT_EQ(Route("POST", "/api/status", "ABCDEF123456", info, facts, nullptr).status, 405);
}

// A body that cannot fit must be a 500, never a truncated reply: a truncated
// JSON body is a parse error on the device's own page with no clue where it came
// from.
TEST(MaintenanceHttp, AReplyTooLargeForTheBufferIsAnErrorNotTruncation) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();

    HttpRequest req{};
    req.method = "GET";
    req.path = "/api/status";
    req.token_header = "ABCDEF123456";

    char tiny[8];
    const HttpOutcome r = MaintenanceHttpRoute(req, facts, info, tiny, sizeof(tiny));
    EXPECT_EQ(r.status, 500);
    EXPECT_EQ(r.body_len, 0u);
}

// Every GENERATED reply that writes into the caller's buffer must NUL-terminate
// it, because the tests and the device's adapter both treat the buffer as a C
// string at `body_len`. The borrowed page reply does not use the buffer at all,
// which is asserted by its own test above.
TEST(MaintenanceHttp, EveryGeneratedBodyIsNulTerminatedWithinTheBuffer) {
    const MaintenanceInfo info = WindowWithToken("ABCDEF123456");
    const MaintenanceFacts facts = SampleFacts();
    const char *kPaths[] = {"/api/status", "/api/wifi", "/api/nope", "/nope"};

    for (const char *p : kPaths) {
        char out[4096];
        std::memset(out, 'X', sizeof(out));
        HttpRequest req{};
        req.method = (std::strcmp(p, "/api/status") == 0) ? "GET" : "POST";
        req.path = p;
        req.token_header = "ABCDEF123456";
        const HttpOutcome r = MaintenanceHttpRoute(req, facts, info, out, sizeof(out));
        EXPECT_EQ(r.borrowed, nullptr) << p;   // these all write the buffer
        ASSERT_LT(r.body_len, sizeof(out)) << p;
        EXPECT_EQ(out[r.body_len], '\0') << p;
    }
}
