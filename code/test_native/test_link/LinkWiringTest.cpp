#include "Link/LinkWiring.h"

#include "Config/ConfigStore.h"
#include "Link/CommandRouter.h"
#include "Link/Ndjson.h"
#include "MockHAL.h"

#include "cJSON.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

/*
 * The router<->transport wiring itself, which no other suite covered.
 *
 * Every existing link test attaches its OWN recording sink to the router, so it
 * exercises the router in isolation and never sees how the two directions are
 * bound. The binding lived in `UsbLink.cpp`, a TinyUSB-only TU with no host
 * coverage, which is exactly how the two directions came to be wired to the same
 * thunk: `SetSink(&RouterSinkThunk, &router)` and `Init(..., &RouterSinkThunk,
 * &router)`. Both are `FrameSink`s, so it compiled -- and it made every emitted
 * frame re-enter `OnLine` as if the app had sent it (unbounded recursion) while
 * nothing ever reached the TX buffer (the app saw a silent device).
 *
 * These tests drive `LinkBind` -- the production binding -- and would fail on
 * that swap.
 */
namespace {

// A raw write that accepts everything, standing in for the USB FIFO.
struct Sink {
    std::string got;
    int calls = 0;
    static size_t Write(void *ctx, const uint8_t *data, size_t len) {
        Sink *s = static_cast<Sink *>(ctx);
        ++s->calls;
        s->got.append(reinterpret_cast<const char *>(data), len);
        return len;
    }
};

std::vector<std::string> Lines(const std::string &blob) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < blob.size()) {
        const size_t nl = blob.find('\n', start);
        if (nl == std::string::npos) break;
        out.push_back(blob.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

bool HasType(const std::vector<std::string> &ls, const char *type) {
    const std::string needle = std::string("\"type\":\"") + type + "\"";
    for (const auto &l : ls) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

struct Fixture {
    MockHal hal;
    ConfigStore store{&hal.InterfaceRef()};
    UsbCdc cdc;
    CommandRouter router{&hal.InterfaceRef(), nullptr, &store};
    Sink sink;

    Fixture() { LinkBind(router, cdc, &Sink::Write, &sink); }

    std::vector<std::string> FrameTheHostSent(const std::string &cmd) {
        // A frame the HOST writes, terminator included, goes into the transport.
        const std::string wire = cmd + "\n";
        cdc.FeedBytes(reinterpret_cast<const uint8_t *>(wire.data()), wire.size());
        // The reply is queued by the poll loop, not by the inbound call: the
        // router emits at most one deferred frame per Process() (a chunked config
        // reply is a run), so drain it the way `UsbLinkService` does.
        for (int i = 0; i < 400; ++i) {
            router.Process();
            cdc.ServiceTx();
        }
        return Lines(sink.got);
    }
};

}  // namespace

TEST(LinkWiring, TheHelloOnConnectReachesTheHostInsteadOfTheParser) {
    // The exact defect: `hello` was emitted and immediately fed back into
    // `OnLine`, which answered it with a `bad_frame` nack, which recursed. It must
    // instead land in the TX buffer.
    Fixture f;
    f.router.OnConnected();
    // Drain: the router queues one frame per Process() call (a large config reply
    // is chunked), which is how the poll loop drives it.
    for (int i = 0; i < 400; ++i) {
        f.router.Process();
        f.cdc.ServiceTx();
    }
    const auto ls = Lines(f.sink.got);
    ASSERT_FALSE(ls.empty()) << "no frame ever reached the transport";
    EXPECT_EQ(ls[0].rfind("{\"v\":1,\"seq\":", 0), 0u) << ls[0];
    EXPECT_NE(ls[0].find("\"type\":\"hello\""), std::string::npos) << ls[0];
    // The signature of the defect: the hello is re-parsed and nacked.
    EXPECT_FALSE(HasType(ls, "nack")) << "an outbound frame was re-ingested as inbound";
}

TEST(LinkWiring, EveryEmittedFrameIsSentOnceAndNeverFedBackIn) {
    Fixture f;
    f.router.OnConnected();
    for (int i = 0; i < 400; ++i) {
        f.router.Process();
        f.cdc.ServiceTx();
    }
    const auto ls = Lines(f.sink.got);
    // No frame is emitted twice, which is what the recursion produced: the same
    // `hello` reappearing would mean the parser re-ran on it.
    int hellos = 0;
    for (const auto &l : ls) {
        if (l.find("\"type\":\"hello\"") != std::string::npos) ++hellos;
    }
    EXPECT_EQ(hellos, 1) << "hello was emitted more than once -- the outbound sink "
                            "is feeding frames back into the parser";
}

TEST(LinkWiring, AnInboundCommandStillReachesTheRouterAndIsAnswered) {
    // The other direction must survive the fix: a host frame goes in, an ack
    // comes out through the transport. If `Init` were bound to the outbound thunk
    // this would produce nothing.
    Fixture f;
    const auto ls = f.FrameTheHostSent(
        "{\"v\":1,\"seq\":1,\"type\":\"config_get\"}");
    EXPECT_TRUE(HasType(ls, "config_begin")) << "the inbound path is not wired to the router";
}

TEST(LinkWiring, AFrameTheHostSentIsNotSentBackToTheHost) {
    // A round-trip guard: the host's own bytes must not be echoed. If the outbound
    // sink were bound to the transport's reader, an inbound line would reappear.
    Fixture f;
    const auto ls = f.FrameTheHostSent(
        "{\"v\":1,\"seq\":1,\"type\":\"status_get\"}");
    for (const auto &l : ls) {
        EXPECT_EQ(l.find("\"type\":\"status_get\""), std::string::npos)
            << "the host's own frame was echoed back";
    }
}

TEST(LinkWiring, TheHelloFrameParsesAsJsonEvenWhenTheVersionMacroIsEmpty) {
    // The `hello` body is built with `snprintf(..., "\"fw_version\":\"%s\",...")`.
    // That form is only valid while the macro expands to an UNQUOTED string, and
    // it silently mangles the frame otherwise -- both the build's
    // `-D ...=\"1.2.3-ci\"` form (which used to be re-stringified into
    // `""1.2.3-ci""`) and the empty native form produced `"fw_version":""""` /
    // `""0.0.0-ci""`, which no JSON parser accepts. The app's first frame is the
    // version negotiation, so this is the frame that must never be malformed.
    Fixture f;
    f.router.OnConnected();
    f.router.Process();
    f.cdc.ServiceTx();
    const auto ls = Lines(f.sink.got);
    ASSERT_FALSE(ls.empty());
    // Parse the emitted line for real. A substring check would pass on a body
    // that merely CONTAINS the right bytes; only a parse rejects the doubled
    // quote, the empty value, and the unbalanced pair alike.
    const std::string doc = "[" + ls[0] + "]";
    cJSON *root = cJSON_Parse(doc.c_str());
    ASSERT_NE(root, nullptr) << "the hello frame is not valid JSON: " << ls[0];
    cJSON *frame = cJSON_GetArrayItem(root, 0);
    ASSERT_TRUE(cJSON_IsObject(frame)) << ls[0];
    cJSON *fw = cJSON_GetObjectItemCaseSensitive(frame, "fw_version");
    ASSERT_TRUE(cJSON_IsString(fw)) << ls[0];
    EXPECT_GT(strlen(fw->valuestring), 0u) << "an empty fw_version is not a version";
    cJSON_Delete(root);
}

TEST(LinkWiring, TheRebootAckReachesTheTransportBeforeTheReset) {
    // The ack must be ON THE WIRE, not merely queued, when `reboot()` is called:
    // the poll loop's ServiceTx never runs again, so a queued-only ack is lost
    // with the reset and the app cannot distinguish a successful reboot from a
    // dropped link. The capture sink records at EMIT time, so this must go
    // through the real UsbCdc into a raw-write sink, with NO ServiceTx call of
    // its own -- exactly like the device.
    Fixture f;
    f.router.OnConnected();
    f.sink.got.clear();
    const std::string rb = "{\"v\":1,\"seq\":7,\"type\":\"reboot\",\"boot_target\":\"app\"}";
    const std::string wire = rb + "\n";
    f.cdc.FeedBytes(reinterpret_cast<const uint8_t *>(wire.data()), wire.size());
    ASSERT_EQ(f.hal.RebootCount(), 1);
    EXPECT_NE(f.sink.got.find("\"type\":\"ack\""), std::string::npos)
        << "the reboot ack must be flushed before reboot(), not queued into the reset";
}
