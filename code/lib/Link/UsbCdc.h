#pragma once

#include <stddef.h>
#include <stdint.h>

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

    // The RX entry point. The TinyUSB read callback calls this with whatever
    // arrived, and it feeds `Ndjson`, which invokes the sink once per COMPLETE
    // frame. A partial line never reaches the sink.
    void FeedBytes(const uint8_t *data, size_t len);

    size_t PendingTx() const { return tx_len_; }
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

    NdjsonReader reader_;
    bool         connected_ = false;
    uint32_t     dropped_ = 0;
};
