#include "Link/LinkWiring.h"

#include "Link/CommandRouter.h"

namespace {

// INBOUND: a frame the host sent -> the router's single entry point.
void RouterSinkThunk(void *ctx, const char *line, size_t len) {
    CommandRouter *router = static_cast<CommandRouter *>(ctx);
    if (router != nullptr) router->OnLine(line, len);
}

// OUTBOUND: a frame the router emitted -> the transport's TX buffer, which owns
// the trailing newline and drains it on the poll loop.
//
// This MUST NOT be `RouterSinkThunk`. Both are `FrameSink`s, so the swap
// compiles -- and it makes every reply re-enter the parser: `hello` is answered
// with a `bad_frame` nack, which is answered with an `unknown_type` nack, which
// recurses until the stack dies. Nothing reaches the host. The two thunks are
// deliberately adjacent and separately named so a reader cannot miss the
// distinction.
void RouterOutSinkThunk(void *ctx, const char *line, size_t len) {
    UsbCdc *cdc = static_cast<UsbCdc *>(ctx);
    if (cdc != nullptr) cdc->Send(line, len);
}

// A synchronous drain of the transport's TX buffer. The router calls this only
// where it must reach the host before the poll loop stops (the reboot ack).
void RouterTxFlushThunk(void *ctx) {
    UsbCdc *cdc = static_cast<UsbCdc *>(ctx);
    if (cdc != nullptr) cdc->ServiceTx();
}

}  // namespace

void LinkBind(CommandRouter &router, UsbCdc &cdc, UsbCdc::RawWrite raw, void *raw_ctx) {
    // The router's OUTBOUND sink goes straight to the transport. Binding it via
    // the router's own context (not the CDC's) keeps `Send` called on THIS `cdc`.
    router.SetSink(&RouterOutSinkThunk, &cdc);
    // ...and its synchronous flush, so a reply that must beat a reset can.
    router.SetTxFlush(&RouterTxFlushThunk, &cdc);
    // The transport's loss counters, so `status` can report them: a refused frame
    // fails entirely inside the transport (the sink returns void), so the router
    // cannot learn about it any other way. This is the wiring point that makes
    // `DroppedFrames`/`RxOverflows` reachable by a user.
    router.SetLossCounters(&cdc);
    // The transport's INBOUND sink goes to the router. Two different thunks, two
    // different contexts: neither direction can be mistaken for the other.
    cdc.Init(raw, raw_ctx, &RouterSinkThunk, &router);
}
