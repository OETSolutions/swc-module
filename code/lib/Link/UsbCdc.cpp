#include "Link/UsbCdc.h"

#include <string.h>

void UsbCdc::Init(RawWrite w, void *wctx, FrameSink sink, void *sink_ctx) {
    raw_ = w;
    raw_ctx_ = wctx;
    sink_ = sink;
    sink_ctx_ = sink_ctx;
    tx_len_ = 0;
    tx_off_ = 0;
    rx_head_.store(0, std::memory_order_relaxed);
    rx_tail_.store(0, std::memory_order_relaxed);
    rx_epoch_.store(0, std::memory_order_relaxed);
    rx_overflows_ = 0;
    reader_ = NdjsonReader();
    dropped_ = 0;
}

bool UsbCdc::Send(const char *line, size_t len) {
    if (line == nullptr) return false;
    // +1 for the newline the transport owns. A frame that cannot fit even in an
    // EMPTY buffer is refused rather than partially queued: a truncated frame
    // desynchronizes the peer for every frame after it.
    if (len + 1 > kTxCapacity) {
        ++dropped_;
        return false;
    }

    // Compact when the drained prefix is getting large, so repeated sends do not
    // march `tx_off_` to the end of the buffer. Done here rather than in
    // ServiceTx so a caller driving only Send/ServiceTx cannot starve it.
    if (tx_off_ > 0 && (tx_off_ == tx_len_ || tx_off_ > kTxCapacity / 2)) {
        const size_t live = tx_len_ - tx_off_;
        if (live > 0) memmove(tx_, tx_ + tx_off_, live);
        tx_len_ = live;
        tx_off_ = 0;
    }

    if (tx_len_ + len + 1 > kTxCapacity) {
        // The buffer is full because the app is not draining. Dropping is the
        // only safe option -- blocking would stall the poll loop, and a stall
        // makes a key press late rather than merely unreported.
        ++dropped_;
        return false;
    }

    memcpy(tx_ + tx_len_, line, len);
    tx_len_ += len;
    tx_[tx_len_++] = '\n';
    return true;
}

void UsbCdc::ServiceTx() {
    if (raw_ == nullptr) return;
    while (tx_off_ < tx_len_) {
        const size_t want = tx_len_ - tx_off_;
        const size_t wrote = raw_(raw_ctx_, tx_ + tx_off_, want);
        if (wrote == 0) {
            // The FIFO is full. Leave the rest buffered and try again next call;
            // spinning here would stall the poll loop.
            return;
        }
        // Advance ONLY by what the raw write accepted. Advancing by `want`
        // instead is how bytes get silently overwritten in the FIFO.
        if (wrote > want) {
            // A raw write that claims more than it was given would corrupt the
            // accounting, so clamp it rather than trusting it.
            tx_off_ = tx_len_;
            break;
        }
        tx_off_ += wrote;
    }
    if (tx_off_ == tx_len_) {
        tx_off_ = 0;
        tx_len_ = 0;
    }
}

void UsbCdc::FeedBytes(const uint8_t *data, size_t len) {
    if (data == nullptr && len != 0) return;

    // PRODUCER side of the SPSC ring, on the TinyUSB task. Only `rx_head_` is
    // written here; `rx_tail_` is read with acquire ordering so a position the
    // consumer has already published is never reused before it is done with it.
    //
    // The epoch is read ONCE here and re-checked at the end: a disconnect landing
    // while this call is in flight must discard these bytes rather than have them
    // published into the next session.
    //
    // Deliberately NOT gated on the connection flag. The router treats a frame from
    // the peer as proof a peer exists (`OnLine` arms the status clock itself, so a
    // device whose DTR event was never serviced still works), and the app's first
    // frame can legitimately arrive in the same poll tick as the DTR assertion --
    // gating here would silently drop it.
    const uint32_t epoch = rx_epoch_.load(std::memory_order_acquire);
    size_t head = rx_head_.load(std::memory_order_relaxed);
    const size_t tail = rx_tail_.load(std::memory_order_acquire);

    for (size_t i = 0; i < len; ++i) {
        const size_t next = (head + 1) % kRxCapacity;
        if (next == tail) {
            // The poll loop has not drained enough. Losing bytes is bad but
            // bounded and counted; blocking a USB callback on the parser would
            // stall the USB stack itself. The next newline resynchronizes the
            // parse (see DrainRx), so a dropped run costs one frame rather than
            // the session.
            ++rx_overflows_;
            break;
        }
        rx_[head] = data[i];
        head = next;
    }
    // The test seam fires HERE: the bytes are staged but the head that publishes
    // them is not yet stored. A disconnect delivered at this instant is the
    // interleaving the epoch exists to handle.
    if (rx_interleave_ != nullptr) rx_interleave_(rx_interleave_ctx_);
    // RELEASE: the bytes above must be visible to the consumer before the head
    // that publishes them is. A relaxed store here would let the poll task read a
    // head it can see before the payload it points at.
    rx_head_.store(head, std::memory_order_release);

    // **The lost-disconnect close.** `ResetSession` can run between the head
    // read above and this store -- it is the other task, and the write above is
    // not atomic with respect to it. Its tail reset would then be overwritten by
    // this head, the ring would look non-empty, and a command the app sent in the
    // session that just ended would be parsed as the first frame of the next one.
    //
    // So: if the epoch moved while this call was in flight, the consumer has
    // already declared the session over and these bytes belong to it. Pull the head
    // back to the tail to discard them. A concurrent `DrainRx` either sees the
    // pre-pull head and may deliver bytes it read before the disconnect (safe: that
    // is the same task that is about to reset the reader anyway) or sees the pulled
    // head and delivers nothing.
    if (rx_epoch_.load(std::memory_order_acquire) != epoch) {
        rx_head_.store(rx_tail_.load(std::memory_order_acquire), std::memory_order_release);
    }
}

size_t UsbCdc::DrainRx() {
    // CONSUMER side, on the poll task. This is the ONE place a frame is parsed,
    // which is what keeps every protocol handler on the same task as `Tick`.
    size_t delivered = 0;
    size_t tail = rx_tail_.load(std::memory_order_relaxed);
    const size_t head = rx_head_.load(std::memory_order_acquire);

    while (tail != head) {
        const uint8_t byte = rx_[tail];
        tail = (tail + 1) % kRxCapacity;
        switch (reader_.Push(static_cast<char>(byte))) {
            case NdjsonResult::kComplete:
                // One complete frame. The reader hands it WITHOUT the newline,
                // which is exactly FrameSink's contract, so it goes straight to
                // the sink with no re-framing here.
                if (sink_ != nullptr) {
                    sink_(sink_ctx_, reader_.Line(), reader_.LineLen());
                }
                reader_.Consume();
                ++delivered;
                break;
            case NdjsonResult::kTooLong:
                // The line exceeded the cap. Keep pushing: the reader discards
                // the rest of the overrun line on its own and resynchronizes at
                // the next newline, which is the only way to recover a boundary
                // that has already been lost.
                break;
            default:
                break;
        }
    }
    // RELEASE so the producer can safely reuse the space behind `tail`.
    rx_tail_.store(tail, std::memory_order_release);
    return delivered;
}

size_t UsbCdc::PendingRx() const {
    const size_t head = rx_head_.load(std::memory_order_acquire);
    const size_t tail = rx_tail_.load(std::memory_order_acquire);
    return (head >= tail) ? (head - tail) : (kRxCapacity - tail + head);
}

void UsbCdc::ResetSession() {
    // Drop BOTH the pending TX bytes and the RX assembler. Spec 4.4 makes
    // reconnect stateless: nothing is replayed and nothing carries over. The TX
    // bytes must go because a frame half-written when the host unplugged is
    // indistinguishable from a whole one still queued -- `tx_off_` records what
    // was already accepted by the FIFO, not what the host consumed -- so
    // resuming it would deliver a frame's TAIL to a session that never saw its
    // head. The reader is reset for the same reason in the other direction: a
    // half-assembled RX line belongs to the session that ended.
    tx_off_ = 0;
    tx_len_ = 0;
    reader_ = NdjsonReader();
    // The staged bytes belong to the ended session too. Dropping them is the
    // correct half of "nothing is replayed": a partial command left in the ring
    // would be parsed at the top of the next session as if the app had sent it.
    //
    // Only the CONSUMER index moves -- the producer is the other task and may be
    // mid-write, so writing `rx_head_` here would race it. Advancing the tail would
    // be enough for the bytes already staged, but NOT for bytes a producer is in
    // flight on: those publish their head AFTER this reset, so the ring looks
    // non-empty again and an old command is delivered into the new session.
    //
    // The epoch is what covers that window, and the ORDER is what makes it
    // correct: bump it BEFORE reading the head. The leaking interleaving with the
    // opposite order is: consumer reads the head, producer stores a later head,
    // consumer stores the tail from its earlier read (so the producer's bytes
    // survive), consumer bumps. The producer re-reads the epoch, sees no change,
    // and does not pull -- and its bytes are delivered into the new session.
    // (A reasoned invariant rather than a unit-tested one: the seam in
    // `FeedBytes` fires before the head store, so a test that disconnects there
    // cannot separate the bump from the tail reset to observe the difference.)
    rx_epoch_.fetch_add(1, std::memory_order_acq_rel);
    rx_tail_.store(rx_head_.load(std::memory_order_acquire), std::memory_order_release);
}
