#include "Link/UsbCdc.h"

#include "Link/Ndjson.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {
// A raw write that accepts at most `chunk` bytes per call, like a USB FIFO.
struct Fifo {
    size_t chunk = 7;
    std::string got;
    // How many times the raw write was called, so "retried" is observable.
    int calls = 0;
    // When true, the write always reports 0 accepted, which is what a full FIFO
    // does. Used to prove ServiceTx returns rather than spinning.
    bool always_full = false;

    static size_t Write(void *ctx, const uint8_t *data, size_t len) {
        Fifo *f = static_cast<Fifo *>(ctx);
        ++f->calls;
        if (f->always_full) return 0;
        const size_t n = (len < f->chunk) ? len : f->chunk;
        f->got.append(reinterpret_cast<const char *>(data), n);
        return n;
    }
};

struct Sink {
    std::vector<std::string> lines;
    static void OnLine(void *ctx, const char *line, size_t len) {
        static_cast<Sink *>(ctx)->lines.emplace_back(line, len);
    }
};
}  // namespace

TEST(UsbCdc, AShortWriteIsRetriedUntilTheWholeFrameIsSentExactlyOnce) {
    // A 900-byte frame into a 7-byte FIFO takes many writes. The failure this
    // guards is silent: a dropped frame looks like "the app missed a key", and a
    // duplicated one looks like a double press.
    Fifo f; Sink s; UsbCdc u;
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    const std::string line(900, 'x');
    ASSERT_TRUE(u.Send(line.c_str(), line.size()));
    for (int i = 0; i < 500 && u.PendingTx() > 0; ++i) u.ServiceTx();

    EXPECT_EQ(u.PendingTx(), 0u);
    EXPECT_EQ(f.got, line + "\n") << "the frame must arrive whole, once, newline-terminated";
    EXPECT_EQ(f.got.size(), line.size() + 1);
    EXPECT_GT(f.calls, 1) << "a 7-byte FIFO cannot take 901 bytes in one call";
    EXPECT_EQ(u.DroppedFrames(), 0u);
}

TEST(UsbCdc, TheFramesNewlineIsAddedByTheTransportNotTheCaller) {
    // FrameSink's contract: a sink gets a frame WITHOUT its newline (Shared
    // contract). The transport is therefore the one component that owns the
    // newline, and this pins that the two ends cannot double or omit it.
    Fifo f; Sink s; UsbCdc u;
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    const std::string line = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    ASSERT_TRUE(u.Send(line.c_str(), line.size()));
    while (u.PendingTx() > 0) u.ServiceTx();
    ASSERT_EQ(f.got.size(), line.size() + 1);
    EXPECT_EQ(f.got.back(), '\n');
    EXPECT_EQ(f.got.find('\n'), line.size()) << "exactly one newline, at the end";
}

TEST(UsbCdc, BytesArrivingInFragmentsBecomeOneWholeFrameAtTheSink) {
    // The RX side is where a real USB link differs from a test: bytes arrive a
    // few at a time. A partial line must not reach the sink, and a completed one
    // must arrive exactly once.
    Fifo f; Sink s; UsbCdc u;
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    const std::string frame = "{\"v\":1,\"seq\":2,\"type\":\"status\"}\n";
    for (char c : frame) u.FeedBytes(reinterpret_cast<const uint8_t *>(&c), 1);
    ASSERT_EQ(s.lines.size(), 1u) << "fragments must assemble into one frame";
    EXPECT_EQ(s.lines[0], "{\"v\":1,\"seq\":2,\"type\":\"status\"}");
    EXPECT_EQ(s.lines[0].find('\n'), std::string::npos) << "the sink must not see the newline";
}

TEST(UsbCdc, TwoCompleteFramesInOneReadArriveAsTwoFrames) {
    // A single `FeedBytes` call can carry more than one frame -- nothing in USB
    // prevents it. Calling the sink once per LINE rather than once per read is
    // the difference between working and losing every other command.
    Fifo f; Sink s; UsbCdc u;
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    const std::string two = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}\n{\"v\":1,\"seq\":2,\"type\":\"ping\"}\n";
    u.FeedBytes(reinterpret_cast<const uint8_t *>(two.data()), two.size());
    ASSERT_EQ(s.lines.size(), 2u);
    EXPECT_NE(s.lines[0].find("\"seq\":1"), std::string::npos);
    EXPECT_NE(s.lines[1].find("\"seq\":2"), std::string::npos);
}

TEST(UsbCdc, AnOversizedLineDoesNotBecomeAFrameAndTheNextOneStillDoes) {
    // The reader latches kTooLong and discards the rest of the bad line. The
    // requirement is that the link RECOVERS: the following valid frame must
    // still arrive, or one bad peer frame kills the session permanently.
    Fifo f; Sink s; UsbCdc u;
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    std::string bad(kNdjsonMaxFrame + 200, 'x');
    bad += "\n";
    u.FeedBytes(reinterpret_cast<const uint8_t *>(bad.data()), bad.size());
    EXPECT_TRUE(s.lines.empty()) << "an overrun line is not a frame";

    const std::string good = "{\"v\":1,\"seq\":9,\"type\":\"ping\"}\n";
    u.FeedBytes(reinterpret_cast<const uint8_t *>(good.data()), good.size());
    ASSERT_EQ(s.lines.size(), 1u) << "the link must resynchronize after an overrun";
    EXPECT_NE(s.lines[0].find("\"seq\":9"), std::string::npos);
}

TEST(UsbCdc, AFifoThatAcceptsNothingLeavesTheFrameBufferedRatherThanSpinning) {
    // The real full-FIFO case: the raw write reports 0 accepted. ServiceTx must
    // RETURN (leaving the bytes buffered) rather than loop forever, because it is
    // called from the poll loop and a spin there would make every key press late.
    Fifo f; Sink s; UsbCdc u;
    f.always_full = true;
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    const std::string line = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    ASSERT_TRUE(u.Send(line.c_str(), line.size()));
    for (int i = 0; i < 10; ++i) u.ServiceTx();
    EXPECT_GT(u.PendingTx(), 0u) << "the frame is still buffered, not lost";
    EXPECT_EQ(f.got.size(), 0u);

    // And once the FIFO drains, the frame goes out whole.
    f.always_full = false;
    while (u.PendingTx() > 0 && f.calls < 1000) u.ServiceTx();
    EXPECT_EQ(f.got, line + "\n");
}

TEST(UsbCdc, TheBufferHoldsTwoFullFramesSoABurstDoesNotDropTheSecond) {
    // The stated reason the buffer is 2x rather than 1x: the app may be mid-write
    // when the firmware emits the next frame.
    Fifo f; Sink s; UsbCdc u;
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    const std::string big(kNdjsonMaxFrame - 1, 'y');   // + newline == one maximum frame
    EXPECT_TRUE(u.Send(big.c_str(), big.size())) << "the first frame must fit";
    EXPECT_TRUE(u.Send(big.c_str(), big.size())) << "and the second, without draining";
    EXPECT_EQ(u.PendingTx(), big.size() + 1 + big.size() + 1);
    EXPECT_EQ(u.DroppedFrames(), 0u);

    // The third does not fit, and is DROPPED AND COUNTED rather than partially
    // queued -- a truncated frame desynchronizes the peer.
    EXPECT_FALSE(u.Send(big.c_str(), big.size()));
    EXPECT_EQ(u.DroppedFrames(), 1u);
    EXPECT_EQ(u.PendingTx(), 2 * (big.size() + 1)) << "the buffer was not corrupted by the refusal";

    while (u.PendingTx() > 0) u.ServiceTx();
    EXPECT_EQ(f.got, big + "\n" + big + "\n");
}

TEST(UsbCdc, RepeatedSendsWithoutDrainingDoNotOverrunTheBuffer) {
    // The compaction path: a caller that sends many small frames and rarely
    // drains must not march the offset to the end of a fixed buffer.
    Fifo f; Sink s; UsbCdc u;
    f.chunk = 1;   // force a drain between sends
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    const std::string line = "{\"v\":1,\"seq\":1,\"type\":\"ping\"}";
    for (int i = 0; i < 200; ++i) {
        u.Send(line.c_str(), line.size());
        u.ServiceTx();
    }
    EXPECT_EQ(u.DroppedFrames(), 0u) << "compaction must keep the buffer usable";
    EXPECT_EQ(f.got.size(), 200u * (line.size() + 1));
}

TEST(UsbCdc, AFrameTooLargeForAnEmptyBufferIsRefusedRatherThanTruncated) {
    Fifo f; Sink s; UsbCdc u;
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    const std::string huge(UsbCdc::kTxCapacity, 'z');
    EXPECT_FALSE(u.Send(huge.c_str(), huge.size()));
    EXPECT_EQ(u.PendingTx(), 0u) << "nothing partial was queued";
    EXPECT_EQ(u.DroppedFrames(), 1u);
}

TEST(UsbCdc, DisconnectResetsTheAssemblerSoAHalfFrameCannotLeakIntoTheNextSession) {
    // A half-frame left in the reader when the host unplugged would be prepended
    // to the first frame after re-enumeration, producing one garbage command.
    Fifo f; Sink s; UsbCdc u;
    u.Init(&Fifo::Write, &f, &Sink::OnLine, &s);
    u.NoteConnected();
    const std::string half = "{\"v\":1,\"seq\":1,";
    u.FeedBytes(reinterpret_cast<const uint8_t *>(half.data()), half.size());
    EXPECT_TRUE(s.lines.empty());

    u.NoteDisconnected();
    EXPECT_FALSE(u.IsConnected());
    const std::string whole = "{\"v\":1,\"seq\":2,\"type\":\"ping\"}\n";
    u.FeedBytes(reinterpret_cast<const uint8_t *>(whole.data()), whole.size());
    ASSERT_EQ(s.lines.size(), 1u);
    EXPECT_EQ(s.lines[0], "{\"v\":1,\"seq\":2,\"type\":\"ping\"}")
        << "the pre-disconnect fragment must not be prepended";
}
