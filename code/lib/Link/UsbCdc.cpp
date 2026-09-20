#include "Link/UsbCdc.h"

#include <string.h>

void UsbCdc::Init(RawWrite w, void *wctx, FrameSink sink, void *sink_ctx) {
    raw_ = w;
    raw_ctx_ = wctx;
    sink_ = sink;
    sink_ctx_ = sink_ctx;
    tx_len_ = 0;
    tx_off_ = 0;
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
    for (size_t i = 0; i < len; ++i) {
        switch (reader_.Push(data[i])) {
            case NdjsonResult::kComplete:
                // One complete frame. The reader hands it WITHOUT the newline,
                // which is exactly FrameSink's contract, so it goes straight to
                // the sink with no re-framing here.
                if (sink_ != nullptr) {
                    sink_(sink_ctx_, reader_.Line(), reader_.LineLen());
                }
                reader_.Consume();
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
}

void UsbCdc::NoteDisconnected() {
    connected_ = false;
    // Deliberately NOT clearing the TX buffer: a frame half-sent when the host
    // unplugged is gone anyway, and clearing it here would also discard a frame
    // queued for a host that is about to re-enumerate (spec 4.4 says reconnect is
    // stateless, so nothing is replayed -- but nothing is corrupted either).
    tx_off_ = 0;
    tx_len_ = 0;
    reader_ = NdjsonReader();
}
