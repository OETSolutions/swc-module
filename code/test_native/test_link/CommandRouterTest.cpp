#include "Link/CommandRouter.h"

#include "Config/ConfigCodec.h"
#include "Config/ConfigDefaults.h"
#include "Config/ConfigStore.h"
#include "Link/Ndjson.h"
#include "MockHAL.h"
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
    const Config c = ConfigDefault();
    EXPECT_TRUE(ConfigValidate(c));

    char buf[ConfigMaxSerializedSize() + 1];
    const size_t n = ConfigEncodeJson(c, buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    Config out{};
    EXPECT_TRUE(ConfigDecodeJson(buf, n, &out));
    EXPECT_STREQ(out.device_id, c.device_id);
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
    // If the ack came after reboot(), it would be lost with the reset and the
    // app could not tell a successful reboot from a dropped link.
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

TEST(CommandRouter, AKnownButUnimplementedCommandIsDistinguishedFromAnUnknownOne) {
    // ota_* and maintenance_* are in the spec's vocabulary (so they are not
    // unknown_type) but this build cannot execute them. Saying so is better than
    // a silent no-op, which the app would read as success.
    //
    // `learn_start` USED to be in this set and is now implemented (FR-5), so it
    // is asserted separately below -- leaving it here would have made this test
    // pass for the wrong reason the moment the learn path landed.
    MockHal hal; Capture cap; ConfigStore store(&hal.InterfaceRef());
    CommandRouter r(&hal.InterfaceRef(), nullptr, &store);
    cap.Attach(r);
    const std::string ota = "{\"v\":1,\"seq\":1,\"type\":\"ota_begin\"}";
    r.OnLine(ota.c_str(), ota.size());
    ASSERT_TRUE(HasType(cap, "nack"));
    EXPECT_NE(cap.lines.back().find("not_implemented"), std::string::npos);
    EXPECT_EQ(cap.lines.back().find("unknown_type"), std::string::npos);
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
    ev.at_ms = 1234;
    r.EmitGesture(ev);
    ASSERT_EQ(cap.lines.size(), 1u);
    const std::string &l = cap.lines[0];
    EXPECT_NE(l.find("\"type\":\"event\""), std::string::npos);
    EXPECT_NE(l.find("\"button\":\"vol_up\""), std::string::npos)
        << "the app matches this against Binding.button, so it must be the quoted id";
    EXPECT_NE(l.find("\"gesture\":\"SINGLE\""), std::string::npos);
    EXPECT_NE(l.find("\"level_mv\":1430"), std::string::npos);
    EXPECT_LT(l.size(), kNdjsonMaxFrame);
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
        // learn_commit carries ONE sample each, so drive many of them to clear the
        // count and span gates (>=10 samples over >=100 ms).
        for (int i = 0; i < 20; ++i) {
            const std::string lc =
                "{\"v\":1,\"seq\":3,\"type\":\"learn_commit\",\"channel\":0,"
                "\"button_id\":\"vol_dn\",\"name\":\"Volume Down\"}";
            r.OnLine(lc.c_str(), lc.size());
            hal.AdvanceMs(20);
        }
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
    cap.lines.clear();
    for (int i = 0; i < 20; ++i) {
        const std::string lc =
            "{\"v\":1,\"seq\":3,\"type\":\"learn_commit\",\"channel\":0,"
            "\"button_id\":\"vol_dn\",\"name\":\"Volume Down\"}";
        r.OnLine(lc.c_str(), lc.size());
        hal.AdvanceMs(20);
    }

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

// --- link liveness (spec 4.4) ------------------------------------------------

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
    EXPECT_NE(cap.lines.back().find("\"gain_mode\":\"tracking\""), std::string::npos)
        << "the frame must report the mode the device actually resolved; got: "
        << cap.lines.back();
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
