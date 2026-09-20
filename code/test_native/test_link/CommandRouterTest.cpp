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
