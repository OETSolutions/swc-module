#pragma once

#include "Link/UsbCdc.h"

class CommandRouter;

/*
 * Binds a router to a transport, BOTH directions, in one place.
 *
 * This exists because the two directions have the same function signature
 * (`FrameSink`) and are trivially confused. Passing the inbound thunk where the
 * outbound one belongs compiles cleanly and produces the worst possible failure:
 * every frame the firmware emits is fed back into `CommandRouter::OnLine` as if
 * the app had sent it. `hello` becomes a `bad_frame` nack, which is re-ingested
 * and becomes an `unknown_type` nack, and so on until the stack dies -- and
 * because nothing ever reaches the TX buffer, the app sees a device that
 * enumerates and then says nothing at all.
 *
 * That defect lived in `UsbLink.cpp` (a TinyUSB-only translation unit nothing
 * could host-test) and survived the full link suite, which always attached its
 * own recording sink. Keeping the binding here, in a TU with no device
 * dependency, is what makes it testable: `LinkWiringTest.cpp` drives the real
 * `UsbCdc` through the real binding and fails on the swap.
 *
 * `raw`/`raw_ctx` are the byte-level write `UsbCdc` drains into (on device:
 * `tinyusb_cdcacm_write_queue` + flush), injected so the whole path is testable.
 */
void LinkBind(CommandRouter &router, UsbCdc &cdc, UsbCdc::RawWrite raw, void *raw_ctx);
