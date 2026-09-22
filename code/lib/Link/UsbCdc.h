#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <string.h>

#include "Link/Ndjson.h"
#include "Link/SwcContract.h"

/*
 * The USB CDC app link (spec 4.1).
 *
 * **Two USB peripherals, two jobs.** The ROM **USB-Serial-JTAG** carries the
 * console and JTAG; a **TinyUSB CDC OTG** instance carries the app protocol.
 * Spec 4.1 calls it a hard requirement that the console never lands on the CDC
 * port, because a debug `printf` leaking into the app port is the classic failure
 * when one CDC serves both -- and it surfaces as the app parsing garbage.
 *
 * **What is host-testable, stated honestly.** Enumeration, the TinyUSB callbacks
 * and re-enumeration are device-only, and their coverage is `test/test_hw`. What
 * IS testable here is the transport's one real piece of logic: the **TX byte
 * buffer**. A Full-Speed FIFO is smaller than a maximum frame, so a short write
 * must be retried and delivered EXACTLY ONCE -- never dropped (the app "randomly
 * missed a key") and never duplicated (it "double pressed").
 */
class UsbCdc {
public:
    // The byte-level write, injected so the buffering is testable. On device it
    // is `tinyusb_cdcacm_write_queue` + `tinyusb_cdcacm_write_flush`, which
    // returns the number of bytes ACCEPTED, not the number sent.
    using RawWrite = size_t (*)(void *ctx, const uint8_t *data, size_t len);

    // Two maximum frames, not one. The app may be mid-write when the firmware
    // emits the next frame; a single-frame buffer would make the second `Send`
    // either block the poll loop or drop the frame, and over a Full-Speed link a
    // drop is invisible to the app.
    static constexpr size_t kTxCapacity = 2 * kNdjsonMaxFrame;

    // The RX staging ring: bytes the TinyUSB callback has taken from the FIFO but
    // the poll task has not yet parsed.
    //
    // **Why bytes are staged here at all, rather than parsed in the callback.**
    // The callback runs on the **TinyUSB task** (priority 5) and the poll loop on
    // **app_main** (priority 1), both pinned to core 0 -- so the callback preempts
    // the poll loop at any instruction. Parsing in the callback therefore ran the
    // whole protocol on the TinyUSB task while `SystemOrchestrator::Tick` mutated
    // the SAME state on app_main: `config_end`/`config_patch`/`learn_commit`
    // reassigning `config_` and the classifiers, `identify` and `maintenance_`
    // entries, `test_key` driving a channel, all racing a tick that was reading
    // them. Nothing in the firmware takes a lock, so two tasks on one 8,912-byte
    // `Config` is a torn read at best and a wrong classification at worst -- on the
    // path that decides what voltage reaches the head unit.
    //
    // Staging the raw bytes makes the split explicit and keeps every handler on
    // ONE task: the callback only copies bytes (no allocation, no parsing, no
    // state), and the poll loop drains and parses them in its own context.
    //
    // Sized well above `CONFIG_TINYUSB_CDC_RX_BUFSIZE` (1024) so a burst that
    // arrived between two poll ticks cannot overflow it; the callback drains the
    // whole FIFO per invocation, and the poll loop runs every 10 ms.
    static constexpr size_t kRxCapacity = 4096;

    void Init(RawWrite w, void *wctx, FrameSink sink, void *sink_ctx);

    // BUFFERS the frame. The transport owns the single trailing newline (the
    // inverse of FrameSink's contract, which delivers a frame WITHOUT one).
    // Returns false and drops the frame if the buffer is full -- a caller that
    // needs to know is a caller that has outrun the link.
    bool Send(const char *line, size_t len);

    // Drains the buffer, retrying short writes. Advancing only by the bytes the
    // raw write reports is the whole reason this is a separate call: it must be
    // safe to call repeatedly from the poll loop.
    void ServiceTx();

    /*
     * The RX entry point, called from the TinyUSB read callback. It COPIES the
     * bytes into the staging ring and returns; it does not parse and touches no
     * protocol state (see `kRxCapacity`). Safe to call from another task because
     * the ring is single-producer/single-consumer.
     */
    void FeedBytes(const uint8_t *data, size_t len);

    /*
     * Parse whatever the callback has staged, calling the sink once per COMPLETE
     * frame. Called from the POLL task only, which is what puts every protocol
     * handler on the same task as `Tick`. Returns the number of frames delivered.
     */
    size_t DrainRx();

    // Frames dropped because the RX staging ring overflowed, i.e. the poll loop
    // fell far enough behind that input was lost. Observable for the same reason
    // `DroppedFrames` is: a silent loss is the failure this class prevents.
    uint32_t RxOverflows() const { return rx_overflows_; }

    /*
     * A test seam: invoked inside `FeedBytes`, after the bytes are staged and
     * BEFORE the head is published. Null on device (one predictable branch).
     *
     * It exists because the window it opens is precisely the one the epoch closes,
     * and no test can reach it otherwise: the disconnect must land in the MIDDLE of
     * a producer call, between the producer reading its head and publishing it. A
     * test that disconnected before calling `FeedBytes` would be rescued by the
     * disconnect's own tail reset and would pass with the epoch check deleted --
     * a false negative on the one path that matters.
     */
    using InterleaveHook = void (*)(void *ctx);
    void SetRxInterleaveHookForTest(InterleaveHook hook, void *ctx) {
        rx_interleave_ = hook;
        rx_interleave_ctx_ = ctx;
    }

    size_t PendingTx() const { return tx_len_; }
    size_t PendingRx() const;
    // The session flag is the CONSUMER's (the poll task sets it from the DTR
    // transition), so it is a plain bool by design: the producer never reads it.
    // Gating `FeedBytes` on it would drop the app's first frame when that frame
    // arrives in the same poll tick as the DTR assertion -- the router treats a
    // frame as proof of a peer for exactly that reason.
    bool IsConnected() const { return connected_; }
    void NoteConnected() { connected_ = true; }
    void NoteDisconnected();

    // Frames dropped because the buffer was full. Exposed because a silent drop
    // is the failure this class exists to prevent, so it must be observable.
    uint32_t DroppedFrames() const { return dropped_; }

private:
    RawWrite  raw_ = nullptr;
    void     *raw_ctx_ = nullptr;
    FrameSink sink_ = nullptr;
    void     *sink_ctx_ = nullptr;

    // A flat buffer rather than a ring: frames are appended and drained in order,
    // and `tx_off_` only ever advances. Compaction happens when the drained
    // prefix is worth reclaiming, which keeps `Send` O(1) in the common case.
    uint8_t  tx_[kTxCapacity] = {};
    size_t   tx_len_ = 0;   // bytes currently buffered (valid region is tx_off_..tx_len_)
    size_t   tx_off_ = 0;   // bytes already handed to the raw write

    // The RX staging ring, SPSC: `rx_head_` is written only by the producer (the
    // TinyUSB callback) and `rx_tail_` only by the consumer (the poll task), so
    // neither needs a lock -- each side reads the other's index once per call and
    // a stale read only means "try again next tick".
    uint8_t  rx_[kRxCapacity] = {};
    std::atomic<size_t> rx_head_{0};   // producer: next write position
    std::atomic<size_t> rx_tail_{0};   // consumer: next read position
    // Bumped by the consumer on a session reset (disconnect). It exists because
    // `NoteDisconnected` -- which moves only the consumer-owned tail -- cannot by
    // itself discard bytes a producer is ALREADY in flight on: those bytes are
    // published after the reset, so the ring looks non-empty again and a command
    // the app sent before it closed (a `config_patch`, a `test_key`) is parsed as
    // if the new session had sent it. The producer publishes its head and THEN
    // verifies this counter, pulling the head back to the tail if it moved; the
    // consumer bumps it BEFORE reading the head. That order is what makes the race
    // impossible rather than merely unlikely -- see FeedBytes and NoteDisconnected.
    std::atomic<uint32_t> rx_epoch_{0};
    uint32_t rx_overflows_ = 0;
    // Null on device. See SetRxInterleaveHookForTest.
    InterleaveHook rx_interleave_ = nullptr;
    void          *rx_interleave_ctx_ = nullptr;

    // Owned by the poll task alone, because it is only ever touched in `DrainRx`.
    NdjsonReader reader_;
    bool         connected_ = false;
    uint32_t     dropped_ = 0;
};
