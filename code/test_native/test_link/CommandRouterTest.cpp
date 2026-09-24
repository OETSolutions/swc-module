#include "Link/CommandRouter.h"

#include "Config/ConfigCodec.h"
#include "Config/ConfigDefaults.h"
#include "Config/ConfigStore.h"
#include "Link/Ndjson.h"
#include "Link/UsbCdc.h"
#include "MockHAL.h"
#include "Update/OtaUsb.h"
#include "Util/Base64.h"
#include "Util/Sha256.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Capture {
    std::vector<std::string> lines;
    static void Sink(void *ctx, const char *line, size_t len) {
        static_cast<Capture *>(ctx)->lines.push_back(std::string(line, len));
    }
    void Attach(CommandRouter &r) { r.SetSink(&Sink, this); }
};

bool HasType(const Capture &c, const char *type) {
    const std::string needle = std::string("\"type\":\"") + type + "\"";
    for (const auto &l : c.lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The sense reading that yields a 4980 mV KEY idle -- a 5 V head unit, which
// selects gain 1.82. Needed by the tests that drive a REAL orchestrator behind
// the router, so the output stage is in the state a 5 V head unit puts it in.
constexpr int kSenseFor5vHeadUnit = 2490;

std::string B64(const std::string &raw) {
    char enc[8192];
    const size_t n = Base64Encode(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(),
                                  enc, sizeof(enc));
    return std::string(enc, n);
}

void SendConfigBegin(CommandRouter &r, uint32_t seq, size_t total_len, uint32_t crc32) {
    char buf[256];
    const int n = std::snprintf(buf, sizeof(buf),
        "{\"v\":1,\"seq\":%u,\"type\":\"config_begin\",\"total_len\":%u,\"crc32\":%u}",
        seq, static_cast<unsigned>(total_len), crc32);
    r.OnLine(buf, static_cast<size_t>(n));
}

void SendConfigChunks(CommandRouter &r, const char *raw, size_t len) {
    uint32_t seq = 100;
    for (size_t off = 0; off < len; off += kConfigWireChunkBytes) {
        const size_t n = (len - off < kConfigWireChunkBytes) ? (len - off) : kConfigWireChunkBytes;
        const std::string b64 = B64(std::string(raw + off, n));
        const std::string line = "{\"v\":1,\"seq\":" + std::to_string(seq++) +
            ",\"type\":\"config_chunk\",\"offset\":" + std::to_string(off) +
            ",\"data_b64\":\"" + b64 + "\"}";
        r.OnLine(line.c_str(), line.size());
    }
}

void SendConfigEnd(CommandRouter &r, uint32_t seq, const char *raw, size_t len) {
    char hex[65];
    Sha256Hex(reinterpret_cast<const uint8_t *>(raw), len, hex);
    const std::string line = "{\"v\":1,\"seq\":" + std::to_string(seq) +
        ",\"type\":\"config_end\",\"sha256\":\"" + std::string(hex, 64) + "\"}";
    r.OnLine(line.c_str(), line.size());
}

void SendConfigChunked(CommandRouter &r, uint32_t seq, const std::string &raw) {
    SendConfigBegin(r, seq, raw.size(),
                    Crc32(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()));
    SendConfigChunks(r, raw.data(), raw.size());
    SendConfigEnd(r, seq + 1, raw.data(), raw.size());
}

// Drive the outbound reply run to completion: Process() emits ONE frame per
// call by design, so a test that called it once would see only config_begin.
void DrainReplies(CommandRouter &r, int max_frames = 64) {
    for (int i = 0; i < max_frames; ++i) r.Process();
}

std::string EncodeConfig(const Config &c) {
    char *json = new char[ConfigMaxSerializedSize() + 1]();
    const size_t n = ConfigEncodeJson(c, json, ConfigMaxSerializedSize());
    EXPECT_GT(n, 0u);
    std::string s(json, n);
    delete[] json;
    return s;
}
}  // namespace

TEST(CommandRouter, ConnectEmitsHelloWithTheProtocolVersion) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.OnConnected();
    ASSERT_TRUE(HasType(cap, "hello"));
    EXPECT_NE(cap.lines[0].find("\"v\":1"), std::string::npos);
}

// `caps` must name only what this build can actually do. It advertised "ota"
// while the dispatcher nacked every `ota_*` frame as `not_implemented` (spec open
// item N-14), so a client trusting `caps[]` would offer an update flow that can
// never succeed. The assertion is written against the DISPATCHER's behaviour, not
// against a hardcoded string: it starts an `ota_begin` and requires the reply to
// contradict the advertisement, so the two cannot drift apart again -- wiring the
// router to `OtaUsb` will make this test fail until `caps` is updated too.
TEST(CommandRouter, HelloAdvertisesOtaOnlyIfTheDispatcherImplementsIt) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.OnConnected();
    ASSERT_TRUE(HasType(cap, "hello"));
    const bool advertises_ota = cap.lines[0].find("\"ota\"") != std::string::npos;

    cap.lines.clear();
    const std::string begin =
        "{\"v\":1,\"seq\":2,\"type\":\"ota_begin\",\"size_bytes\":1000,"
        "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
    r.OnLine(begin.c_str(), begin.size());
    // A refusal is an `nack` whose "err" is `not_implemented` (that is a nack
    // REASON, not a frame type, so `HasType` cannot see it).
    const bool dispatcher_refuses =
        HasType(cap, "nack") && cap.lines.back().find("\"err\":\"not_implemented\"") != std::string::npos;

    EXPECT_EQ(advertises_ota, !dispatcher_refuses)
        << "hello's caps[] and the dispatcher disagree about OTA: advertised="
        << advertises_ota << " refused=" << dispatcher_refuses;
}

TEST(CommandRouter, ConnectBeginsTheConfigRunSoTheAppCanRenderImmediately) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.OnConnected();
    DrainReplies(r);
    ASSERT_TRUE(HasType(cap, "config_begin")) << "the app must be able to render without asking";
    EXPECT_TRUE(HasType(cap, "config_chunk"));
    EXPECT_TRUE(HasType(cap, "config_end"));
    // Every emitted frame must respect the cap, or the run cannot be received.
    for (const auto &l : cap.lines) EXPECT_LT(l.size(), kNdjsonMaxFrame) << l;
}

// --- spec 4.3's `maintenance`: the delivery path for the two per-device secrets
// (spec 8.3 option 1). The board has no display, so the PoP and the page token
// reach the user OVER this link or not at all -- so what the router writes here
// IS the feature.

namespace {
MaintenanceInfo OpenWindow(const char *pop, const char *token) {
    MaintenanceInfo info;
    info.active = true;
    std::snprintf(info.pop, sizeof(info.pop), "%s", pop);
    std::snprintf(info.token, sizeof(info.token), "%s", token);
    std::snprintf(info.page_url, sizeof(info.page_url), "http://192.168.4.1/?token=%s", token);
    std::snprintf(info.ble_name, sizeof(info.ble_name), "%s", "A1B2");
    return info;
}

// The line carrying the `maintenance` frame, or "" when none was emitted.
std::string MaintenanceFrame(const Capture &cap) {
    for (const auto &l : cap.lines) {
        if (l.find("\"type\":\"maintenance\"") != std::string::npos) return l;
    }
    return "";
}
}  // namespace

// The secrets reach the app with their values, so a user can type the PoP and
// open the page. A frame that carried the fields but not the values would satisfy
// the contract's field list and deliver nothing.
TEST(CommandRouter, TheOpenWindowsSecretsAndPageUrlReachTheApp) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    const MaintenanceInfo info = OpenWindow("A1B2C3", "ABCDEF123456");
    r.SetMaintenanceInfo(info, 0);

    const std::string f = MaintenanceFrame(cap);
    ASSERT_FALSE(f.empty()) << "the window opened and no maintenance frame went out";
    EXPECT_NE(f.find("\"active\":true"), std::string::npos) << f;
    EXPECT_NE(f.find("\"pop\":\"A1B2C3\""), std::string::npos) << f;
    EXPECT_NE(f.find("\"token\":\"ABCDEF123456\""), std::string::npos) << f;
    EXPECT_NE(f.find("\"page_url\":\"http://192.168.4.1/?token=ABCDEF123456\""), std::string::npos)
        << f;
    EXPECT_NE(f.find("\"ble_name\":\"A1B2\""), std::string::npos) << f;
    EXPECT_NE(f.find("\"ble_failures\":0"), std::string::npos) << f;
}

// A CLOSED window must carry no secret. This is the property that stops a stale
// PoP being read off a window that has already shut -- and the radio's own
// teardown clears the struct, so an all-empty frame alongside `active:false` is
// the honest report rather than an omission.
TEST(CommandRouter, AClosedWindowCarriesNoSecret) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    // Open, then close.
    r.SetMaintenanceInfo(OpenWindow("A1B2C3", "ABCDEF123456"), 0);
    cap.lines.clear();
    MaintenanceInfo closed;   // active=false, every string empty
    r.SetMaintenanceInfo(closed, 0);

    const std::string f = MaintenanceFrame(cap);
    ASSERT_FALSE(f.empty()) << "a closing window must be reported, or the app shows it open forever";
    EXPECT_NE(f.find("\"active\":false"), std::string::npos) << f;
    EXPECT_NE(f.find("\"pop\":\"\""), std::string::npos) << f;
    EXPECT_NE(f.find("\"token\":\"\""), std::string::npos) << f;
}

// The frame is emitted on CHANGE and not every tick. `SetMaintenanceInfo` runs
// from the poll loop, so an unconditional emit would flood the link at 100 Hz
// with a frame whose content never changes -- filling the TX buffer that a real
// reply then cannot use.
TEST(CommandRouter, AnUnchangedWindowIsNotReEmitted) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    const MaintenanceInfo info = OpenWindow("A1B2C3", "ABCDEF123456");
    r.SetMaintenanceInfo(info, 0);
    const size_t after_first = cap.lines.size();
    ASSERT_NE(MaintenanceFrame(cap), "");

    for (int i = 0; i < 20; ++i) r.SetMaintenanceInfo(info, 0);
    EXPECT_EQ(cap.lines.size(), after_first)
        << "an unchanged window re-emitted " << (cap.lines.size() - after_first)
        << " frames; the poll loop calls this every tick";
}

// A radio that FAILED to come up must be distinguishable from one that came up,
// or the app cannot tell the user their setup page is not there (the N-76 shape:
// a user-facing surface promising a capability the device does not have). The
// count is the signal, so a change in it must emit.
TEST(CommandRouter, ARadioFailureIsReportedEvenWhenTheWindowIsUnchanged) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    // The window is open but the radio never came up: no secrets, a failure count.
    MaintenanceInfo failed;
    failed.active = true;
    r.SetMaintenanceInfo(failed, 0);
    cap.lines.clear();
    r.SetMaintenanceInfo(failed, 1);

    const std::string f = MaintenanceFrame(cap);
    ASSERT_FALSE(f.empty()) << "the failure count changed and nothing went out";
    EXPECT_NE(f.find("\"active\":true"), std::string::npos) << f;
    EXPECT_NE(f.find("\"ble_failures\":1"), std::string::npos) << f;
}

// An app that connects into an ALREADY-open window must learn the state without
// asking. `hello` carries no maintenance fields (spec 4.3), so the frame has to
// be sent on connect as well -- otherwise a user who opened the window before
// plugging the app in sees nothing.
TEST(CommandRouter, ConnectReportsAnAlreadyOpenWindow) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    // The window opened while the app was away, so the first frame was emitted
    // with no sink attached.
    r.SetMaintenanceInfo(OpenWindow("A1B2C3", "ABCDEF123456"), 0);
    cap.lines.clear();

    r.OnConnected();
    const std::string f = MaintenanceFrame(cap);
    ASSERT_FALSE(f.empty()) << "a connecting app was not told the window is open";
    EXPECT_NE(f.find("\"pop\":\"A1B2C3\""), std::string::npos) << f;
}

// A window that OPENS after a previous window's radio FAILED must report itself
// as healthy. This is the wire half of N-81: the device used to keep a running
// failure total that no window reset, so the frame for a perfectly good later
// window carried `ble_failures: 1` -- and the app branches on that FIRST, so the
// user was told "its radio did not come up, so there is no setup page to open"
// while `page_url` in the same frame pointed at the page.
//
// The router carries whatever the device hands it, so what this pins is the two
// facts arriving TOGETHER and consistently: a window with a page_url and a PoP
// is healthy, and a healthy window says so.
TEST(CommandRouter, AWindowAfterAFailedOneReportsItselfAsHealthy) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    // The first window: the radio failed, so no secrets and a failure count.
    MaintenanceInfo failed;
    failed.active = true;
    r.SetMaintenanceInfo(failed, 1);
    ASSERT_NE(MaintenanceFrame(cap).find("\"ble_failures\":1"), std::string::npos);
    cap.lines.clear();

    // It closes. The device clears its failure state with the window
    // (`MaintenanceRadioStop`), so the closing frame is clean...
    MaintenanceInfo closed;
    r.SetMaintenanceInfo(closed, 0);
    const std::string closing = MaintenanceFrame(cap);
    ASSERT_FALSE(closing.empty()) << "a closing window must be reported";
    EXPECT_NE(closing.find("\"ble_failures\":0"), std::string::npos) << closing;
    cap.lines.clear();

    // ...and the NEXT window opens with a working radio, so its frame must carry
    // the page and the secrets and no failure. A sticky count would fail here.
    const MaintenanceInfo good = OpenWindow("A1B2C3", "ABCDEF123456");
    r.SetMaintenanceInfo(good, 0);
    const std::string f = MaintenanceFrame(cap);
    ASSERT_FALSE(f.empty()) << "the second window opened and nothing went out";
    EXPECT_NE(f.find("\"ble_failures\":0"), std::string::npos) << f;
    EXPECT_NE(f.find("\"page_url\":\"http://192.168.4.1/?token=ABCDEF123456\""), std::string::npos)
        << f;
    EXPECT_NE(f.find("\"pop\":\"A1B2C3\""), std::string::npos) << f;
}

TEST(CommandRouter, AConfigGetRepliesWithAWholeChunkedRunThatRoundTrips) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string get = "{\"v\":1,\"seq\":1,\"type\":\"config_get\"}";
    r.OnLine(get.c_str(), get.size());
    DrainReplies(r);

    // Reassemble the way the protocol actually defines it: `offset` is the byte
    // offset of the chunk's first DECODED byte (spec 4.2), so each chunk decodes
    // INDEPENDENTLY and lands at its offset.
    //
    // Concatenating the `data_b64` strings and decoding once is the obvious but
    // wrong approach, and an earlier version of this test did exactly that: each
    // chunk is separately base64'd, so every chunk except the last carries its
    // own '=' padding, and the joined text has '=' in the middle -- which a
    // correct decoder rejects. The bug was in the test's reassembly, not the
    // wire format.
    uint8_t raw[65536];
    size_t highest = 0;
    int chunks = 0;
    for (const auto &l : cap.lines) {
        const size_t p = l.find("\"data_b64\":\"");
        if (p == std::string::npos) continue;
        const size_t start = p + 12;
        const size_t end = l.find('"', start);
        const std::string b64 = l.substr(start, end - start);
        const size_t op = l.find("\"offset\":");
        ASSERT_NE(op, std::string::npos) << l;
        const size_t off = static_cast<size_t>(std::stoul(l.substr(op + 9)));

        uint8_t decoded[kConfigWireChunkBytes];
        size_t dn = 0;
        ASSERT_TRUE(Base64Decode(b64.c_str(), b64.size(), decoded, sizeof(decoded), &dn)) << l;
        ASSERT_LE(off + dn, sizeof(raw)) << "the run overflows the reassembly buffer";
        memcpy(raw + off, decoded, dn);
        if (off + dn > highest) highest = off + dn;
        ++chunks;
    }
    EXPECT_GT(chunks, 1) << "a multi-chunk run is the case that matters";
    Config out{};
    EXPECT_TRUE(ConfigDecodeJson(reinterpret_cast<const char *>(raw), highest, &out));
}

TEST(CommandRouter, TheDefaultConfigRoundTripsThroughTheWire) {
    // The config the device REPLIES with must be decodable by the codec that
    // encoded it. This was broken: ConfigDefault() left `device_id` empty, and
    // ConfigDecodeJson's ReadStr refuses an empty string -- so the device sent a
    // config it could not itself read, and nothing caught it because
    // ConfigValidate did not check device_id. A config that cannot survive its
    // own round trip is worse than an invalid one: it looks fine until the app
    // echoes it back.
    Config c{};
    ConfigDefault(&c);
    EXPECT_TRUE(ConfigValidate(c));

    char buf[ConfigMaxSerializedSize() + 1];
    const size_t n = ConfigEncodeJson(c, buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    Config out{};
    EXPECT_TRUE(ConfigDecodeJson(buf, n, &out));
    EXPECT_STREQ(out.device_id, c.device_id);
}

TEST(CommandRouter, ConfigDefaultFullyOverwritesItsOutParameter) {
    // `ConfigDefault(&out)` is the shape that replaces the by-value factory, and
    // it is what keeps an 8,912-byte object off the 3,584-byte main task's stack
    // (see tools/check_stack_usage.py). The risk that comes with an out-parameter
    // is the opposite of the risk that came with a return value: a return value
    // cannot leave stale fields behind, but a function that fills `*out` field by
    // field can. So this starts from a config that differs from the default in
    // every region -- id, settings, a learned button, bindings, aux -- and
    // asserts the result is the DEFAULT and not a blend.
    Config c = MockHalDefaultsConfig();
    ASSERT_NE(c.channel_count, 0);
    ASSERT_GT(c.binding_count, 0u) << "the fixture must differ from the default";
    ASSERT_GT(c.channels[0].ladder.count, 0u);

    ConfigDefault(&c);

    // Channel 0's ladder is EMPTY in the default (nothing learned yet) -- the
    // most likely field to survive a partial overwrite, and the one whose
    // survival would be worst (it would report learned buttons on a fresh board).
    EXPECT_EQ(c.channels[0].ladder.count, 0u);
    EXPECT_EQ(c.binding_count, 0u);
    EXPECT_STREQ(c.device_id, "SWC-0000");
    EXPECT_EQ(c.channel_count, kMaxChannels);
    EXPECT_TRUE(ConfigValidate(c));

    // The ARRAY ENTRIES behind the zeroed counts must be cleared too. A count
    // field gates its array, so stale entries there are unread today -- which is
    // exactly why they are worth asserting: the contract is "out IS the default",
    // and a leftover binding or button that only a zero count hides is a live
    // hazard the first time anything reads the array before the count.
    EXPECT_EQ(c.bindings[0].id[0], '\0');
    EXPECT_EQ(c.bindings[0].action_count, 0);
    EXPECT_EQ(c.channels[0].ladder.buttons[0].mv_center, 0);
    EXPECT_EQ(c.channels[0].ladder.buttons[0].id[0], '\0');
}

TEST(CommandRouter, ConfigDefaultToleratesANullOutParameter) {
    // A null pointer is a caller bug, not a crash: this is called from a boot
    // path that reboots on failure, so aborting in a library call would take the
    // device down rather than report the misuse.
    ConfigDefault(nullptr);
}

TEST(CommandRouter, AnEmptyDeviceIdIsRefusedRatherThanBecomingUnreadable) {
    Config c = MockHalDefaultsConfig();
    c.device_id[0] = '\0';
    EXPECT_FALSE(ConfigValidate(c))
        << "an empty id validates but cannot be decoded, so it must not pass validation";
}

TEST(CommandRouter, ASequenceNumberIsAssignedMonotonicallyPerDirection) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.OnConnected();
    const uint32_t first = r.LastSeenSeqSent();
    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    r.OnConnected();
    EXPECT_GT(r.LastSeenSeqSent(), first);
}

TEST(CommandRouter, PingIsAnsweredWithStatusCarryingForSeq) {
    // Spec 4.3: `ping` is "Liveness; FW answers `status`". There is no `pong`
    // frame type, and the reply does NOT echo the peer's `seq` -- `seq` is each
    // sender's OWN monotonic counter (spec 4.2), so echoing it would break the
    // counter and make the app's gap detection fire on every ping.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string p = "{\"v\":1,\"seq\":77,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "status"));
    EXPECT_NE(cap.lines[0].find("\"for_seq\":77"), std::string::npos);
    EXPECT_EQ(cap.lines[0].find("\"seq\":77"), std::string::npos)
        << "the reply's own seq must be the firmware's counter, not the peer's";
}

TEST(CommandRouter, TheFirmwareSendsAPeriodicStatusWhileConnected) {
    // Spec 4.4: "Firmware sends `status` every 2 s when connected." The method
    // that does it had NO caller -- the device emitted a status only in reply to
    // `ping`/`status_get` -- so the keepalive §4.4 promises was never sent and a
    // quiet app saw nothing until it asked. This drives the poll-loop tick.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    // A frame proves the peer exists; the periodic status is armed from here.
    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    const size_t after_ping = cap.lines.size();   // the ping reply

    // The first tick only seeds the clock, so no status yet.
    r.Tick();
    EXPECT_EQ(cap.lines.size(), after_ping) << "no status before one period elapses";

    // Just short of the period: still nothing.
    hal.AdvanceMs(1999);
    r.Tick();
    EXPECT_EQ(cap.lines.size(), after_ping) << "1999 ms is not yet 2 s";

    // At the period boundary: exactly one status.
    hal.AdvanceMs(1);
    r.Tick();
    ASSERT_EQ(cap.lines.size(), after_ping + 1u);
    EXPECT_NE(cap.lines.back().find("\"type\":\"status\""), std::string::npos)
        << "the periodic frame is a `status`";
}

TEST(CommandRouter, ThePeriodicStatusCarriesNoForSeq) {
    // The keepalive answers NOTHING, so it must not name a `for_seq`. It used to
    // carry the firmware's own outbound counter, which made an unsolicited status
    // look like the answer to whichever request happened to share that number.
    // The app completes a pending request on a matching `for_seq` and both
    // counters start near zero, so a keepalive landing between a refused
    // `config_chunk` and its own nack could complete the waiter first and report
    // the refusal as success -- "config saved" for a config never accepted.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "status")) << "the ping reply is a status";
    // The REPLY does carry one -- it is answering the ping.
    EXPECT_NE(cap.lines[0].find("\"for_seq\":1"), std::string::npos);

    r.Tick();              // seed the status clock
    hal.AdvanceMs(2000);
    r.Tick();              // the periodic status
    ASSERT_EQ(cap.lines.size(), 2u) << "one ping reply, one periodic status";
    EXPECT_NE(cap.lines.back().find("\"type\":\"status\""), std::string::npos);
    EXPECT_EQ(cap.lines.back().find("\"for_seq\""), std::string::npos)
        << "an unsolicited keepalive must not claim to answer a request";
    // The body is otherwise unchanged -- the fields the app renders are still
    // there, so the guard above did not drop the payload along with the field.
    EXPECT_NE(cap.lines.back().find("\"config_state\""), std::string::npos);
    EXPECT_NE(cap.lines.back().find("\"output_safe\""), std::string::npos);
}

TEST(CommandRouter, APeriodicStatusNeedsAConnectedLink) {
    // With no peer, a periodic status would fill the small TX buffer with frames
    // nothing drains, and the buffer would then refuse a real reply.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    hal.AdvanceMs(10000);
    r.Tick();
    EXPECT_TRUE(cap.lines.empty()) << "no peer, so no periodic status";
}

TEST(CommandRouter, DisconnectingStopsThePeriodicStatus) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.OnConnected();                 // emits hello, opens a config reply run
    r.OnDisconnected();
    const size_t after = cap.lines.size();

    hal.AdvanceMs(10000);
    r.Tick();
    EXPECT_EQ(cap.lines.size(), after) << "a disconnected link sends no status";
}

TEST(CommandRouter, AnUnknownCommandTypeIsNackedNotIgnored) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string t = "{\"v\":1,\"seq\":5,\"type\":\"teleport\"}";
    r.OnLine(t.c_str(), t.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines[0].find("unknown_type"), std::string::npos);
}

// The nack's `detail` echoes a PEER-SUPPLIED string for three errors (an unknown
// `type`, an unknown patch `path`, an unknown identify `pattern`). It was placed
// into the JSON body with a bare `%s`, so a value containing a quote -- or one
// long enough that snprintf cut the body -- produced a nack that no longer parses
// as JSON, and the app lost the error it was being sent and saw only a malformed
// line. The body must survive a hostile value and stay a coherent frame.
TEST(CommandRouter, ANackDetailIsEscapedSoAHostileValueCannotBreakTheFrame) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    // A `type` carrying a quote and injected structure. The envelope parser
    // accepts it (it is a well-formed string value) and the unknown-type path
    // echoes it back.
    const std::string evil = "{\"v\":1,\"seq\":9,\"type\":\"bo\\\"gus\"}";
    r.OnLine(evil.c_str(), evil.size());
    ASSERT_TRUE(HasType(cap, "nack"));

    // The emitted line must parse as a frame, and its echoed detail must not have
    // opened a second JSON member.
    FrameHeader h{};
    EXPECT_TRUE(NdjsonParseEnvelope(cap.lines[0].c_str(), &h))
        << "an unescaped quote in `detail` would make the nack unparseable: " << cap.lines[0];
    EXPECT_EQ(h.type, std::string("nack"));
    // `for_seq` is the frame being answered; it must still name seq 9 (the
    // envelope's own seq is the router's outbound counter, a different number).
    EXPECT_NE(cap.lines[0].find("\"for_seq\":9"), std::string::npos) << cap.lines[0];
    EXPECT_EQ(cap.lines[0].find("\\\"gus"), std::string::npos)
        << "the quote must be neutralised, not carried through";

    // A pathologically long value must still yield a parseable, bounded frame --
    // the body is cut by snprintf otherwise, which is invalid JSON for the same
    // reason a raw quote is.
    std::string big = "{\"v\":1,\"seq\":10,\"type\":\"";
    big += std::string(600, 'X');
    big += "\"}";
    r.OnLine(big.c_str(), big.size());
    FrameHeader h2{};
    EXPECT_TRUE(NdjsonParseEnvelope(cap.lines.back().c_str(), &h2)) << cap.lines.back();
    EXPECT_LT(cap.lines.back().size(), kNdjsonMaxFrame);
}

TEST(CommandRouter, AMalformedLineIsReportedRatherThanDropped) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const char *bad = "{not json";
    r.OnLine(bad, std::strlen(bad));
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines[0].find("bad_frame"), std::string::npos);
}

TEST(CommandRouter, AProtocolVersionMismatchIsRefusedExplicitly) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string p = "{\"v\":99,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines[0].find("version"), std::string::npos)
        << "the app must learn the versions disagree, not silently misparse";
}

TEST(CommandRouter, AnInvalidConfigIsRejectedAndTheOldOneSurvives) {
    // FR-26: "an invalid config is rejected with a nack, leaving the previous
    // config intact". The assertion has to be about the PREVIOUS config, so this
    // stores a distinctive one first.
    //
    // An earlier version of this test accepted `kLoaded || kNoConfig` for the
    // post-state, which proves nothing: a rejected config that WIPED storage would
    // have satisfied it. A test whose passing branch includes "the data is gone"
    // cannot detect the loss of data.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    Config original = MockHalDefaultsConfig();
    original.settings.timings.long_press_ms = 900;   // distinctive, so survival is visible
    ASSERT_TRUE(store.Save(original)) << "the test needs a previous config to protect";

    // The config must DECODE and then FAIL VALIDATION, or this test exercises the
    // decode path instead -- which is what three earlier versions of it did. The
    // encoder refuses to emit an invalid config (ConfigEncodeBlob validates too),
    // so the invalid value has to be PATCHED into the wire bytes after encoding,
    // exactly as a buggy or hostile app could send it.
    //
    // `long_press_ms <= double_press_off_ms` is the right kind of invalid: it is a
    // CROSS-FIELD rule that only ConfigValidate applies, while the decoder reads
    // the number without complaint. A single-field violation (debounce_ms = 0,
    // say) is caught earlier by the decoder itself, so it never reaches validation.
    const std::string good = EncodeConfig(MockHalDefaultsConfig());
    const std::string needle = "\"long_press_ms\":750";
    const size_t pos = good.find(needle);
    ASSERT_NE(pos, std::string::npos) << "the fixture's long_press_ms must be findable: " << good;
    std::string invalid_json = good;
    invalid_json.replace(pos, needle.size(), "\"long_press_ms\":500");
    SendConfigChunked(r, /*seq=*/2, invalid_json);
    ASSERT_TRUE(HasType(cap, "nack"));
    // The ERROR CODE matters, and it is `decode`, not `invalid`. `ConfigDecodeJson`
    // validates the config it builds, so an invalid config is refused inside the
    // decode and never reaches a separate validation step -- three earlier versions
    // of this test asserted "invalid" and could not have passed for the right
    // reason. Asserting the code pins the ordering AND tells the app author which
    // message is real.
    EXPECT_NE(cap.lines.back().find("\"decode\""), std::string::npos) << cap.lines.back();
    EXPECT_EQ(cap.lines.back().find("save_failed"), std::string::npos)
        << "an invalid config must not be reported as a write failure -- that would "
           "send the user looking for an NVS problem instead of a bad field";

    ConfigStore s2(&hal.InterfaceRef());
    Config out{};
    ASSERT_EQ(s2.Load(&out), ConfigLoadResult::kLoaded)
        << "a refused config must not have destroyed the stored one";
    EXPECT_EQ(out.settings.timings.long_press_ms, 900u)
        << "and the stored config must be the ORIGINAL, not a default or a partial write";
}

TEST(CommandRouter, AValidConfigIsAckedAndPersisted) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string json = EncodeConfig(MockHalDefaultsConfig());
    SendConfigChunked(r, /*seq=*/3, json);
    ASSERT_TRUE(HasType(cap, "ack"));

    ConfigStore s2(&hal.InterfaceRef());
    Config out{};
    EXPECT_EQ(s2.Load(&out), ConfigLoadResult::kLoaded);
}

TEST(CommandRouter, ASetConfigOnlyTakesEffectAfterTheAck) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    Config c = MockHalDefaultsConfig();
    c.settings.timings.long_press_ms = 900;
    SendConfigChunked(r, /*seq=*/4, EncodeConfig(c));
    // The ACK must be the LAST thing said about the run.
    ASSERT_FALSE(cap.lines.empty());
    EXPECT_NE(cap.lines.back().find("\"type\":\"ack\""), std::string::npos);
}

TEST(CommandRouter, ACommittedConfigRunsImmediatelyThroughTheWire) {
    // The wiring half of spec 4.2's "committed means persisted AND running": the
    // handler must hand the config it just saved to the orchestrator.
    // `ApplyConfig` existing and being correct is worth nothing if the three write
    // paths never reach it, and nothing else in this suite would notice.
    //
    // The observable is the `long_press_ms` the RUNNING machine actually uses: the
    // router's own reply cannot show it, because the reply is about storage.
    // `vol_up` binds LONG in the default fixture, so a hold past the threshold
    // reports LONG -- and the threshold itself is what the push changes.
    //
    // The device boots on the DEFAULT 750, so 900 ms fires LONG there. The push
    // raises it to 1500, and THEN 900 ms must not. A router that only persisted
    // leaves the 750 in force and the post-push hold fires LONG anyway.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;                       // vol_up binds LONG at 750
    d.config.settings.timings.long_press_ms = 750;
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    std::vector<std::string> seen;
    sys.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            if (ev.gesture == Gesture::kLong) v->push_back("LONG");
        },
        &seen);
    auto hold_ms = [&](uint32_t ms) {
        seen.clear();
        hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
        for (uint32_t t = 0; t < ms; t += 10) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
        hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
        for (uint32_t t = 0; t < 400; t += 10) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    };
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    // Precondition: at the boot threshold, 900 ms IS a long press.
    hold_ms(900);
    ASSERT_FALSE(seen.empty()) << "the fixture must bind LONG, or this proves nothing";

    Config pushed = d.config;
    pushed.settings.timings.long_press_ms = 1500;
    ASSERT_TRUE(ConfigValidate(pushed));
    SendConfigChunked(r, /*seq=*/7, EncodeConfig(pushed));
    ASSERT_TRUE(HasType(cap, "ack")) << "the push must be accepted for this to mean anything";

    hold_ms(900);
    EXPECT_TRUE(seen.empty())
        << "LONG fired at 900 ms AFTER a push that raised long_press_ms to 1500: "
           "config_end persisted the config but did NOT apply it, so the running "
           "device still uses the boot threshold";

    hold_ms(1700);
    EXPECT_FALSE(seen.empty()) << "a hold past the pushed long_press_ms must still fire LONG";
}

TEST(CommandRouter, ARefusedConfigLeavesTheRunningConfigAlone) {
    // The other direction, and the one a careless "just call ApplyConfig" fix gets
    // wrong: a config the device REFUSED must change nothing -- not the stored copy
    // and not the running one. Applying before the save (or applying a config that
    // never validated) would make a rejected push take effect, which is precisely
    // the failure spec 4.2's "rejection leaves the previous config intact in both
    // places" names.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.settings.timings.long_press_ms = 750;
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    std::vector<std::string> seen;
    sys.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            if (ev.gesture == Gesture::kLong) v->push_back("LONG");
        },
        &seen);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    // A config that DECODES but fails validation -- patched into the encoded
    // bytes, because the encoder refuses to emit an invalid one. The patch RAISES
    // long_press_ms, so if the refusal leaked into the running state it would be
    // visible as a hold that stops firing LONG.
    const std::string good = EncodeConfig(MockHalDefaultsConfig());
    const std::string needle = "\"long_press_ms\":750";
    const size_t pos = good.find(needle);
    ASSERT_NE(pos, std::string::npos) << good;
    std::string invalid_json = good;
    invalid_json.replace(pos, needle.size(), "\"long_press_ms\":500");
    SendConfigChunked(r, /*seq=*/8, invalid_json);
    ASSERT_TRUE(HasType(cap, "nack")) << "precondition: the push must be refused";

    // The running config is untouched: the ORIGINAL 750 is still in force, so a
    // 900 ms hold still fires LONG.
    seen.clear();
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (uint32_t t = 0; t < 900; t += 10) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_NE(std::find(seen.begin(), seen.end(), "LONG"), seen.end())
        << "a REFUSED config reached the running device: the 750 threshold should "
           "still be in force, so a 900 ms hold must fire LONG";
}

TEST(CommandRouter, AConfigPatchAppliesImmediately) {
    // `config_patch` is the second write path and the one a browser-driven setup
    // uses, so it needs its own assertion rather than riding on config_end's.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.settings.timings.long_press_ms = 750;
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    std::vector<std::string> seen;
    sys.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            if (ev.gesture == Gesture::kLong) v->push_back("LONG");
        },
        &seen);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    auto hold_ms = [&](uint32_t ms) {
        seen.clear();
        hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
        for (uint32_t t = 0; t < ms; t += 10) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
        hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
        for (uint32_t t = 0; t < 400; t += 10) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    };

    hold_ms(900);
    ASSERT_FALSE(seen.empty()) << "precondition: 900 ms is long at the boot threshold";

    const std::string patch =
        "{\"v\":1,\"seq\":5,\"type\":\"config_patch\","
        "\"path\":\"settings.timings.long_press_ms\",\"value\":1500}";
    r.OnLine(patch.c_str(), patch.size());
    ASSERT_TRUE(HasType(cap, "ack")) << "the patch must be accepted for this to mean anything";

    hold_ms(900);
    EXPECT_TRUE(seen.empty())
        << "LONG fired at 900 ms after a patch that raised long_press_ms to 1500: "
           "config_patch persisted the change but did NOT apply it";

    hold_ms(1700);
    EXPECT_FALSE(seen.empty()) << "a hold past the patched long_press_ms must fire LONG";
}

TEST(CommandRouter, AConfigRunIsStagedAndNothingIsCommittedUntilTheEnd) {
    // Spec 4.2: "An interrupted run is discarded wholesale -- a partial config
    // is never applied."
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    Config c = MockHalDefaultsConfig();
    c.settings.timings.long_press_ms = 900;
    const std::string json = EncodeConfig(c);

    SendConfigBegin(r, /*seq=*/5, json.size(),
                    Crc32(reinterpret_cast<const uint8_t *>(json.data()), json.size()));
    SendConfigChunks(r, json.data(), json.size());
    // Deliberately do NOT send config_end. Nothing may have been committed.
    ConfigStore s2(&hal.InterfaceRef());
    Config out{};
    EXPECT_NE(s2.Load(&out), ConfigLoadResult::kLoaded)
        << "a run without its end must leave the previous config in place";
}

TEST(CommandRouter, AConfigRunWithABadCrcIsRejectedAtTheEnd) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string json = EncodeConfig(MockHalDefaultsConfig());
    SendConfigBegin(r, /*seq=*/6, json.size(), /*crc32=*/0xDEADBEEFu);   // wrong on purpose
    SendConfigChunks(r, json.data(), json.size());
    SendConfigEnd(r, /*seq=*/7, json.data(), json.size());   // hash is CORRECT
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("crc"), std::string::npos)
        << "the CRC is checked first, so a bad CRC must not be reported as a hash failure";
}

TEST(CommandRouter, AConfigRunWhoseSha256DisagreesIsRejectedEvenWithAGoodCrc) {
    // The two checks are not redundant and this is the test that proves it.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string json = EncodeConfig(MockHalDefaultsConfig());
    SendConfigBegin(r, /*seq=*/9, json.size(),
                    Crc32(reinterpret_cast<const uint8_t *>(json.data()), json.size()));
    SendConfigChunks(r, json.data(), json.size());
    // Correct CRC above, deliberately wrong digest here.
    const std::string bad_end =
        "{\"v\":1,\"seq\":10,\"type\":\"config_end\",\"sha256\":\""
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"}";
    r.OnLine(bad_end.c_str(), bad_end.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("sha256"), std::string::npos);
    ConfigStore s2(&hal.InterfaceRef());
    Config out{};
    EXPECT_NE(s2.Load(&out), ConfigLoadResult::kLoaded)
        << "a run that failed verification must not have been committed";
}

TEST(CommandRouter, AConfigBeginLargerThanTheStagingBufferIsRefusedUpFront) {
    // Without this the staging buffer is a heap-overflow primitive driven by the
    // peer: total_len comes from the frame, and the buffer is fixed.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    SendConfigBegin(r, /*seq=*/8, ConfigMaxSerializedSize() + 1, 0u);
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("too_large"), std::string::npos);
}

TEST(CommandRouter, AChunkWithAGapIsRejectedRatherThanConcatenated) {
    // A receiver that ignores `offset` silently splices two runs into a config
    // that passes its own CRC -- over the wrong bytes.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string json = EncodeConfig(MockHalDefaultsConfig());
    ASSERT_GT(json.size(), kConfigWireChunkBytes);
    SendConfigBegin(r, /*seq=*/11, json.size(),
                    Crc32(reinterpret_cast<const uint8_t *>(json.data()), json.size()));
    // Skip the first chunk entirely: the run now starts at a nonzero offset.
    const std::string b64 = B64(std::string(json.data() + kConfigWireChunkBytes, kConfigWireChunkBytes));
    const std::string line = "{\"v\":1,\"seq\":12,\"type\":\"config_chunk\",\"offset\":" +
        std::to_string(kConfigWireChunkBytes) + ",\"data_b64\":\"" + b64 + "\"}";
    r.OnLine(line.c_str(), line.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("gap"), std::string::npos);
}

TEST(CommandRouter, ASequenceGapIsReportedAsAnEvent) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string p1 = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p1.c_str(), p1.size());
    cap.lines.clear();
    const std::string p5 = "{\"v\":1,\"seq\":5,\"type\":\"ping\"}";
    r.OnLine(p5.c_str(), p5.size());   // 2-4 missing
    EXPECT_TRUE(HasType(cap, "link_gap"))
        << "a dropped frame must be surfaced, not silently tolerated";
}

// --- the four spec 4.3 commands the router previously did not handle --------

TEST(CommandRouter, ATestKeyDrivesTheOutputAndIsAcked) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    const std::string tk =
        "{\"v\":1,\"seq\":1,\"type\":\"test_key\",\"channel\":0,\"key_mv\":2400,\"hold_ms\":200}";
    r.OnLine(tk.c_str(), tk.size());
    ASSERT_TRUE(HasType(cap, "ack"));
    // And it actually drove the KEY line, rather than only acking.
    EXPECT_TRUE(hal.LastDacCode(DAC_CH_KEY1) != 0);
}

TEST(CommandRouter, ATestKeyOutsideTheEnvelopeIsRefusedRatherThanClamped) {
    // A bench command that silently clamped would measure the clamp, not the
    // servo -- and would hide a missing clamp entirely.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    const std::string tk =
        "{\"v\":1,\"seq\":1,\"type\":\"test_key\",\"channel\":0,\"key_mv\":9000,\"hold_ms\":200}";
    r.OnLine(tk.c_str(), tk.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("out_of_range"), std::string::npos);
}

TEST(CommandRouter, IdentifyIsAcceptedAndAnUnknownPatternIsRefused) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    const std::string i1 = "{\"v\":1,\"seq\":1,\"type\":\"identify\",\"pattern\":\"flash\"}";
    r.OnLine(i1.c_str(), i1.size());
    EXPECT_TRUE(HasType(cap, "ack"));
    cap.lines.clear();
    const std::string i2 = "{\"v\":1,\"seq\":2,\"type\":\"identify\",\"pattern\":\"somersault\"}";
    r.OnLine(i2.c_str(), i2.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("unknown_pattern"), std::string::npos);
}

TEST(CommandRouter, ARebootIsAckedBeforeTheReset) {
    // The ack is EMITTED before `reboot()`, so a device that is reset by the call
    // still queued it first. This test's `Capture` sink records at emit time, so
    // it does NOT prove the ack reaches the wire -- `LinkWiringTest`'s
    // `TheRebootAckReachesTheTransportBeforeTheReset` does, through the real
    // transport with no ServiceTx (the flush is what makes the ordering real).
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string rb = "{\"v\":1,\"seq\":1,\"type\":\"reboot\",\"boot_target\":\"app\"}";
    r.OnLine(rb.c_str(), rb.size());
    ASSERT_TRUE(HasType(cap, "ack"));
    EXPECT_EQ(hal.RebootCount(), 1);
}

TEST(CommandRouter, ARebootWithABadTargetIsRefusedAndDoesNotReboot) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string rb =
        "{\"v\":1,\"seq\":1,\"type\":\"reboot\",\"boot_target\":\"toaster\"}";
    r.OnLine(rb.c_str(), rb.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_EQ(hal.RebootCount(), 0);
}

TEST(CommandRouter, TimeSyncIsAckedRatherThanNacked) {
    // The firmware has no wall clock to set. Refusing would tell the app the
    // link is broken; the frame WAS understood, so it is acked.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string ts =
        "{\"v\":1,\"seq\":1,\"type\":\"time_sync\",\"epoch_ms\":1700000000000,\"tz_offset_min\":-420}";
    r.OnLine(ts.c_str(), ts.size());
    EXPECT_TRUE(HasType(cap, "ack"));
    EXPECT_FALSE(HasType(cap, "nack"));
}

TEST(CommandRouter, AConfigPatchChangesOneFieldAndPersistsIt) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    store.Save(MockHalDefaultsConfig());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string patch =
        "{\"v\":1,\"seq\":1,\"type\":\"config_patch\",\"path\":"
        "\"settings.timings.long_press_ms\",\"value\":900}";
    r.OnLine(patch.c_str(), patch.size());
    ASSERT_TRUE(HasType(cap, "ack"));

    ConfigStore s2(&hal.InterfaceRef());
    Config out{};
    ASSERT_EQ(s2.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_EQ(out.settings.timings.long_press_ms, 900u);
}

TEST(CommandRouter, AConfigPatchToAnUnknownPathIsRefusedAndChangesNothing) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    store.Save(MockHalDefaultsConfig());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string patch =
        "{\"v\":1,\"seq\":1,\"type\":\"config_patch\",\"path\":"
        "\"settings.not_a_field\",\"value\":1}";
    r.OnLine(patch.c_str(), patch.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("unknown_path"), std::string::npos);
}

TEST(CommandRouter, AKnownCommandIsDispatchedAndAnUnknownOneIsRefused) {
    // Every command in the spec's vocabulary now has a handler, so a
    // "known-but-unimplemented" case no longer exists -- `ota_*` WAS the last one
    // (N-14) and is now wired to `OtaUsb`. What still matters is the distinction
    // this test exists for: a KNOWN type reaches its handler (and a malformed one
    // is refused for a FIELD reason, proving the handler ran), while an UNKNOWN
    // type is `unknown_type` and never dispatched.
    //
    // `learn_start` USED to be in the unimplemented set and is now implemented
    // (FR-5), so it is asserted separately below.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    // A bare `ota_begin` names neither `size` nor `sha256`, so the handler refuses
    // it `bad_frame`. That the refusal is `bad_frame` rather than
    // `not_implemented` is the assertion: the handler executed.
    const std::string ota = "{\"v\":1,\"seq\":1,\"type\":\"ota_begin\"}";
    r.OnLine(ota.c_str(), ota.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("bad_frame"), std::string::npos)
        << "ota_begin is implemented now, so a fieldless one is a bad_frame";
    EXPECT_EQ(cap.lines.back().find("not_implemented"), std::string::npos);
    EXPECT_EQ(cap.lines.back().find("unknown_type"), std::string::npos);

    // An unknown type is still refused as unknown, and never dispatched.
    cap.lines.clear();
    const std::string bogus = "{\"v\":1,\"seq\":2,\"type\":\"definitely_not_a_frame\"}";
    r.OnLine(bogus.c_str(), bogus.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("unknown_type"), std::string::npos);
}

TEST(CommandRouter, LearnStartOpensTheLiveStreamAndIsNotUnimplemented) {
    // FR-5: during learn, the filtered level must reach the link so the app can
    // render it live. The stream is opened by learn_start and drained by
    // Process() one sample per call.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string ls = "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0}";
    r.OnLine(ls.c_str(), ls.size());
    ASSERT_TRUE(HasType(cap, "ack")) << "learn_start must be implemented, not nacked";
    EXPECT_EQ(cap.lines.back().find("not_implemented"), std::string::npos);

    cap.lines.clear();
    r.Process();
    ASSERT_TRUE(HasType(cap, "ladder_sample"))
        << "an open learn run must stream the level (FR-5)";
    EXPECT_NE(cap.lines.back().find("\"channel\":0"), std::string::npos);

    // learn_stop closes it; Process must then be silent.
    const std::string lstop = "{\"v\":1,\"seq\":2,\"type\":\"learn_stop\",\"channel\":0}";
    r.OnLine(lstop.c_str(), lstop.size());
    cap.lines.clear();
    r.Process();
    EXPECT_FALSE(HasType(cap, "ladder_sample"))
        << "a closed learn run must stop streaming";
}

TEST(CommandRouter, ASecondConfigBeginWhileOneIsOpenIsRefused) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    SendConfigBegin(r, 1, 100, 0u);
    cap.lines.clear();
    SendConfigBegin(r, 2, 100, 0u);
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("run_open"), std::string::npos)
        << "two interleaved runs would splice into a config that passes its own CRC";
}

TEST(CommandRouter, DisconnectDiscardsAHalfReceivedRun) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string json = EncodeConfig(MockHalDefaultsConfig());
    SendConfigBegin(r, 1, json.size(),
                    Crc32(reinterpret_cast<const uint8_t *>(json.data()), json.size()));
    SendConfigChunks(r, json.data(), json.size());
    r.OnDisconnected();
    // The end arrives after the link came back; the run it refers to is gone.
    cap.lines.clear();
    SendConfigEnd(r, 2, json.data(), json.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("no_run"), std::string::npos);
}

// --- EmitGesture: the `event` frame's rendering (spec 4.3, FR-12) -----------
//
// These exist because the null case had NO test at all, and the code crashed on
// it: the id was copied into a local buffer with `ev.button_id[n]`, which is a
// null dereference the moment FR-12 reports an unrecognised press. Nothing in
// the suite drove `EmitGesture`, so a device that segfaulted on the first
// unlearned press would have passed every test here.

TEST(CommandRouter, AGestureRendersTheLearnedButtonIdAsAQuotedString) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    SystemOrchestrator::GestureEventRecord ev{};
    ev.channel_index = 0;
    ev.button_id = "vol_up";
    ev.gesture = Gesture::kSingle;
    ev.level_mv = 1430;
    ev.idle_mv = 2835;
    ev.at_ms = 1234;
    r.EmitGesture(ev);
    ASSERT_EQ(cap.lines.size(), 1u);
    const std::string &l = cap.lines[0];
    EXPECT_NE(l.find("\"type\":\"event\""), std::string::npos);
    EXPECT_NE(l.find("\"button\":\"vol_up\""), std::string::npos)
        << "the app matches this against Binding.button, so it must be the quoted id";
    EXPECT_NE(l.find("\"gesture\":\"SINGLE\""), std::string::npos);
    EXPECT_NE(l.find("\"level_mv\":1430"), std::string::npos);
    EXPECT_NE(l.find("\"idle_mv\":2835"), std::string::npos)
        << "the live idle is the denominator the device classified against; the app "
           "needs it to reproduce the decision as a ratio (N-25)";
    EXPECT_LT(l.size(), kNdjsonMaxFrame);
}

TEST(CommandRouter, TheEventCarriesTheLiveIdleTheClassificationUsed) {
    // Open item N-25's wire half. `LadderClassify` normalizes the reading against
    // the LIVE idle and each window against `learned_idle_mv`; the app can only
    // reproduce that ratio if the frame carries the denominator the device used.
    // The value must be the reading's own denominator, not a constant: a frame
    // that reported a fixed 3300 would move the app's match by the same error the
    // ratio exists to cancel.
    //
    // Driven through the whole chain (Boot -> classify -> gesture -> EmitGesture)
    // rather than a hand-built record, so the test covers the wiring that fills the
    // field -- the system suite owns that path and has the poll helpers; here the
    // router is handed the record it would have received.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    store.Save(d.config);
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    const int live_idle = sys.IdleReferenceMv(0);
    ASSERT_GT(live_idle, 0) << "fixture: Boot must have seeded a live idle";

    // The fixture's `vol_up` centre, classified against the live idle.
    SystemOrchestrator::GestureEventRecord ev{};
    ev.channel_index = 0;
    ev.button_id = "vol_up";
    ev.gesture = Gesture::kSingle;
    ev.level_mv = 1430;
    ev.idle_mv = live_idle;
    ev.at_ms = 1;
    r.EmitGesture(ev);

    ASSERT_TRUE(HasType(cap, "event"));
    const std::string &l = cap.lines.back();
    char want[32];
    std::snprintf(want, sizeof(want), "\"idle_mv\":%d", live_idle);
    EXPECT_NE(l.find(want), std::string::npos)
        << "the event must carry the idle the classifier used, so the app's ratio is "
           "a copy of the device's decision; want " << want << ", got: " << l;
}

TEST(CommandRouter, AnUnrecognizedPressRendersANullButtonAndNotAQuotePair) {
    // FR-12: `event{button: null}`. JSON null, not `""` -- an empty string is a
    // button named "", which the app would render as a real (if blank) button,
    // and a named id would be the guess FR-12 forbids.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    SystemOrchestrator::GestureEventRecord ev{};
    ev.channel_index = 0;
    ev.button_id = nullptr;
    ev.gesture = Gesture::kNone;
    ev.level_mv = 2400;
    ev.idle_mv = 2835;
    ev.at_ms = 99;
    r.EmitGesture(ev);
    ASSERT_EQ(cap.lines.size(), 1u)
        << "an unrecognised press is reported; suppressing it hides the press from the app";
    const std::string &l = cap.lines[0];
    EXPECT_NE(l.find("\"button\":null"), std::string::npos) << l;
    EXPECT_EQ(l.find("\"button\":\"\""), std::string::npos)
        << "an empty string would render as a button with a blank name";
    EXPECT_NE(l.find("\"gesture\":\"NONE\""), std::string::npos) << l;
    EXPECT_NE(l.find("\"level_mv\":2400"), std::string::npos)
        << "the level is the whole diagnostic value of the frame";
}

TEST(CommandRouter, AHostileButtonIdCannotBreakTheJsonOrOverrunTheBuffer) {
    // The id reaches both a JSON string and a `%s`. A quote would terminate the
    // string early and a backslash would escape the closing quote, so both are
    // replaced; the length is bounded by kLadderIdLen whatever the config holds.
    //
    // The calibration frame is what makes this a real check: a quote count is only
    // meaningful against a frame known to be well formed, so the same record is
    // emitted twice and the counts compared. A hostile quote leaking through adds
    // exactly two quotes, which the comparison catches without hardcoding the
    // envelope's shape.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    SystemOrchestrator::GestureEventRecord good{};
    good.button_id = "vol_up";
    good.gesture = Gesture::kLong;
    r.EmitGesture(good);

    const std::string hostile = std::string("\"\\").append(200, 'a');
    SystemOrchestrator::GestureEventRecord bad{};
    bad.button_id = hostile.c_str();
    bad.gesture = Gesture::kLong;
    r.EmitGesture(bad);

    ASSERT_EQ(cap.lines.size(), 2u);
    const std::string &cal = cap.lines[0];
    const std::string &l = cap.lines[1];

    EXPECT_NE(l.find("\"button\":\"__aaa"), std::string::npos)
        << "the quote and backslash must both become '_', keeping the JSON parseable";
    EXPECT_LT(l.size(), kNdjsonMaxFrame) << "a 200-byte id must be truncated, not emitted";

    auto quotes = [](const std::string &s) {
        size_t n = 0;
        for (char c : s) if (c == '"') ++n;
        return n;
    };
    EXPECT_EQ(quotes(l), quotes(cal))
        << "a leaked hostile quote would add a pair of quotes:" << l;
    EXPECT_EQ(l.find("\\"), std::string::npos)
        << "a raw backslash would escape the closing quote and desync the parser";
    EXPECT_EQ(l.find("button\":null"), std::string::npos)
        << "a non-null id must never be reported as an unrecognised press";
}

// --- the `log` frame (spec 4.3) ---------------------------------------------
//
// The frame type was in the contract from the start and nothing ever emitted one,
// so FR-18's "clamp with a logged warning" had a specified destination and no
// writer. These assert the rendering, and the escaping that keeps a message from
// breaking the JSON.

TEST(CommandRouter, ALogFrameCarriesALevelAndAMessage) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.EmitLog("WARN", "key_mv 9000 clamped");
    ASSERT_EQ(cap.lines.size(), 1u);
    const std::string &l = cap.lines[0];
    EXPECT_NE(l.find("\"type\":\"log\""), std::string::npos);
    EXPECT_NE(l.find("\"level\":\"WARN\""), std::string::npos);
    EXPECT_NE(l.find("\"msg\":\"key_mv 9000 clamped\""), std::string::npos);
    EXPECT_LT(l.size(), kNdjsonMaxFrame);
}

TEST(CommandRouter, ALogMessageCannotBreakTheJson) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.EmitLog("WARN", "a \"quoted\" and \\escaped message");
    ASSERT_EQ(cap.lines.size(), 1u);
    // BY VALUE, not a reference: the second EmitLog below pushes onto
    // `cap.lines`, and a vector reallocation would leave a reference dangling.
    // (It did -- the first version of this test compared quote counts through a
    // dangling reference and read zero.)
    const std::string hostile_line = cap.lines[0];
    EXPECT_EQ(hostile_line.find("\\\""), std::string::npos) << hostile_line;
    // Exactly the envelope's own quote pairs, as in the hostile-button test: a
    // leaked quote would add a pair, which the comparison catches without
    // hardcoding the frame's shape.
    auto quotes = [](const std::string &s) {
        size_t n = 0;
        for (char c : s) if (c == '"') ++n;
        return n;
    };
    r.EmitLog("WARN", "plain");
    ASSERT_EQ(cap.lines.size(), 2u);
    EXPECT_EQ(quotes(hostile_line), quotes(cap.lines[1]))
        << "a leaked quote would add a pair:" << hostile_line;
}

TEST(CommandRouter, AnOversizedLogMessageIsTruncatedNotOverrun) {
    // The message is bounded so it can never overflow the body buffer, whatever a
    // caller passes. A truncated log line is worth more than a corrupt frame.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string huge(2000, 'x');
    r.EmitLog("WARN", huge.c_str());
    ASSERT_EQ(cap.lines.size(), 1u);
    EXPECT_LT(cap.lines[0].size(), kNdjsonMaxFrame);
    EXPECT_NE(cap.lines[0].find("\"type\":\"log\""), std::string::npos);
}

TEST(CommandRouter, ReLearningAButtonByIdReplacesItRatherThanDuplicating) {
    // The app names the button it learns (`button_id`), and it sends the SAME id
    // again when the user re-measures one. The handler always appended, so
    // re-learning put two buttons on the ladder with ONE id. Nothing rejects that
    // -- `ConfigValidate` does not check id uniqueness -- and `BindingResolve`
    // finds a binding by `strcmp` on the id, so the id became ambiguous between
    // two different voltages.
    //
    // This is the app-path twin of the headless wizard's version of the same bug;
    // both are fixed by replacing in place.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.channels[0].ladder.count = 0;
    d.config.channels[0].ladder.learned_idle_mv = 2835;
    // The fixture's bindings name buttons the emptied ladder no longer has, and
    // `ConfigValidate` refuses a binding that names no real input. Left in, they
    // make the config UNLOADABLE -- so `store.Load` fell back to defaults, and the
    // first version of this test passed by measuring that fallback rather than the
    // re-learn (`ConfigValidate` accepts defaults, so a commit over them looked
    // like a success). Clearing them is what makes the store round-trip at all.
    d.config.binding_count = 0;
    ASSERT_TRUE(store.Save(d.config));

    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    // The two levels are deliberately FAR APART. With nearby levels the
    // duplicate's windows overlap, the config becomes unvalidatable, and the next
    // `store.Load` falls back to DEFAULTS -- which resets `count` to 0 and hides
    // the duplicate. That is how the first version of this test passed against a
    // mutated build: it measured the fallback, not the bug. Well-separated levels
    // keep the duplicate valid, so it persists and is visible.
    const int levels[2] = {1430, 2500};
    for (int pass = 0; pass < 2; ++pass) {
        // TICK the orchestrator so its FR-3 filter warms: `FilteredLevelMv` returns
        // 0 for a stale reading, and a commit of 0 is now correctly REFUSED (a
        // centre of 0 is not a measurement), so without the ticks nothing would be
        // learned at all and this test would be measuring the wrong thing.
        hal.SetAdcMilliVolts(ADC_CH_SWC1, levels[pass]);
        for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }

        const std::string ls =
            "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0,\"button_id\":\"vol_dn\"}";
        r.OnLine(ls.c_str(), ls.size());
        // The REAL flow (spec 4.3): learn_start opens the stream, `Process()` ticks
        // emit ladder_sample AND record each one, and ONE learn_commit accepts the
        // streamed samples. Firing learn_commit repeatedly was how this test used
        // to reach the sample count, which is not the flow the spec describes and
        // is what masked the bug that the stream was never recorded.
        for (int i = 0; i < 20; ++i) {
            r.Process();
            hal.AdvanceMs(20);
        }
        const std::string lc =
            "{\"v\":1,\"seq\":3,\"type\":\"learn_commit\",\"channel\":0,"
            "\"button_id\":\"vol_dn\",\"name\":\"Volume Down\"}";
        r.OnLine(lc.c_str(), lc.size());
    }

    Config out{};
    EXPECT_EQ(store.Load(&out), ConfigLoadResult::kLoaded)
        << "the config must still be loadable -- a duplicate id is accepted by "
           "ConfigValidate, so a fallback here would mean the test measured the "
           "fallback rather than the duplicate";
    const LadderProfile &lp = out.channels[0].ladder;
    EXPECT_EQ(lp.count, 1)
        << "re-learning one id must CORRECT that button, not add a second entry";
    if (lp.count >= 2) {
        // Name the actual defect when it fires, so the failure is self-explaining.
        ADD_FAILURE() << "duplicate id: two buttons share \"" << lp.buttons[1].id
                      << "\" at " << static_cast<int>(lp.buttons[0].mv_center) << " and "
                      << static_cast<int>(lp.buttons[1].mv_center) << " mV";
    }
    EXPECT_STREQ(lp.buttons[0].id, "vol_dn");
    EXPECT_EQ(lp.buttons[0].mv_center, 2500)
        << "and the LATER measurement is the one that survives";
}

TEST(CommandRouter, ALearnCommitOverAnUnreadableConfigRefusesRatherThanOverwriting) {
    // The handler did `if (Load(&c) != kLoaded) c = ConfigDefault()`, which cannot
    // tell "never configured" from "configured but UNREADABLE". In the second case
    // the save overwrites the user's ENTIRE config -- bindings, the other channel,
    // every setting -- with defaults. Measured before the fix: three bindings
    // replaced by zero, from one `learn_commit`, with an `ack` returned.
    //
    // An unreadable stored config is a fault to report, not a blank sheet.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.channels[0].ladder.count = 0;
    d.config.channels[0].ladder.learned_idle_mv = 2835;
    d.config.binding_count = 0;
    ASSERT_TRUE(store.Save(d.config));

    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    // Rot the stored slots, so the config is present but cannot be decoded.
    hal.CorruptNvsValue("cfg_a_0", 24);
    hal.CorruptNvsValue("cfg_b_0", 24);
    {
        Config t{};
        ASSERT_EQ(store.Load(&t), ConfigLoadResult::kFellBackToDefaults)
            << "the fixture must actually be unreadable for this test to mean "
               "anything";
    }

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    const std::string ls =
        "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0,\"button_id\":\"vol_dn\"}";
    r.OnLine(ls.c_str(), ls.size());
    for (int i = 0; i < 20; ++i) {
        r.Process();
        hal.AdvanceMs(20);
    }
    cap.lines.clear();
    const std::string lc =
        "{\"v\":1,\"seq\":3,\"type\":\"learn_commit\",\"channel\":0,"
        "\"button_id\":\"vol_dn\",\"name\":\"Volume Down\"}";
    r.OnLine(lc.c_str(), lc.size());

    // The refusal is the point, and it must be REPORTED rather than silent.
    ASSERT_TRUE(HasType(cap, "nack"))
        << "an unreadable config must be refused, not overwritten";
    EXPECT_NE(cap.lines.back().find("config_unreadable"), std::string::npos);

    // And the store is untouched: still unreadable, NOT replaced by defaults.
    Config after{};
    EXPECT_EQ(store.Load(&after), ConfigLoadResult::kFellBackToDefaults)
        << "a refused commit must leave the stored bytes alone -- a load that now "
           "SUCCEEDS means defaults were written over the user's config";
}

// --- a config recovered from the backup slot is a REAL config ----------------

/*
 * Build a store whose newest slot is torn so `Load` reports
 * `kRecoveredFromBackup`, and hand back the config it recovers to.
 *
 * The device that did this is RUNNING that config: `Boot` accepts both
 * `kLoaded` and `kRecoveredFromBackup`, and spec 6.8's whole point is that the
 * other slot becomes authoritative after a tear. So anything that answers "what
 * config does the device have" must agree with `Boot` and not answer
 * `== kLoaded`.
 */
namespace {
Config MakeAStoreThatRecoveredFromItsBackupSlot(MockHal &hal, ConfigStore &store) {
    Config live = MockHalDefaultsConfig();
    // A distinctive field, so "the recovered config" is distinguishable from
    // anything the default path could synthesize. ONLY the device id changes:
    // the fixture must stay VALID, because `ConfigDecodeJson` runs
    // `ConfigValidate` -- so e.g. trimming `ladder.count` while the fixture's
    // bindings still name the dropped button would make the RECOVERED slot fail
    // to decode and `Load` would fall through to defaults, i.e. the test would
    // pass for the wrong reason.
    std::strncpy(live.device_id, "SWC-RECOVERED", sizeof(live.device_id) - 1);
    // `ASSERT_*` cannot be used from a value-returning helper (gtest requires a
    // void return), so these are EXPECTs; the caller's later assertion on
    // `kRecoveredFromBackup` is what proves the fixture is what it claims.
    EXPECT_TRUE(store.Save(live));       // slot A, seq 1: the copy that survives

    Config newer = MockHalDefaultsConfig();
    std::strncpy(newer.device_id, "SWC-NEWER", sizeof(newer.device_id) - 1);
    EXPECT_TRUE(store.Save(newer));      // slot B, seq 2: the newest slot

    // `kRecoveredFromBackup` is ROT in the newest slot, not a torn write: the
    // store writes the payload before the sequence, so a tear leaves the OLDER
    // slot as "newest" and reads as `kLoaded`. Reaching the recovery path needs
    // the sequence to have advanced (it has -- slot B is newest) and slot B's own
    // bytes to be unreadable. Rot its header chunk; the store then falls back to
    // slot A, which is spec 6.8's whole point.
    hal.CorruptNvsValue("cfg_b_0", 3);

    Config check{};
    EXPECT_EQ(store.Load(&check), ConfigLoadResult::kRecoveredFromBackup)
        << "the fixture must actually be recovered-from-backup for this to mean anything";
    return live;
}
}  // namespace

TEST(CommandRouter, AConfigGetReportsARecoveredBackupRatherThanDefaults) {
    // The device is running the config it recovered from the backup slot, so
    // `config_get` must report THAT -- the reply is the app's whole picture of
    // the device. Reporting defaults instead draws an empty grid over a
    // configured device, and the app's next save then overwrites the recovered
    // config with those defaults, losing the user's bindings and both ladders.
    // Measured before the fix: the reply carried the default device id and an
    // empty ladder while `Boot` was running the recovered one.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    const Config live = MakeAStoreThatRecoveredFromItsBackupSlot(hal, store);

    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string get = "{\"v\":1,\"seq\":1,\"type\":\"config_get\"}";
    r.OnLine(get.c_str(), get.size());
    DrainReplies(r);

    // Reassemble the chunked reply and decode it, exactly as the app does.
    uint8_t raw[65536] = {};
    for (const auto &l : cap.lines) {
        const size_t p = l.find("\"data_b64\":\"");
        if (p == std::string::npos) continue;
        const size_t start = p + 12;
        const size_t end = l.find('"', start);
        const std::string b64 = l.substr(start, end - start);
        const size_t op = l.find("\"offset\":");
        ASSERT_NE(op, std::string::npos) << l;
        const size_t off = static_cast<size_t>(std::stoul(l.substr(op + 9)));
        uint8_t decoded[kConfigWireChunkBytes];
        size_t dn = 0;
        ASSERT_TRUE(Base64Decode(b64.c_str(), b64.size(), decoded, sizeof(decoded), &dn)) << l;
        ASSERT_LE(off + dn, sizeof(raw));
        memcpy(raw + off, decoded, dn);
    }
    Config out{};
    ASSERT_TRUE(ConfigDecodeJson(reinterpret_cast<const char *>(raw), sizeof(raw), &out))
        << "the reply must be a decodable config";

    EXPECT_STREQ(out.device_id, live.device_id)
        << "the reply must carry the RECOVERED config, not synthesized defaults";
    EXPECT_EQ(out.channels[0].ladder.count, live.channels[0].ladder.count)
        << "a recovered ladder must reach the app, or the grid is empty over a "
           "configured device";
}

TEST(CommandRouter, ALearnStartOnARecoveredDeviceSeedsTheRecoveredLadder) {
    // The neighbour set a learn checks against comes from the same load. Reading
    // only `kLoaded` left it EMPTY on a recovered device, so a re-measure could
    // land on top of a button that is really there and the classifier could no
    // longer tell the two windows apart.
    //
    // The observable is the REJECTION. The recovered ladder carries `vol_up` at
    // 1430 mV. A learn of `vol_dn` measured at `vol_up`'s own centre must be
    // refused as `too_close_to_existing` when the recovered ladder seeds the
    // session; with an empty seed it commits a second button on the same level.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MakeAStoreThatRecoveredFromItsBackupSlot(hal, store);

    MockHal::Defaults d;
    d.config = MockHalDefaultsConfig();
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    // At the recovered ladder's `vol_up` centre, i.e. inside a real neighbour's
    // window -- which is the ONE thing the seed exists to catch.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }

    const std::string ls =
        "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0,\"button_id\":\"vol_dn\"}";
    r.OnLine(ls.c_str(), ls.size());
    ASSERT_TRUE(HasType(cap, "ack")) << "learn_start must open the stream";

    for (int i = 0; i < 30; ++i) { r.Process(); hal.AdvanceMs(10); }

    cap.lines.clear();
    const std::string lc =
        "{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
        "\"button_id\":\"vol_dn\",\"name\":\"Volume Down\"}";
    r.OnLine(lc.c_str(), lc.size());
    ASSERT_TRUE(HasType(cap, "nack"))
        << "a measurement inside a recovered button's window must be refused; an "
           "`ack` here means the recovered ladder was not seeded as neighbours";
    EXPECT_NE(cap.lines.back().find("too_close_to_existing"), std::string::npos)
        << "the reason must name the neighbour, got: " << cap.lines.back();
}

// --- link liveness (spec 4.4) ------------------------------------------------

TEST(CommandRouter, ALearnCommitRefusesAnEmptyOrOversizedIdOrName) {
    // `Str` returns the item for `""` -- a valid JSON string with a non-null
    // `valuestring` -- so a present-but-EMPTY id or name passed the
    // "is it there?" guard, was copied into the stored button, and persisted
    // (`LearnSession::Commit` and `ConfigStore::Save` both accept it). The next
    // boot's `ConfigDecodeJson` refuses the empty string, so `Load` returns
    // `kFellBackToDefaults` and the user loses EVERY binding, both channels and
    // all settings, reported only as a corrupt config.
    //
    // The oversized case is the same hazard by a different route: the copy is an
    // `snprintf` into a `char[kLadderIdLen]`/`kLadderNameLen`, so an over-long
    // value would be SILENTLY TRUNCATED -- a stored button whose id no longer
    // matches the binding the user wrote.
    //
    // Both must be REFUSED before the write, and the stored config must be left
    // untouched.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.channels[0].ladder.count = 0;
    d.config.channels[0].ladder.learned_idle_mv = 2835;
    // The bindings go too: they name ladder buttons, so a ladder with `count = 0`
    // leaves them pointing at inputs nothing holds -- `ConfigValidate` refuses
    // that, `Save` therefore refuses, and the fixture would be an EMPTY store
    // rather than the readable one this test needs.
    d.config.binding_count = 0;
    ASSERT_TRUE(store.Save(d.config));

    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }

    auto run_a_learn = [&](const std::string &commit) {
        cap.lines.clear();
        const std::string ls =
            "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0,\"button_id\":\"vol_dn\"}";
        r.OnLine(ls.c_str(), ls.size());
        for (int i = 0; i < 30; ++i) { r.Process(); hal.AdvanceMs(10); }
        cap.lines.clear();
        r.OnLine(commit.c_str(), commit.size());
    };

    // An EMPTY id.
    run_a_learn("{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
                "\"button_id\":\"\",\"name\":\"Volume Down\"}");
    ASSERT_TRUE(HasType(cap, "nack")) << "an empty button_id must be refused";
    EXPECT_NE(cap.lines.back().find("bad_param"), std::string::npos)
        << "got: " << cap.lines.back();

    // An EMPTY name.
    run_a_learn("{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
                "\"button_id\":\"vol_dn\",\"name\":\"\"}");
    ASSERT_TRUE(HasType(cap, "nack")) << "an empty name must be refused";
    EXPECT_NE(cap.lines.back().find("bad_param"), std::string::npos)
        << "got: " << cap.lines.back();

    // A button_id at exactly the LADDER id width (16) -- refused, not truncated.
    // The id is 16 chars, so `snprintf` into `char[16]` would keep 15 and NUL.
    run_a_learn("{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
                "\"button_id\":\"0123456789abcdef\",\"name\":\"Volume Down\"}");
    ASSERT_TRUE(HasType(cap, "nack"))
        << "a button_id at the width must be refused rather than silently truncated";
    EXPECT_NE(cap.lines.back().find("bad_param"), std::string::npos)
        << "got: " << cap.lines.back();

    // A name at the LADDER name width (16).
    run_a_learn("{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
                "\"button_id\":\"vol_dn\",\"name\":\"0123456789abcdef\"}");
    ASSERT_TRUE(HasType(cap, "nack"))
        << "a name at the width must be refused rather than silently truncated";
    EXPECT_NE(cap.lines.back().find("bad_param"), std::string::npos)
        << "got: " << cap.lines.back();

    // Nothing was written by any of the four: the stored ladder is still empty.
    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_EQ(out.channels[0].ladder.count, 0u)
        << "a refused learn_commit must not have persisted a button";

    // And a legitimate id ONE below the width DOES commit -- so the guard bounds
    // the value rather than refusing the field outright.
    run_a_learn("{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
                "\"button_id\":\"0123456789abcde\",\"name\":\"Volume Down\"}");
    ASSERT_TRUE(HasType(cap, "ack")) << "a 15-character id must be accepted";
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    ASSERT_EQ(out.channels[0].ladder.count, 1u);
    EXPECT_STREQ(out.channels[0].ladder.buttons[0].id, "0123456789abcde");
}

TEST(CommandRouter, AnAbandonedConfigRunIsReapedAfterTenSecondsOfSilence) {
    // Spec 4.4: "After 10 s of silence the firmware considers the link down."
    // The app that dies mid-`config_set` -- or a USB glitch that drops the tail --
    // must not leave the device refusing every later config. Measured before the
    // fix: a config_begin with no config_end, 60 s of silence, then a legitimate
    // config_begin came back `nack run_open`.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    // A frame arms the liveness clock (a peer exists and has spoken).
    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    SendConfigBegin(r, 2, 10, 0);
    cap.lines.clear();

    // Still inside the bound: the run is intact and a second begin is refused.
    hal.AdvanceMs(9999);
    r.Tick();
    SendConfigBegin(r, 3, 10, 0);
    ASSERT_TRUE(HasType(cap, "nack")) << "inside 10 s the run is still open";
    EXPECT_NE(cap.lines.back().find("run_open"), std::string::npos);
    cap.lines.clear();

    // Past the bound since the LAST inbound frame (the seq-3 probe refreshed it):
    // the abandoned run is gone and a fresh begin is accepted.
    hal.AdvanceMs(10001);                 // 10001 ms since that frame
    r.Tick();
    SendConfigBegin(r, 4, 10, 0);
    ASSERT_TRUE(HasType(cap, "ack")) << "silence must reap the abandoned run";
    EXPECT_FALSE(HasType(cap, "nack"));
}

TEST(CommandRouter, SilenceStopsThePeriodicStatusUntilAFrameReArmsIt) {
    // The other half of spec 4.4's link-down state: with the peer gone, a status
    // is written into a FIFO nobody drains, exactly as when there was never a
    // peer at all. A frame afterwards proves the link recovered, and the status
    // resumes WITHOUT a reconnect (the app may simply have been quiet).
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    hal.AdvanceMs(10001);
    r.Tick();
    cap.lines.clear();

    hal.AdvanceMs(20000);
    r.Tick();
    EXPECT_TRUE(cap.lines.empty()) << "a link gone quiet sends no periodic status";

    // A frame re-arms the link; recovery behaves like a fresh connect, so the
    // status clock is seeded first and the status lands one period later.
    const std::string p2 = "{\"v\":1,\"seq\":2,\"type\":\"ping\"}";
    r.OnLine(p2.c_str(), p2.size());
    cap.lines.clear();
    hal.AdvanceMs(2000);
    r.Tick();                    // seeds the recovered link's status clock
    hal.AdvanceMs(2000);
    r.Tick();
    ASSERT_TRUE(HasType(cap, "status")) << "a frame recovers the link";
}

TEST(CommandRouter, SilenceClosesAnOpenLearnStream) {
    // A learn stream is link-scoped: it belongs to the app session that opened it.
    // If that session dies, the device must stop streaming ladder samples into a
    // FIFO nobody drains.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.OnConnected();
    DrainReplies(r);            // finish the hello-time config reply run

    const std::string ls = "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0}";
    r.OnLine(ls.c_str(), ls.size());
    r.Process();
    ASSERT_TRUE(HasType(cap, "ladder_sample")) << "the stream is live while the app is";
    cap.lines.clear();

    hal.AdvanceMs(10001);
    r.Tick();
    r.Process();
    r.Process();
    EXPECT_FALSE(HasType(cap, "ladder_sample"))
        << "after the link goes quiet the learn stream must stop";
}

TEST(CommandRouter, AConfigPatchOverAnUnreadableConfigRefusesRatherThanOverwriting) {
    // The SAME defect as the learn_commit one, in the sibling handler: a patch is
    // read-modify-write, and `if (Load != kLoaded) c = ConfigDefault()` cannot
    // tell "never configured" from "configured but unreadable". Measured before
    // the fix: patching one scalar over a corrupted config took three bindings to
    // zero and answered `ack`.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.binding_count = 3;
    d.config.settings.timings.debounce_ms = 40;
    ASSERT_TRUE(store.Save(d.config));
    hal.CorruptNvsValue("cfg_a_0", 24);
    hal.CorruptNvsValue("cfg_b_0", 24);
    {
        Config t{};
        ASSERT_EQ(store.Load(&t), ConfigLoadResult::kFellBackToDefaults)
            << "the fixture must actually be unreadable for this test to mean "
               "anything";
    }

    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string p =
        "{\"v\":1,\"seq\":1,\"type\":\"config_patch\",\"path\":\"settings.led_level\",\"value\":3}";
    r.OnLine(p.c_str(), p.size());

    ASSERT_TRUE(HasType(cap, "nack")) << "an unreadable config must be refused";
    EXPECT_NE(cap.lines.back().find("config_unreadable"), std::string::npos);
    EXPECT_FALSE(HasType(cap, "ack"));

    // The store still holds the user's bytes, not defaults plus one field.
    Config after{};
    EXPECT_EQ(store.Load(&after), ConfigLoadResult::kFellBackToDefaults)
        << "a refused patch must leave the stored bytes alone -- a load that now "
           "SUCCEEDS means defaults were written over the user's config";
}

TEST(CommandRouter, TestKeyDrivesTheChannelItNamesNotAlwaysChannelZero) {
    // Spec 4.3 spells the frame `channel`, `key_mv`, `hold_ms`. The handler read
    // ONLY key_mv and hardcoded channel 0, so a bench test of the SECOND output
    // was impossible -- it silently drove the first one instead. The DAC channel
    // is what proves which output moved.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    // Two channels, or channel 1 does not exist and the test would be asserting
    // the range check rather than the routing.
    d.config.channel_count = 2;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    const uint16_t key1_before = hal.LastDacCode(DAC_CH_KEY1);
    const uint16_t key2_before = hal.LastDacCode(DAC_CH_KEY2);

    const std::string t =
        "{\"v\":1,\"seq\":1,\"type\":\"test_key\",\"channel\":1,\"key_mv\":2400,\"hold_ms\":200}";
    r.OnLine(t.c_str(), t.size());
    ASSERT_TRUE(HasType(cap, "ack")) << "an in-envelope channel-1 test must be acked";
    EXPECT_EQ(hal.LastDacCode(DAC_CH_KEY1), key1_before)
        << "channel 1 must not move the channel 0 output";
    EXPECT_NE(hal.LastDacCode(DAC_CH_KEY2), key2_before)
        << "channel 1 must drive the channel 1 output";
}

TEST(CommandRouter, TestKeyRefusesAHoldLongerThanTheBound) {
    // A hold is time the OUTPUT is driven. An unbounded value pins the KEY line,
    // and `now + hold_ms` in uint64 is a wrap primitive. Refused, not clamped: a
    // silently shortened hold would measure a different thing than was asked for.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    const std::string t =
        "{\"v\":1,\"seq\":1,\"type\":\"test_key\",\"channel\":0,\"key_mv\":2400,"
        "\"hold_ms\":100000}";
    r.OnLine(t.c_str(), t.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("bad_param"), std::string::npos);
    EXPECT_FALSE(HasType(cap, "ack"));
}

TEST(CommandRouter, TestKeyRefusesAChannelOutOfRange) {
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    const std::string t =
        "{\"v\":1,\"seq\":1,\"type\":\"test_key\",\"channel\":9,\"key_mv\":2400}";
    r.OnLine(t.c_str(), t.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("bad_param"), std::string::npos);
}

// --- status carries the CONFIG's state (spec 4.3 / 6.8) ----------------------

TEST(CommandRouter, StatusReportsConfigStateNotTheOutputsState) {
    // Spec 6.8 requires a corrupt config to be reported as `config_state:
    // defaults`. The frame instead derived `config_state` from
    // `SafeIdleEstablished()`, so a device running on fallback defaults answered
    // `"ok"` -- the one answer that hides the fault. The output's own state is a
    // SEPARATE field, so neither name can be read as the other.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    ASSERT_TRUE(store.Save(d.config));   // a real, readable stored config
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    ASSERT_STREQ(sys.ConfigStateWord(), "ok") << "fixture: the config loaded";
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "status"));
    const std::string &s = cap.lines.back();
    EXPECT_NE(s.find("\"config_state\":\"ok\""), std::string::npos)
        << "a healthy config is `ok`; got: " << s;
    EXPECT_NE(s.find("\"output_safe\":true"), std::string::npos)
        << "the output's state is its own field now; got: " << s;
}

TEST(CommandRouter, AConfigThatFellBackToDefaultsIsReportedAsDefaults) {
    // The case spec 6.8 exists for: the stored config is unreadable, so the device
    // runs on defaults and MUST say so. This is also the case the old code hid --
    // it reported the OUTPUT's state (`unsafe` only if safe idle failed), so a
    // device on defaults answered `ok` and the fault had no name anywhere.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    ASSERT_TRUE(store.Save(d.config));
    hal.CorruptNvsValue("cfg_a_0", 24);
    hal.CorruptNvsValue("cfg_b_0", 24);
    {
        Config t{};
        ASSERT_EQ(store.Load(&t), ConfigLoadResult::kFellBackToDefaults)
            << "the fixture must actually be unreadable for this test to mean anything";
    }

    // Boot reads through its OWN store, so the corrupted slots are what it sees.
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    ASSERT_STREQ(sys.ConfigStateWord(), "defaults")
        << "an unreadable config must be named, not reported as safe";
    ASSERT_TRUE(sys.SafeIdleEstablished())
        << "the fixture must ALSO be output-safe, or the two fields never diverge "
           "and the test cannot tell the old derivation from the new";

    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "status"));
    const std::string &s = cap.lines.back();
    EXPECT_NE(s.find("\"config_state\":\"defaults\""), std::string::npos)
        << "spec 6.8 names this exact word; got: " << s;
    EXPECT_NE(s.find("\"output_safe\":true"), std::string::npos)
        << "the output IS safe even so -- which is why the two must be separate "
           "fields; got: " << s;
}

TEST(CommandRouter, StatusGainModeReflectsTheOrchestratorsResolvedMode) {
    // The frame hardcoded `"amplified"` with a comment saying the real per-channel
    // mode would be refined in later. `ChannelGainMode(0)` already returned it, so
    // the hardcode was a stale lie: a device running in tracking mode told the app
    // it was amplified.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    // A tracking-mode channel: the config names it concretely, which overrides the
    // device-wide policy, so the resolved mode is unambiguous.
    d.config.channels[0].output.gain_mode = GainMode::kTracking;
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2490);   // head unit present
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    ASSERT_EQ(sys.ChannelGainMode(0), GainMode::kTracking) << "fixture: tracking";
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "status"));
    EXPECT_NE(cap.lines.back().find("\"gain_mode_0\":\"tracking\""), std::string::npos)
        << "the frame must report the mode the device actually resolved; got: "
        << cap.lines.back();
    // A one-channel device reports no mode for a channel that is not there.
    EXPECT_NE(cap.lines.back().find("\"gain_mode_1\":null"), std::string::npos)
        << "with one channel the second gain field must be null, not a value; got: "
        << cap.lines.back();
}

TEST(CommandRouter, StatusGainModeIsReportedPerChannelNotAsOneDeviceWideScalar) {
    // The mode is PER CHANNEL -- FR-14 selects it per channel from `gain_policy`,
    // and the two head-unit inputs are independent (spec 6.2 samples `/SENSEn` per
    // channel), so a 3 V channel and a 5 V channel on the same device legitimately
    // resolve to 1.00 and 1.82 at once. The frame used to carry ONE `gain_mode`
    // built from channel 0 while the name and spec 4.3 ("`gain_mode` is the mode
    // the device actually resolved") read as a device-wide fact (spec N-60). It is
    // now indexed: `gain_mode_0` and `gain_mode_1`, with the second null when there
    // is no second channel. This test drives OPPOSITE modes so a regression to a
    // single scalar -- or to channel 1 -- fails.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.channel_count = 2;
    // Deliberately OPPOSITE concrete modes, so "channel 0 only" is observable: if
    // the emitter ever reported a device-wide value, or channel 1, this fails.
    d.config.channels[0].output.gain_mode = GainMode::kTracking;    // gain 1.00
    d.config.channels[1].output.gain_mode = GainMode::kAmplified;   // gain 1.82
    std::strncpy(d.config.channels[1].name, "SWC2", sizeof(d.config.channels[1].name) - 1);
    d.config.channels[1].ladder.learned_idle_mv = 2835;
    d.config.channels[1].ladder.count = 1;
    d.config.channels[1].ladder.buttons[0] = {"NEXT", "Next", 2145, 110, 3300, 235, 200, 99};
    d.config.channels[1].output.idle_dac_code = 4095;
    d.config.channels[1].enabled = true;
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, 2490);   // head unit present, ch 0
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE2, 2490);   // and ch 1
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    ASSERT_EQ(sys.ChannelGainMode(0), GainMode::kTracking) << "fixture: ch0 tracking";
    ASSERT_EQ(sys.ChannelGainMode(1), GainMode::kAmplified) << "fixture: ch1 amplified";

    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "status"));
    const std::string &s = cap.lines.back();
    // Each channel gets its OWN field. This is the repair of N-60: the frame
    // previously carried one `gain_mode` built from channel 0, which read as a
    // device-wide fact while the mode is per channel.
    EXPECT_NE(s.find("\"gain_mode_0\":\"tracking\""), std::string::npos)
        << "channel 0's resolved mode; got: " << s;
    EXPECT_NE(s.find("\"gain_mode_1\":\"amplified\""), std::string::npos)
        << "channel 1's OWN resolved mode, which is the opposite of channel 0's -- "
           "a single scalar could not carry both; got: " << s;
}

TEST(CommandRouter, StatusReportsTheTransportsLossCounters) {
    // N-24 and its inbound twin. `UsbCdc::DroppedFrames()` documented itself as
    // "the failure this class exists to prevent, so it must be observable" while
    // a unit test was its only reader, and `RxOverflows()` did not even have that
    // claim. A refused OUTBOUND frame fails entirely inside the transport (the
    // sink returns void), so the router cannot learn about one at the `Send`
    // call: the only channel it has to a user is a frame it emits. What is under
    // test is therefore the counter's REACHABILITY, not its correctness.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    // A transport with no raw write, so nothing drains it: `Send` fills the TX
    // buffer and then refuses. The router's sink is the Capture, NOT this
    // transport, so the status frame always reaches the test.
    UsbCdc cdc;
    cdc.Init(nullptr, nullptr, nullptr, nullptr);
    char big[512];
    std::memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    while (cdc.Send(big, sizeof(big) - 1)) { /* fill to capacity */ }
    ASSERT_GT(cdc.DroppedFrames(), 0u) << "fixture: the transport must have refused";

    // The wiring `LinkBind` performs. Without this call the counters exist and
    // reach nobody -- which was the whole of N-24.
    r.SetLossCounters(&cdc);

    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "status"));
    const std::string &s = cap.lines.back();

    const std::string needle = "\"tx_dropped\":";
    const size_t at = s.find(needle);
    ASSERT_NE(at, std::string::npos)
        << "the status must carry the transport's outbound-loss count; got: " << s;
    const unsigned long reported = std::strtoul(s.c_str() + at + needle.size(), nullptr, 10);
    EXPECT_EQ(reported, cdc.DroppedFrames())
        << "the reported count must be the transport's own; got: " << s;
    EXPECT_NE(s.find("\"rx_overflows\":0"), std::string::npos)
        << "a quiet inbound side reports zero; got: " << s;
}

TEST(CommandRouter, StatusReportsTheNtcTemperatureAndTheFreeHeap) {
    // Open item N-22's last two fields. Both were declared in spec 4.3's row and
    // written by nothing, so the app read a field that never arrived. What is
    // under test is that each now has a REAL producer: the temperature from the
    // NTC reading path, the heap from the HAL.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();

    // A settled NTC: 25.0 C is 250 tenths, and the reading path records the last
    // GOOD value. The mock's NTC node voltage must convert through the B3380
    // model, so drive it with a value the converter accepts and prove the status
    // reports what `LastNtcTenthsC` holds rather than a literal.
    hal.SetAdcMilliVolts(ADC_CH_TEMP, 1500);
    const int tenths = sys.SampleNtcTenthsC();
    ASSERT_NE(tenths, SystemOrchestrator::kTempNotMeasuredTenths)
        << "fixture: the NTC reading must be a real temperature";
    ASSERT_EQ(sys.LastNtcTenthsC(), tenths)
        << "fixture: SampleNtcTenthsC must have recorded the value";
    hal.SetHeapFree(123456u);

    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "status"));
    const std::string &s = cap.lines.back();

    // The temperature is written as a DECIMAL, the same convention the config
    // codec uses for `temp_c_at_learn`. Build the expected string from the tenths
    // so the assertion does not hardcode a model constant.
    char want[24];
    std::snprintf(want, sizeof(want), "\"temp_c\":%d.%d", tenths / 10,
                  (tenths < 0 ? -tenths : tenths) % 10);
    EXPECT_NE(s.find(want), std::string::npos)
        << "the status must carry the NTC temperature; want " << want << ", got: " << s;
    EXPECT_NE(s.find("\"heap_free\":123456"), std::string::npos)
        << "the status must carry the HAL's free-heap figure; got: " << s;
}

TEST(CommandRouter, StatusReportsNoTemperatureRatherThanZeroWhenNoneWasMeasured) {
    // The NTC sentinel is `kTempNotMeasuredTenths` (0), and 0 C is a LEGAL
    // temperature -- so reporting a bare `0.0` for "never measured" is the same
    // lie the sentinel exists to prevent elsewhere. The field is JSON null until a
    // reading is good.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);   // no orchestrator
    cap.Attach(r);
    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    ASSERT_TRUE(HasType(cap, "status"));
    const std::string &s = cap.lines.back();
    EXPECT_NE(s.find("\"temp_c\":null"), std::string::npos)
        << "no measured temperature is `null`, never a fabricated 0 C; got: " << s;
    EXPECT_EQ(s.find("\"temp_c\":0.0"), std::string::npos)
        << "a `0.0` would be indistinguishable from a measured freezing board; got: " << s;
}

// --- config_patch value bounds (spec 4.3: a patch is one field) --------------

TEST(CommandRouter, AConfigPatchRefusesAValueTheCodecWouldRefuse) {
    // The handler cast a peer double straight to uint8_t/uint32_t. Measured before
    // the fix: `buzzer_level = 259` persisted as 3, and `send_duration_ms = 1e10`
    // persisted as 4294967295 -- a ~49-DAY KEY-line hold. `ConfigValidate` only
    // checks these are nonzero and internally ordered, so it accepted all of them;
    // the codec's ReadU32/ReadU8 are the rule, and this path had no equivalent.
    struct Case { const char *path; const char *value; };
    const Case cases[] = {
        {"settings.buzzer_level", "259"},              // wraps into 0..3
        {"settings.led_level", "256"},
        {"settings.timings.long_press_ms", "1e19"},    // > UINT32_MAX
        {"settings.timings.send_duration_ms", "1e10"},
        {"settings.timings.debounce_ms", "-5"},
        {"settings.timings.debounce_ms", "2.7"},       // fractional ms
    };
    for (const Case &c : cases) {
        MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
        ASSERT_TRUE(store.Save(MockHalDefaultsConfig()));
        CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
        cap.Attach(r);
        const std::string patch = std::string("{\"v\":1,\"seq\":1,\"type\":\"config_patch\",\"path\":\"") +
                                  c.path + "\",\"value\":" + c.value + "}";
        r.OnLine(patch.c_str(), patch.size());
        EXPECT_TRUE(HasType(cap, "nack")) << c.path << "=" << c.value << " must be refused";
        EXPECT_FALSE(HasType(cap, "ack")) << c.path << "=" << c.value << " must not be acked";

        // And the stored config is untouched: the hostile value did not land.
        Config out{};
        ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
        EXPECT_EQ(out.settings.timings.send_duration_ms, 200u)
            << "a refused patch must not have written a wrapped value";
        EXPECT_EQ(out.settings.buzzer_level, 2u);
    }
}

TEST(CommandRouter, AConfigPatchStillAcceptsAnInRangeValueAtTheBoundary) {
    // The bound must not be so tight it refuses a legal value: 0xFF for a level
    // and UINT32_MAX for a timing are both IN range for the conversion (whether
    // they are semantically sane is ConfigValidate's call, and it refuses what it
    // should -- the point here is that the RANGE check does not over-reject).
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    ASSERT_TRUE(store.Save(MockHalDefaultsConfig()));
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string patch =
        "{\"v\":1,\"seq\":1,\"type\":\"config_patch\",\"path\":"
        "\"settings.timings.long_press_ms\",\"value\":900}";
    r.OnLine(patch.c_str(), patch.size());
    ASSERT_TRUE(HasType(cap, "ack"));
    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_EQ(out.settings.timings.long_press_ms, 900u);
}

TEST(CommandRouter, APatchCanSetEverySettingsScalarIncludingTheMaintenanceWindow) {
    // The handler's own comment scopes it to "`settings.*` scalars only", and the
    // table covered every one EXCEPT `maintenance_timeout_ms` -- so a client could
    // patch every timing and both feedback levels but not the maintenance window.
    // This pins the full set, which is what the comment claims, and is what makes
    // a future added scalar fail here rather than go silently unpatchable.
    struct Case { const char *path; const char *value; };
    const Case cases[] = {
        {"settings.timings.debounce_ms", "30"},
        {"settings.timings.double_press_off_ms", "450"},
        {"settings.timings.long_press_ms", "700"},
        {"settings.timings.send_duration_ms", "250"},
        {"settings.buzzer_level", "1"},
        {"settings.led_level", "0"},
        {"settings.maintenance_timeout_ms", "120000"},
    };
    for (const Case &c : cases) {
        MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
        ASSERT_TRUE(store.Save(MockHalDefaultsConfig()));
        CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
        cap.Attach(r);
        const std::string patch = std::string("{\"v\":1,\"seq\":1,\"type\":\"config_patch\",\"path\":\"") +
                                  c.path + "\",\"value\":" + c.value + "}";
        r.OnLine(patch.c_str(), patch.size());
        EXPECT_TRUE(HasType(cap, "ack")) << c.path << " must be a patchable settings scalar";
        EXPECT_FALSE(HasType(cap, "unknown_path")) << c.path << " is in the handler's declared scope";
    }
}

TEST(CommandRouter, APatchOfTheMaintenanceWindowIsRangeChecked) {
    // The new path is not a bypass of the range rule the others follow: zero and
    // a value past `kMaintenanceTimeoutMaxMs` are both refused (ConfigValidate),
    // leaving the stored config untouched -- the same contract as
    // `AConfigPatchRefusesAValueTheCodecWouldRefuse`.
    const char *bad[] = {"0", "3600001"};
    for (const char *v : bad) {
        MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
        Config before = MockHalDefaultsConfig();
        before.settings.maintenance_timeout_ms = 300000;
        ASSERT_TRUE(store.Save(before));
        CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
        cap.Attach(r);
        const std::string patch =
            std::string("{\"v\":1,\"seq\":1,\"type\":\"config_patch\",\"path\":"
                        "\"settings.maintenance_timeout_ms\",\"value\":") + v + "}";
        r.OnLine(patch.c_str(), patch.size());
        EXPECT_FALSE(HasType(cap, "ack")) << "maintenance_timeout_ms=" << v << " must be refused";
        Config out{};
        ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
        EXPECT_EQ(out.settings.maintenance_timeout_ms, 300000u)
            << "a refused patch must not have written the value";
    }
}

TEST(CommandRouter, AChannelIndexWithAFractionIsRefusedNotRounded) {
    // Every channel-reading handler range-checked the CONVERTED int, which cannot
    // see a fraction: `channel = 1.9` became 1 and drove the first channel -- a
    // command accepted as channel 1.9 but executed on channel 1. Same rule as the
    // codec's integer fields.
    const char *frames[] = {
        "{\"v\":1,\"seq\":1,\"type\":\"test_key\",\"channel\":1.9,\"key_mv\":2400}",
        "{\"v\":1,\"seq\":2,\"type\":\"learn_start\",\"channel\":0.5}",
        "{\"v\":1,\"seq\":3,\"type\":\"learn_commit\",\"channel\":1.2,\"button_id\":\"b\",\"name\":\"B\"}",
    };
    for (const char *f : frames) {
        MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
        ASSERT_TRUE(store.Save(MockHalDefaultsConfig()));
        CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
        cap.Attach(r);
        r.OnLine(f, strlen(f));
        EXPECT_TRUE(HasType(cap, "nack")) << f;
        EXPECT_FALSE(HasType(cap, "ack")) << f;
    }
}

TEST(CommandRouter, AnOutOfRangeCrcIsRefusedRatherThanWrapped) {
    // `crc32 = 1e10` was cast to a wrapped 32-bit value no real CRC equals, so
    // every chunk of the run was then rejected as corrupt with no visible reason.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const char *begin =
        "{\"v\":1,\"seq\":1,\"type\":\"config_begin\",\"total_len\":100,\"crc32\":1e10}";
    r.OnLine(begin, strlen(begin));
    EXPECT_TRUE(HasType(cap, "nack"));
    EXPECT_FALSE(HasType(cap, "ack"));
}

TEST(CommandRouter, ATestKeyWithAFractionalMillivoltIsRefused) {
    // A bare `static_cast<int>` of an out-of-range double is undefined behavior,
    // and the envelope check downstream can only see an int formed safely. This
    // needs a REAL orchestrator, or the handler nacks `unavailable` before it
    // ever reaches the key_mv conversion -- and the test would pass for a reason
    // that has nothing to do with the conversion.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    const char *f =
        "{\"v\":1,\"seq\":1,\"type\":\"test_key\",\"channel\":0,\"key_mv\":2400.5}";
    r.OnLine(f, strlen(f));
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("bad_param"), std::string::npos)
        << "the refusal must be the key_mv check, not `unavailable`";
    EXPECT_FALSE(HasType(cap, "ack"));

    // And an in-envelope integer key still drives (the bound is not over-tight).
    Capture cap2;
    cap2.Attach(r);
    const char *g =
        "{\"v\":1,\"seq\":2,\"type\":\"test_key\",\"channel\":0,\"key_mv\":2400}";
    r.OnLine(g, strlen(g));
    EXPECT_TRUE(HasType(cap2, "ack")) << "an in-envelope integer key must still be acked";
}

TEST(CommandRouter, TheStreamedSamplesAreWhatALearnCommitAccepts) {
    // Spec 4.3 and this router's header both describe the learn flow as
    // learn_start -> ladder_sample frames -> ONE learn_commit that "accepts the
    // streamed samples". That could not work: `Process()` emitted and COUNTED
    // samples but recorded none, and `AddSample` ran only inside the commit (once)
    // while `LearnSession::Commit` needs >=10 samples over >=100 ms. Measured
    // before the fix: a 30-frame stream followed by one learn_commit returned
    // `learn_rejected: too_few_samples`, so the specified protocol never succeeded.
    // The host suite masked it by firing 20 learn_commit frames, which is not a
    // flow anyone would write.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.channels[0].ladder.count = 0;
    d.config.channels[0].ladder.learned_idle_mv = 2835;
    d.config.binding_count = 0;
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }

    const std::string ls = "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0}";
    r.OnLine(ls.c_str(), ls.size());
    ASSERT_TRUE(HasType(cap, "ack")) << "learn_start must open the stream";

    // Stream for >=100 ms of span and well past the 10-sample floor.
    int emitted = 0;
    for (int i = 0; i < 30; ++i) {
        cap.lines.clear();
        r.Process();
        hal.AdvanceMs(10);
        if (HasType(cap, "ladder_sample")) ++emitted;
    }
    ASSERT_GE(emitted, 12) << "the stream must actually run for this test to mean anything";

    // ONE commit, the way the spec's frame list describes it.
    cap.lines.clear();
    const std::string lc =
        "{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
        "\"button_id\":\"vol_dn\",\"name\":\"Volume Down\"}";
    r.OnLine(lc.c_str(), lc.size());
    ASSERT_TRUE(HasType(cap, "ack"))
        << "one learn_commit after a real stream must be ACCEPTED; a "
           "too_few_samples here means the stream was not recorded: "
        << (cap.lines.empty() ? "(nothing)" : cap.lines.back());

    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    ASSERT_EQ(out.channels[0].ladder.count, 1u) << "the streamed samples must become a button";
    EXPECT_STREQ(out.channels[0].ladder.buttons[0].id, "vol_dn");
    EXPECT_NEAR(out.channels[0].ladder.buttons[0].mv_center, 1430, 60);
    // `learned_at_rail_mv` is the +3V3 RAIL (spec 3.4/FR-30), not the wheel's
    // idle. The app-driven path used to pass the idle (~2835 here) for this field,
    // so the app displayed a rail that was really an idle level and the sagging-
    // regulator check compared against the wrong number -- while the headless
    // wizard recorded the nominal rail. This asserts the RAIL, which is what
    // separates the two quantities.
    EXPECT_EQ(out.channels[0].ladder.buttons[0].learned_at_rail_mv, kNominalRailMv)
        << "the recorded rail must be the nominal +3V3 rail, not the wheel's idle";
}

TEST(CommandRouter, ALearnCommitAppliesImmediately) {
    // The third write path (spec 4.2/7.3, and the headless equivalent already did
    // this via `ApplyLearnedProfile`). The app's learn screen is gone after the
    // `ack`, so a device that re-derived its classifier only at the next boot would
    // leave the user with a button they just measured doing nothing.
    //
    // The observable is the just-learned button RESOLVING: `mute` is seeded on the
    // ladder at 2000 mV, and the learn re-measures it to 1430. A press at 1430 is
    // unrecognised before and must resolve after, with no reboot.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    // One seeded button, FAR from the level the learn will measure, so the two
    // windows cannot overlap and the before/after are unambiguous.
    d.config.channels[0].ladder.learned_idle_mv = 2835;
    d.config.channels[0].ladder.count = 1;
    d.config.channels[0].ladder.buttons[0] =
        {"mute", "Mute", 2000, 60, 3300, 235, 200, 98};
    d.config.binding_count = 1;
    std::snprintf(d.config.bindings[0].id, sizeof(d.config.bindings[0].id), "lb");
    std::snprintf(d.config.bindings[0].button, sizeof(d.config.bindings[0].button), "mute");
    d.config.bindings[0].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    d.config.bindings[0].gesture = Gesture::kSingle;
    d.config.bindings[0].enabled = true;
    d.config.bindings[0].action_count = 1;
    d.config.bindings[0].actions[0].kind = ActionKind::kOutVoltage;
    d.config.bindings[0].actions[0].key_mv = 2400;
    ASSERT_TRUE(ConfigValidate(d.config));
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    std::vector<std::string> ids;
    sys.SetGestureSink(
        [](void *ctx, const SystemOrchestrator::GestureEventRecord &ev) {
            auto *v = static_cast<std::vector<std::string> *>(ctx);
            if (ev.button_id != nullptr) v->push_back(ev.button_id);
        },
        &ids);
    hal.SetAdcMilliVolts(ADC_CH_KEY_SENSE1, kSenseFor5vHeadUnit);
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    auto tap_at = [&](int mv) {
        ids.clear();
        hal.SetAdcMilliVolts(ADC_CH_SWC1, mv);
        for (uint32_t t = 0; t < 100; t += 10) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
        hal.SetAdcMilliVolts(ADC_CH_SWC1, 2835);
        for (uint32_t t = 0; t < 400; t += 10) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    };

    // Before the learn, 1430 is not a window: no button is reported.
    tap_at(1430);
    EXPECT_TRUE(ids.empty())
        << "the seeded ladder must not recognize 1430, or this proves nothing";

    // Stream and commit `mute` at 1430 mV -- an id already on the ladder, which the
    // commit REPLACES in place.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    const std::string ls = "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0}";
    r.OnLine(ls.c_str(), ls.size());
    ASSERT_TRUE(HasType(cap, "ack")) << "learn_start must open the stream";
    for (int i = 0; i < 30; ++i) { cap.lines.clear(); r.Process(); hal.AdvanceMs(10); }
    cap.lines.clear();
    const std::string lc =
        "{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
        "\"button_id\":\"mute\",\"name\":\"Mute\"}";
    r.OnLine(lc.c_str(), lc.size());
    ASSERT_TRUE(HasType(cap, "ack"))
        << "the commit must be accepted for this to mean anything: "
        << (cap.lines.empty() ? "(nothing)" : cap.lines.back());

    // The button the user just measured must resolve NOW -- no reboot.
    tap_at(1430);
    EXPECT_NE(std::find(ids.begin(), ids.end(), std::string("mute")), ids.end())
        << "the just-learned button did not resolve: learn_commit persisted the "
           "profile but did not apply it, so the running classifier still holds the "
           "old window and calls the level the user just measured unrecognised";
}

TEST(CommandRouter, ALearnCommitOnADifferentChannelThanTheStreamIsRefused) {
    // The session is fed from the STREAM's channel (`RecordLearnSample`), but the
    // commit wrote to the channel it NAMED. Measured before the fix:
    // learn_start(channel 0), a stream over channel 0's input, then
    // learn_commit(channel 1) returned `ack` and stored channel 0's 1430 mV
    // measurement on channel 1's ladder -- while channel 1's own input sat idle at
    // 2835 mV. A wrong-channel write from a mismatched frame.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.channel_count = 2;
    for (int i = 0; i < 2; ++i) {
        // The fixture ships channel_count = 1, so channel 1 is default-built with
        // an EMPTY name -- which `ConfigValidate` refuses, making the stored config
        // undecodable (Save does not validate; Load's decode does).
        std::strncpy(d.config.channels[i].name, (i == 0) ? "SWC1" : "SWC2",
                     sizeof(d.config.channels[i].name) - 1);
        d.config.channels[i].ladder.count = 0;
        d.config.channels[i].ladder.learned_idle_mv = 2835;
    }
    d.config.binding_count = 0;
    ASSERT_TRUE(store.Save(d.config));
    { Config probe{}; ASSERT_EQ(store.Load(&probe), ConfigLoadResult::kLoaded)
          << "the fixture must be loadable for this test to mean anything"; }
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    // Channel 0 pressed, channel 1 idle.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 2835);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }

    const std::string ls = "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0}";
    r.OnLine(ls.c_str(), ls.size());
    for (int i = 0; i < 20; ++i) { r.Process(); hal.AdvanceMs(20); }

    cap.lines.clear();
    const std::string lc =
        "{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":1,"
        "\"button_id\":\"x\",\"name\":\"X\"}";
    r.OnLine(lc.c_str(), lc.size());
    ASSERT_TRUE(HasType(cap, "nack")) << "a channel mismatch must be refused, not acked";
    EXPECT_NE(cap.lines.back().find("channel_mismatch"), std::string::npos);

    // And NOTHING was written to either channel.
    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_EQ(out.channels[0].ladder.count, 0u) << "no button may be stored from a refused commit";
    EXPECT_EQ(out.channels[1].ladder.count, 0u)
        << "channel 1 must not receive channel 0's measurement";
}

TEST(CommandRouter, ALearnTakenOnAMovedRailNormalizesToTheLiveIdle) {
    // Spec 6.3: the ratio denominator is `V_ADC_idle`, the LIVE idle -- NOT the
    // config's `learned_idle_mv`. The app-driven path (`RecordLearnSample`) passed
    // the STORED learned idle while the headless wizard passed the live reading, so
    // the two learn paths disagreed, and on a moved rail the app recorded a
    // button's `mv_center` against a denominator the classify step would never use.
    //
    // This test moves the rail between the stored learn and the re-learn: the
    // config's learned idle is 2835 (the nominal rail), but the wheel now idles at
    // 2660 (a ~-6 % rail, still inside the ADC ceiling). A committed button must
    // carry `mv_center` in the units of THAT rail AND stamp its own
    // `learned_idle_mv` with it, so the stored pair stays self-consistent.
    //
    // Under the old code the denominator was 2835, so a 1330 mV press recorded
    // `mv_center = 1330` and stamped `learned_idle_mv = 2835` -- mixing the two.
    //
    // The ladder also carries a SIBLING button measured at the stored 2835 rail.
    // A profile has ONE denominator, so the re-learn must rescale that sibling
    // onto the live rail too: leaving it in the old frame while the profile is
    // stamped with the new one makes classification read it on the wrong scale,
    // and nearest-centre matching then fires the WRONG button rather than
    // reporting an error.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.channels[0].ladder.count = 1;
    d.config.channels[0].ladder.learned_idle_mv = 2835;   // the STALE stored rail
    // 2450 mV at the 2835 rail = 864 permille, 200 mV from the 2250-mV button the
    // re-learn will land on -- a gap the commit's tolerance cannot cover.
    d.config.channels[0].ladder.buttons[0] = {"sib", "Sibling", 2450, 100, 3300, 235, 200, 98};
    d.config.binding_count = 0;
    ASSERT_TRUE(store.Save(d.config));

    // The rail has since moved DOWN, to 2700 mV. Boot idles the input there before
    // it establishes the reference, and 2700/2835 = 952 permille is inside spec
    // 6.3's +-5 % plausibility window, so the live reading is adopted rather than
    // the stored 2835.
    const int kMovedIdle = 2700;
    hal.SetAdcMilliVolts(ADC_CH_SWC1, kMovedIdle);
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    ASSERT_EQ(sys.IdleReferenceMv(0), kMovedIdle)
        << "Boot must adopt the live moved rail as the denominator (spec 6.3)";

    // Hold a button at 1330 mV (a real press on the moved rail).
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1330);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }

    const std::string ls = "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0}";
    r.OnLine(ls.c_str(), ls.size());
    for (int i = 0; i < 30; ++i) { r.Process(); hal.AdvanceMs(10); }

    cap.lines.clear();
    const std::string lc =
        "{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
        "\"button_id\":\"vol_dn\",\"name\":\"Volume Down\"}";
    r.OnLine(lc.c_str(), lc.size());
    ASSERT_TRUE(HasType(cap, "ack")) << "the learn must be accepted: "
                                     << (cap.lines.empty() ? "(nothing)" : cap.lines.back());

    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    ASSERT_EQ(out.channels[0].ladder.count, 2u) << "the sibling must survive the re-learn";
    EXPECT_EQ(out.channels[0].ladder.learned_idle_mv, kMovedIdle)
        << "the committed learn must stamp the rail it was measured at (the LIVE "
           "idle), not the stale stored one -- 2835 here means the denominator "
           "came from the config instead of the live reading (spec 6.3)";

    // The measured button is in the live frame; the sibling must have been
    // rescaled INTO it. 2450 mV at 2835 is 2450 * 2700/2835 = 2333 mV at 2700, and
    // its permille window must be unchanged by the move.
    const LadderButton *learned_btn = nullptr;
    const LadderButton *sibling = nullptr;
    for (uint8_t i = 0; i < out.channels[0].ladder.count; ++i) {
        const LadderButton &b = out.channels[0].ladder.buttons[i];
        if (strcmp(b.id, "vol_dn") == 0) learned_btn = &b;
        if (strcmp(b.id, "sib") == 0) sibling = &b;
    }
    ASSERT_NE(learned_btn, nullptr);
    ASSERT_NE(sibling, nullptr);
    EXPECT_NEAR(learned_btn->mv_center, 1330, 60)
        << "the centre is in millivolts ON THE MEASURED RAIL";
    EXPECT_NEAR(sibling->mv_center, 2333, 20)
        << "the sibling must be REBASED onto the live rail, not left at its stored "
           "2450 mV: two frames under one denominator makes classification read it "
           "on the wrong scale and fire the wrong button";
    EXPECT_EQ(LadderRatioPermille(sibling->mv_center, kMovedIdle), 864)
        << "and rebasing must leave the sibling's permille window the same as it "
           "was (its centre and the denominator scale together), to within the one "
           "permille that rounding the millivolts costs";
}

TEST(CommandRouter, ALearnCommitWithAnEmptyButtonIdIsRefused) {
    // `Str` returns the item for `""` -- a valid JSON string with a non-null
    // `valuestring` -- so a present-but-EMPTY id passed the "required" guard and
    // was copied into the stored button. Neither `LearnSession::Commit` nor
    // `ConfigStore::Save` validates it, so it persisted; the next boot's decode
    // refuses the empty string, `Load` falls back to defaults, and the user loses
    // the WHOLE config (both channels, every binding) reported only as corrupt.
    // Tests missed it because every learn_commit they send has a real id.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    cap.lines.clear();
    const std::string lc =
        "{\"v\":1,\"seq\":1,\"type\":\"learn_commit\",\"channel\":0,"
        "\"button_id\":\"\",\"name\":\"\"}";
    r.OnLine(lc.c_str(), lc.size());
    ASSERT_TRUE(HasType(cap, "nack")) << "an empty id/name must be refused";
    EXPECT_NE(cap.lines.back().find("bad_param"), std::string::npos);
}

TEST(CommandRouter, ALearnCommitAfterAStopStillRefusesTheWrongChannel) {
    // The channel guard keyed on `learn_open_`, but `learn_stop` is the SPECIFIED
    // flow (spec 4.3: start, stream, stop, commit), and it clears `learn_open_`
    // while leaving the measured samples in the session. So the ordinary flow
    // defeated the guard: learn_start(0) -> stream -> learn_stop ->
    // learn_commit(1) wrote channel 0's measurement onto channel 1's ladder.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.channel_count = 2;
    for (int i = 0; i < 2; ++i) {
        // Channel 1 is default-built with an empty NAME, which ConfigValidate
        // refuses -- making the stored config undecodable. Name both.
        std::strncpy(d.config.channels[i].name, (i == 0) ? "SWC1" : "SWC2",
                     sizeof(d.config.channels[i].name) - 1);
        d.config.channels[i].ladder.count = 0;
        d.config.channels[i].ladder.learned_idle_mv = 2835;
    }
    d.config.binding_count = 0;
    ASSERT_TRUE(store.Save(d.config));
    { Config probe{}; ASSERT_EQ(store.Load(&probe), ConfigLoadResult::kLoaded); }
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    hal.SetAdcMilliVolts(ADC_CH_SWC2, 2835);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }

    const std::string ls = "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0}";
    r.OnLine(ls.c_str(), ls.size());
    for (int i = 0; i < 20; ++i) { r.Process(); hal.AdvanceMs(20); }
    const std::string lst = "{\"v\":1,\"seq\":2,\"type\":\"learn_stop\"}";
    r.OnLine(lst.c_str(), lst.size());

    cap.lines.clear();
    const std::string lc =
        "{\"v\":1,\"seq\":3,\"type\":\"learn_commit\",\"channel\":1,"
        "\"button_id\":\"x\",\"name\":\"X\"}";
    r.OnLine(lc.c_str(), lc.size());
    ASSERT_TRUE(HasType(cap, "nack"))
        << "a stop between the stream and the commit must not defeat the guard";
    EXPECT_NE(cap.lines.back().find("channel_mismatch"), std::string::npos);
    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    EXPECT_EQ(out.channels[1].ladder.count, 0u)
        << "channel 1 must not receive channel 0's measurement";
}

TEST(CommandRouter, ALearnCommitClosesTheStream) {
    // The commit ends the learn run, so the device must stop streaming and stop
    // accumulating. Leaving it open kept emitting `ladder_sample` and fed
    // `session_` post-press idle readings, so a duplicate commit re-ran over idle
    // samples and a client that committed and went quiet left the device streaming
    // forever.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    d.config.channels[0].ladder.count = 0;
    d.config.binding_count = 0;
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);

    hal.SetAdcMilliVolts(ADC_CH_SWC1, 1430);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    const std::string ls = "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0}";
    r.OnLine(ls.c_str(), ls.size());
    for (int i = 0; i < 20; ++i) { r.Process(); hal.AdvanceMs(20); }
    const std::string lc =
        "{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
        "\"button_id\":\"vol_up\",\"name\":\"Vol Up\"}";
    r.OnLine(lc.c_str(), lc.size());
    ASSERT_TRUE(HasType(cap, "ack")) << "the commit should be accepted";

    cap.lines.clear();
    for (int i = 0; i < 20; ++i) { r.Process(); hal.AdvanceMs(20); }
    EXPECT_FALSE(HasType(cap, "ladder_sample"))
        << "the stream must stop once the button is committed";
}

TEST(CommandRouter, TheTwoIdentifyPatternsDoNotDoTheSameThing) {
    // `pattern` is the frame's field and the two values mean different things --
    // `flash` borrows LED_STAT, `buzz` sounds the buzzer. They were both routed to
    // one call that did BOTH, an "accepts a field and ignores it" defect: a user
    // asking for buzz-only (they are at the wheel, not the box) also got the LED
    // burst. Asserted by the observable consequence over the WHOLE flash window,
    // not the call and not a short sample -- the flash pattern does not write on
    // its first tick, so a brief poll cannot tell the two apart.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    sys.SetUsbConnected(true);
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    const uint32_t window = SystemOrchestrator::kIdentifyFlashMs + 300;

    // buzz: the buzzer sounds, the LEDs are untouched.
    for (int i = 0; i < 10; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    const int led_before = hal.GpioWriteCount(GPIO_LED_STAT);
    const int buzz_before = hal.BuzzerOnCount();
    const std::string buzz = "{\"v\":1,\"seq\":1,\"type\":\"identify\",\"pattern\":\"buzz\"}";
    r.OnLine(buzz.c_str(), buzz.size());
    EXPECT_TRUE(HasType(cap, "ack"));
    for (uint32_t e = 0; e < window; e += 10) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_GT(hal.BuzzerOnCount(), buzz_before) << "buzz must sound the buzzer";
    EXPECT_EQ(hal.GpioWriteCount(GPIO_LED_STAT), led_before)
        << "buzz must NOT drive the LEDs -- that is `flash`'s job, and collapsing "
           "the two patterns is what this test exists to prevent";

    // flash: the LEDs move. This half is what makes the test bite, because it
    // proves the window is long enough to have caught a stray flash above.
    const int led2 = hal.GpioWriteCount(GPIO_LED_STAT);
    const std::string flash = "{\"v\":1,\"seq\":2,\"type\":\"identify\",\"pattern\":\"flash\"}";
    r.OnLine(flash.c_str(), flash.size());
    EXPECT_TRUE(HasType(cap, "ack"));
    for (uint32_t e = 0; e < window; e += 10) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    EXPECT_GT(hal.GpioWriteCount(GPIO_LED_STAT), led2)
        << "flash must drive LED_STAT over its burst";
}

TEST(CommandRouter, SilenceDiscardsAPartlySentConfigGetReply) {
    // Spec 4.4: after 10 s of silence the link is DOWN, and a run that belongs to
    // the link goes with it. The `config_get` reply is such a run: `Process`
    // emits one chunk per call and gates ONLY on `reply_open_` (not on
    // `connected_`), so a reply left open past the end of its link streams the
    // rest of a config to nobody -- filling the TX buffer until a real reply is
    // refused. The silence reap closed the `config_set` run but not this one;
    // `OnDisconnected` closed both, and that drift is the defect.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.OnConnected();
    DrainReplies(r);                 // the hello-time auto-reply finishes
    cap.lines.clear();

    const std::string get = "{\"v\":1,\"seq\":1,\"type\":\"config_get\"}";
    r.OnLine(get.c_str(), get.size());
    r.Process();                     // config_begin goes out
    ASSERT_TRUE(HasType(cap, "config_begin")) << "the reply run must have started";
    EXPECT_FALSE(HasType(cap, "config_end")) << "and must NOT be finished yet";
    cap.lines.clear();

    hal.AdvanceMs(10001);
    r.Tick();                        // the reap
    for (int i = 0; i < 8; ++i) r.Process();
    EXPECT_FALSE(HasType(cap, "config_chunk"))
        << "after the link is down the rest of the reply must not be emitted";
    EXPECT_FALSE(HasType(cap, "config_end"));
}

TEST(CommandRouter, ACommitAfterTheLinkDroppedHasNoSessionToCommit) {
    // Spec 4.4: reconnect is stateless. The learn SESSION -- the samples and the
    // channel -- belongs to the app session that opened it, so it must not
    // outlive the link. Measured before the fix: a stream, a disconnect, then a
    // `learn_commit` with NO `learn_start` committed the previous session's
    // measurement and stamped `learned_idle_mv` from a rail measured before the
    // drop. The reap must empty the session, not merely close the stream.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    r.OnConnected();
    DrainReplies(r);

    // A real streamed measurement on channel 0. 2400 mV is deliberately clear of
    // the fixture's three buttons (1430/1785/2145 mV, tolerance 120) and of the
    // idle, so a session that SURVIVED the link drop would commit it successfully
    // -- which is what makes this test bite. A level that collided with an
    // existing button would be refused either way and mask the defect.
    hal.SetAdcMilliVolts(ADC_CH_SWC1, 2400);
    for (int i = 0; i < 20; ++i) { sys.Tick(hal.NowMs()); hal.AdvanceMs(10); }
    const std::string ls =
        "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0,\"button_id\":\"brand_new\"}";
    r.OnLine(ls.c_str(), ls.size());
    for (int i = 0; i < 20; ++i) { r.Process(); hal.AdvanceMs(20); }

    // The link drops without a transport event, the way a radio going quiet does.
    hal.AdvanceMs(10001);
    r.Tick();
    cap.lines.clear();

    const std::string lc =
        "{\"v\":1,\"seq\":2,\"type\":\"learn_commit\",\"channel\":0,"
        "\"button_id\":\"brand_new\",\"name\":\"Brand New\"}";
    r.OnLine(lc.c_str(), lc.size());
    EXPECT_TRUE(HasType(cap, "nack")) << "a commit with no session must be refused";
    EXPECT_FALSE(HasType(cap, "ack"));

    Config out{};
    ASSERT_EQ(store.Load(&out), ConfigLoadResult::kLoaded);
    // The dead session's measurement must not have become a fourth button. With
    // the session discarded the commit is refused and the ladder is untouched;
    // had the reap left the session behind, this commit would have appended
    // `brand_new` at ~2400 mV.
    ASSERT_EQ(out.channels[0].ladder.count, 3);
    for (uint8_t i = 0; i < out.channels[0].ladder.count; ++i) {
        EXPECT_STRNE(out.channels[0].ladder.buttons[i].id, "brand_new")
            << "the dead session's samples must not have been written to the config";
    }
}

TEST(CommandRouter, ACommitWithNoLearnStartEverNamesNoSession) {
    // `learn_channel_` must START at the `-1` "no session" sentinel, not `0`. The
    // guard in `HandleLearnCommit` refuses `no_session` on `learn_channel_ < 0`
    // BEFORE comparing the frame's channel to the session's, so a default of `0`
    // let `learn_commit{channel:0}` clear BOTH guards on a router that had never
    // seen a `learn_start`, run `session_.Commit` on an empty session, and answer
    // `learn_rejected: too_few_samples`. That names a sample count as the cause of
    // a refusal whose real cause is that no stream was ever opened -- the
    // un-actionable reason FR-29 forbids -- and it pointed the user at "press the
    // button more" when the fix is "start a learn first".
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    MockHal::Defaults d;
    ASSERT_TRUE(store.Save(d.config));
    SystemOrchestrator sys(&hal.InterfaceRef(), d.config, d.timings);
    sys.Boot();
    CommandRouter r(&hal.InterfaceRef(), &sys, &store);
    cap.Attach(r);
    r.OnConnected();
    DrainReplies(r);
    cap.lines.clear();

    // Channel 0 -- the value the old initializer held, so this is the frame that
    // was misreported. Nothing else about the frame is wrong: the id and name are
    // legal, so every guard AFTER `no_session` would pass it.
    const std::string lc =
        "{\"v\":1,\"seq\":1,\"type\":\"learn_commit\",\"channel\":0,"
        "\"button_id\":\"never_started\",\"name\":\"Never Started\"}";
    r.OnLine(lc.c_str(), lc.size());

    ASSERT_TRUE(HasType(cap, "nack")) << "a commit with no learn ever started must be refused";
    EXPECT_FALSE(HasType(cap, "ack"));
    // The REASON is the assertion: `too_few_samples` is the specific wrong answer
    // a `0` initializer produced, and it is a `nack` either way, so a test that
    // only checked for `nack` would not bite.
    bool named_no_session = false;
    bool named_too_few_samples = false;
    for (const auto &l : cap.lines) {
        if (l.find("\"err\":\"no_session\"") != std::string::npos) named_no_session = true;
        if (l.find("too_few_samples") != std::string::npos) named_too_few_samples = true;
    }
    EXPECT_TRUE(named_no_session)
        << "the reason must name the absent session; got: " << cap.lines.back();
    EXPECT_FALSE(named_too_few_samples)
        << "a sample count must NOT be blamed for a commit that never had a stream: "
        << cap.lines.back();
}

TEST(CommandRouter, LearnStopNamesTheStreamToClose) {
    // `learn_stop` used to `(void)root`, so `learn_stop{channel:1}` closed channel
    // 0's open stream and acked -- the peer could not target a stream and silently
    // stopped the wrong one, while the sibling `learn_commit` guards the same
    // field with `channel_mismatch`. A stop that names the OPEN stream closes it;
    // one that names another channel is refused.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.OnConnected();
    DrainReplies(r);

    const std::string ls = "{\"v\":1,\"seq\":1,\"type\":\"learn_start\",\"channel\":0}";
    r.OnLine(ls.c_str(), ls.size());
    cap.lines.clear();

    // The WRONG channel must not close it.
    const std::string wrong = "{\"v\":1,\"seq\":2,\"type\":\"learn_stop\",\"channel\":1}";
    r.OnLine(wrong.c_str(), wrong.size());
    EXPECT_TRUE(HasType(cap, "nack")) << "stopping another channel must be refused";
    EXPECT_TRUE(HasType(cap, "nack")) << "and must name the mismatch";
    // The stream is still open, so Process keeps streaming.
    cap.lines.clear();
    r.Process();
    EXPECT_TRUE(HasType(cap, "ladder_sample"))
        << "the refused stop must NOT have closed the stream";

    // The RIGHT channel closes it.
    cap.lines.clear();
    const std::string right = "{\"v\":1,\"seq\":3,\"type\":\"learn_stop\",\"channel\":0}";
    r.OnLine(right.c_str(), right.size());
    EXPECT_TRUE(HasType(cap, "ack"));
    cap.lines.clear();
    r.Process();
    EXPECT_FALSE(HasType(cap, "ladder_sample"))
        << "the matching stop must close the stream";
}

TEST(CommandRouter, AConfigRunRefusesAFractionalLengthOrOffset) {
    // The SAME rule `NumToU32` and the codec's `ReadU32` enforce, at the last two
    // numbers that bypassed it: a byte count is an integer, and a bare
    // `static_cast<size_t>` truncates. `total_len: 500.9` was acked as 500 and
    // `offset: 9.5` satisfied the contiguity test at 9 -- the frame said one thing
    // and the device did another.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    cap.lines.clear();

    const std::string begin =
        "{\"v\":1,\"seq\":2,\"type\":\"config_begin\",\"total_len\":500.9,\"crc32\":0}";
    r.OnLine(begin.c_str(), begin.size());
    ASSERT_TRUE(HasType(cap, "nack")) << "a fractional total_len must be refused";
    EXPECT_NE(cap.lines.back().find("bad_frame"), std::string::npos);
    cap.lines.clear();

    // A valid begin, then a fractional offset: refused rather than truncated.
    SendConfigBegin(r, 3, 16, 0);
    cap.lines.clear();
    const std::string chunk =
        "{\"v\":1,\"seq\":4,\"type\":\"config_chunk\",\"offset\":9.5,\"data_b64\":\"AAAA\"}";
    r.OnLine(chunk.c_str(), chunk.size());
    ASSERT_TRUE(HasType(cap, "nack")) << "a fractional offset must be refused";
    EXPECT_NE(cap.lines.back().find("bad_frame"), std::string::npos);
}

TEST(CommandRouter, RebootSelectsTheDestinationNamedAndRefusesAnyOther) {
    // Both declared targets are honoured and each reaches a DIFFERENT HAL action.
    // The frame once refused `bootloader` outright, on the belief that entering
    // the ROM download loader was a power-on/BOOT-pin event with no software path;
    // on the S3 the ROM re-checks `RTC_CNTL_FORCE_DOWNLOAD_BOOT` on every reset,
    // so the download stub IS reachable in software. This test pins the routing
    // AND the refusal, because the dangerous half is not "refused" but "acked and
    // went to the wrong destination" -- a peer that asked for a loader and got the
    // application port has been told something false.
    //
    // What this test CANNOT see, and must not be read as covering: that
    // `reboot_to_download` actually sets the force-download bit on the S3. That is
    // `EspHal`, excluded from the host build (see `check_hal_contracts.py`), and is
    // verified on hardware by `tools/dev_flash.sh`.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    r.OnConnected();
    DrainReplies(r);
    cap.lines.clear();

    // `bootloader` is acked AND routed to the download action, not the app one.
    const std::string bl =
        "{\"v\":1,\"seq\":1,\"type\":\"reboot\",\"boot_target\":\"bootloader\"}";
    r.OnLine(bl.c_str(), bl.size());
    EXPECT_TRUE(HasType(cap, "ack")) << "the bootloader target must be acked";
    EXPECT_EQ(hal.RebootToDownloadCount(), 1) << "bootloader must reach the download action";
    EXPECT_EQ(hal.RebootCount(), 0) << "bootloader must NOT be an ordinary restart";

    // `app` is the other honoured target, and reaches the ORDINARY restart.
    cap.lines.clear();
    const std::string ap = "{\"v\":1,\"seq\":2,\"type\":\"reboot\",\"boot_target\":\"app\"}";
    r.OnLine(ap.c_str(), ap.size());
    EXPECT_TRUE(HasType(cap, "ack")) << "the app target must reboot";
    EXPECT_EQ(hal.RebootCount(), 1) << "app must reach the ordinary restart";
    EXPECT_EQ(hal.RebootToDownloadCount(), 1)
        << "app must NOT be routed to the download stub -- the destinations are the point";

    // A target that names neither destination is still refused by name -- the
    // "accepted field that changes nothing" shape the routing exists to prevent.
    cap.lines.clear();
    const std::string no =
        "{\"v\":1,\"seq\":3,\"type\":\"reboot\",\"boot_target\":\"recovery\"}";
    r.OnLine(no.c_str(), no.size());
    ASSERT_TRUE(HasType(cap, "nack")) << "an unknown target must be refused";
    EXPECT_FALSE(HasType(cap, "ack"));
    EXPECT_NE(cap.lines.back().find("bad_target"), std::string::npos);
    EXPECT_EQ(hal.RebootCount(), 1) << "an unknown target must not restart anything";
    EXPECT_EQ(hal.RebootToDownloadCount(), 1) << "an unknown target must not reboot at all";
}

TEST(CommandRouter, ANumericFieldOverflowingToInfinityIsRefusedNotCast) {
    // cJSON's `parse_number` runs `strtod` and ignores `ERANGE`, so a JSON number
    // literal that overflows `double` arrives as `+inf` (`{"total_len":1e999}`
    // parses to `valuedouble == inf`; verified against the pinned 1.7.19 parser).
    // Casting `inf` to an integer type is UNDEFINED BEHAVIOUR. `HandleTestKey`'s
    // bounds test performed the cast FIRST (`static_cast<int>(mvd)` inside the
    // comparison), so `key_mv: 1e999` reached UB before the bounds were consulted;
    // the fix checks the range in a form that rejects a non-finite value before
    // any cast (`!(v >= low && v <= high)`).
    //
    // **This test is a REGRESSION GUARD, not a mutation-detector, and saying so
    // matters.** On x86-64 the pre-fix cast-first form also REJECTED `inf` -- the
    // UB happened to produce a value that failed the equality test -- so reverting
    // the fix does not fail this test (checked). What the fix removes is UB a
    // different compiler/optimisation level could exploit (it is free to assume a
    // cast is in range and delete the guard). The value of the test is that the
    // refusal is now pinned for the reachable input, so a future edit that starts
    // ACCEPTING `inf` is caught; it cannot prove the UB is gone.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);

    const std::string p = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    r.OnLine(p.c_str(), p.size());
    cap.lines.clear();

    // key_mv: the old guard cast inside the comparison itself.
    const std::string tk =
        "{\"v\":1,\"seq\":2,\"type\":\"test_key\",\"channel\":0,\"key_mv\":1e999}";
    r.OnLine(tk.c_str(), tk.size());
    ASSERT_TRUE(HasType(cap, "nack")) << "an inf key_mv must be refused";
    EXPECT_FALSE(HasType(cap, "ack"));
    cap.lines.clear();

    // total_len.
    const std::string begin =
        "{\"v\":1,\"seq\":3,\"type\":\"config_begin\",\"total_len\":1e999,\"crc32\":0}";
    r.OnLine(begin.c_str(), begin.size());
    ASSERT_TRUE(HasType(cap, "nack")) << "an inf total_len must be refused";
    cap.lines.clear();

    // offset, on a valid open run.
    SendConfigBegin(r, 4, 16, 0);
    cap.lines.clear();
    const std::string chunk =
        "{\"v\":1,\"seq\":5,\"type\":\"config_chunk\",\"offset\":1e999,\"data_b64\":\"AAAA\"}";
    r.OnLine(chunk.c_str(), chunk.size());
    EXPECT_TRUE(HasType(cap, "nack")) << "an inf offset must be refused";
}

// ---------------------------------------------------------------------------
// USB OTA frames (spec 9.3, N-14), wired to Update/OtaUsb.
//
// The gate itself is covered by test_update/OtaUsbTest.cpp; what these tests add
// is the ROUTER half -- that the frames are dispatched to it, that a gap or an
// overlap is refused rather than spliced, and that `hello` advertises `ota` only
// because this dispatch exists (see HelloAdvertisesOtaOnlyIfTheDispatcherImplementsIt).
// ---------------------------------------------------------------------------

namespace {
// The digest of a payload, through the same stream the device uses.
std::string PayloadHash(const std::string &data) {
    char hex[65];
    Sha256Hex(reinterpret_cast<const uint8_t *>(data.data()), data.size(), hex);
    return std::string(hex, 64);
}

void SendOtaBegin(CommandRouter &r, uint32_t seq, size_t size, const std::string &sha) {
    const std::string line = "{\"v\":1,\"seq\":" + std::to_string(seq) +
        ",\"type\":\"ota_begin\",\"size\":" + std::to_string(size) +
        ",\"sha256\":\"" + sha + "\"}";
    r.OnLine(line.c_str(), line.size());
}

void SendOtaChunk(CommandRouter &r, uint32_t seq, size_t off, const std::string &raw) {
    const std::string line = "{\"v\":1,\"seq\":" + std::to_string(seq) +
        ",\"type\":\"ota_chunk\",\"offset\":" + std::to_string(off) +
        ",\"data_b64\":\"" + B64(raw) + "\"}";
    r.OnLine(line.c_str(), line.size());
}
}  // namespace

TEST(CommandRouter, OtaBeginOpensTheRunAndAcks) {
    // A well-formed begin must open the run rather than nack. The digest is
    // validated up front (spec 9.3), so a legal 64-hex one is required here.
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string img(4096, 'A');
    SendOtaBegin(r, 1, img.size(), PayloadHash(img));
    ASSERT_TRUE(HasType(cap, "ack")) << "a well-formed ota_begin must ack";
    EXPECT_TRUE(OtaInProgress()) << "the run must be open for chunks to land";
    OtaAbort();
}

TEST(CommandRouter, OtaBeginWithABadHashIsNackedNotSilentlyAccepted) {
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    SendOtaBegin(r, 1, 4096, "not-a-real-hash");
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("verify_failed"), std::string::npos);
    EXPECT_FALSE(OtaInProgress());
}

TEST(CommandRouter, OtaBeginRejectsAnImageLargerThanTheSlot) {
    // Spec 9.3: "The device MUST reject an image claiming the wrong size." The
    // router refuses it before the cast, and the shared gate would refuse it
    // again -- this asserts the router's own bound is live.
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    SendOtaBegin(r, 1, kAppSlotBytes + 1, PayloadHash("x"));
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("too_large"), std::string::npos);
    EXPECT_FALSE(OtaInProgress());
}

TEST(CommandRouter, OtaChunkAtTheWrongOffsetIsRefusedAsAGap) {
    // Spec 9.3 requires a gap or an overlap be rejected. A chunk whose offset is
    // not the next expected byte would splice two images into one whose digest
    // fails at the end for an unrelated-looking reason; refusing it NOW names the
    // real cause. The run is aborted, not left open to keep accepting chunks.
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string img(4096, 'B');
    SendOtaBegin(r, 1, img.size(), PayloadHash(img));
    cap.lines.clear();

    SendOtaChunk(r, 2, 100, std::string(64, 'B'));   // not offset 0
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("gap"), std::string::npos);
    EXPECT_FALSE(OtaInProgress()) << "a spliced run must close, not keep accepting";
}

TEST(CommandRouter, OtaChunkWithNoRunOpenIsNacked) {
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    SendOtaChunk(r, 1, 0, std::string(64, 'C'));
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("no_run"), std::string::npos);
}

TEST(CommandRouter, OtaChunkWithoutAnOffsetIsRefusedAndClosesTheRun) {
    // A malformed frame must not leave a run open that later chunks keep landing
    // in, whichever layer sees the malformation.
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string img(1024, 'D');
    SendOtaBegin(r, 1, img.size(), PayloadHash(img));
    cap.lines.clear();

    const std::string bad = "{\"v\":1,\"seq\":2,\"type\":\"ota_chunk\",\"data_b64\":\"AAAA\"}";
    r.OnLine(bad.c_str(), bad.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("bad_frame"), std::string::npos);
    EXPECT_FALSE(OtaInProgress());
}

TEST(CommandRouter, OtaEndWithoutARunIsNacked) {
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string end = "{\"v\":1,\"seq\":1,\"type\":\"ota_end\"}";
    r.OnLine(end.c_str(), end.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("no_run"), std::string::npos);
}

TEST(CommandRouter, OtaEndOnAHostBuildReportsNotSupportedRatherThanInstalling) {
    // The full happy path, ending where a host build must: the verification gate
    // is reached and PASSED, and the commit still reports that this build cannot
    // install anything. `ok:true` with `result:not_supported` is the honest
    // answer -- claiming an install that never happened is the defect this
    // asserts against.
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string img(2048, 'E');
    SendOtaBegin(r, 1, img.size(), PayloadHash(img));
    cap.lines.clear();
    uint32_t seq = 2;
    for (size_t off = 0; off < img.size(); off += kConfigWireChunkBytes) {
        const size_t n = (img.size() - off < kConfigWireChunkBytes)
                             ? (img.size() - off) : kConfigWireChunkBytes;
        SendOtaChunk(r, seq++, off, img.substr(off, n));
        ASSERT_TRUE(HasType(cap, "ack")) << "each wire-sized chunk must land";
        cap.lines.clear();
    }

    const std::string end = "{\"v\":1,\"seq\":4,\"type\":\"ota_end\"}";
    r.OnLine(end.c_str(), end.size());
    ASSERT_TRUE(HasType(cap, "ack"));
    EXPECT_NE(cap.lines.back().find("not_supported"), std::string::npos)
        << "a host build must say it cannot install, not claim success";
    EXPECT_FALSE(OtaInProgress());
}

TEST(CommandRouter, OtaEndRefusesATruncatedImage) {
    // FR-36: a truncated image is refused at the end, and the run is closed.
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string img(4096, 'F');
    SendOtaBegin(r, 1, img.size(), PayloadHash(img));
    cap.lines.clear();
    SendOtaChunk(r, 2, 0, img.substr(0, 2048));   // half the declared size
    cap.lines.clear();
    const std::string end = "{\"v\":1,\"seq\":3,\"type\":\"ota_end\"}";
    r.OnLine(end.c_str(), end.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("verify_failed"), std::string::npos);
    EXPECT_FALSE(OtaInProgress());
}

TEST(CommandRouter, OtaBeginWithAnInfSizeIsRefusedNotCast) {
    // The same `1e999` -> +inf hazard the config handlers document: cJSON ignores
    // ERANGE, so `size` can arrive non-finite, and the router's cast must not be
    // reached with it. Refused, run not opened.
    OtaAbort();
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string line =
        "{\"v\":1,\"seq\":1,\"type\":\"ota_begin\",\"size\":1e999,"
        "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
    r.OnLine(line.c_str(), line.size());
    ASSERT_TRUE(HasType(cap, "nack")) << "an inf size must be refused";
    EXPECT_FALSE(OtaInProgress());
}
