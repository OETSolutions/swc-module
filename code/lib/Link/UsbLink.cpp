#include "Link/UsbLink.h"

#include <string.h>

#include <atomic>

#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#include "Config/ConfigStore.h"
#include "Link/CommandRouter.h"
#include "Link/LinkWiring.h"
#include "Link/UsbCdc.h"

static const char *TAG = "swc-usb";

namespace {

UsbCdc         g_cdc;
CommandRouter *g_router = nullptr;
bool           g_started = false;
// Whether the HOST has opened the port (DTR), as the poll task has APPLIED it.
// Tracked separately from `g_started` because the driver being installed does not
// mean anything is reading.
//
// Written and read by `ServiceLineState` on the poll task alone, so it needs no
// atomicity of its own; it is the record of what side effects have run, which is
// what makes `ServiceLineState` idempotent when the observed level has not moved.
bool g_host_open = false;
// The DTR LEVEL the callback observed most recently, which the poll task has not
// yet acted on. The callback must not call into the router or the orchestrator
// itself (see `UsbCdc::kRxCapacity`): it only records what it saw here, and
// `UsbLinkService` performs the transition on the task that owns that state.
//
// **A LEVEL, not an edge, and that is a correctness requirement rather than a
// convenience.** An edge published against the *applied* state loses a
// transition: if the host opens and closes between two poll ticks, the close is
// compared against `g_host_open == false` (the open has not been APPLIED yet), is
// judged "not a change", and is dropped -- so the poll task then applies the open
// and the device latches a session the host already ended: `hello` sent to
// nobody, LED_STAT solid forever, and a config run left open.
//
// Publishing the level instead makes the service reconcile with reality: it
// applies whatever the host's line is doing NOW, and applies both transitions
// (one tick apart) when the host really did open and close.
std::atomic<int> g_pending_line_state{-1};   // -1 no observation yet, 0 closed, 1 open

// Gesture and log sinks deliberately stay here: they are wired to the
// orchestrator, not to the transport, and only this TU names TinyUSB.

// A recognized gesture -> spec 4.3's `event`. Non-blocking is a REQUIREMENT, not
// a preference: this is called from inside `SystemOrchestrator::Tick`, on the
// poll loop that drives the KEY line, so a sink that blocked on a full TX buffer
// would delay the output and change what the head unit sees. `UsbCdc::Send`
// buffers and returns false when full, which is the correct behavior here -- a
// dropped telemetry frame is recoverable, a stalled key path is not.
void GestureSinkThunk(void *ctx, const SystemOrchestrator::GestureEventRecord &ev)
{
    CommandRouter *router = static_cast<CommandRouter *>(ctx);
    if (router != nullptr) router->EmitGesture(ev);
}

// FR-18's clamp warning reaches the app's log view through here.
void LogSinkThunk(void *ctx, const char *level, const char *msg)
{
    auto *r = static_cast<CommandRouter *>(ctx);
    if (r != nullptr) r->EmitLog(level, msg);
}

// TinyUSB hands us the bytes the host sent. This callback runs on the TinyUSB
// task, so it does the ONE thing that is safe from another task -- copy the bytes
// into the transport's SPSC staging ring -- and returns. Parsing them here would
// run the whole protocol against state that `SystemOrchestrator::Tick` is
// concurrently mutating on the poll task; `DrainRx` moves that work to the poll
// task, where the protocol's own state lives.
void CdcRxCallback(int itf, cdcacm_event_t *event)
{
    (void)event;
    if (itf != TINYUSB_CDC_ACM_0) return;

    uint8_t buf[64];
    // Drain the whole FIFO: returning after one fixed-size read would leave bytes
    // queued until the next callback, which under load looks like dropped input.
    while (true) {
        size_t len = 0;
        const esp_err_t err = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &len);
        if (err != ESP_OK || len == 0) break;
        g_cdc.FeedBytes(buf, len);
    }
}

// The orchestrator, so the line-state callback can drive `LED_STAT` (spec 7.3).
// Set once at start-up and never cleared: the device runs until reset.
SystemOrchestrator *g_sys = nullptr;

// The host opened the port (DTR asserted). This is when `hello` should go out:
// a host that has not opened the port is not reading, so a frame sent earlier is
// discarded by the driver and the app sees a missing opening frame. TinyUSB's
// own connection flag cannot serve here -- it only tracks the BUS (which is up
// the moment the cable is in), not whether anything is listening.
//
// **The work is deferred, not done here.** `OnConnected` emits `hello` and starts
// the config reply run, and `SetUsbConnected` repaints LED_STAT -- both reach
// state the poll task is using. This callback runs on the TinyUSB task, so it
// records the transition and `UsbLinkService` performs it.
void CdcLineStateCallback(int itf, cdcacm_event_t *event)
{
    if (itf != TINYUSB_CDC_ACM_0) return;
    if (event == nullptr) return;

    const bool open = event->line_state_changed_data.dtr;
    // Publish the LEVEL unconditionally. Comparing against the applied state
    // instead would DROP a close that arrived before its own open was serviced --
    // see `g_pending_line_state`.
    g_pending_line_state.store(open ? 1 : 0, std::memory_order_release);
}

// The deferred half of `CdcLineStateCallback`, on the poll task. Reconciles the
// applied state with the level the callback last observed, at most once per call.
void ServiceLineState()
{
    const int pending = g_pending_line_state.exchange(-1, std::memory_order_acq_rel);
    if (pending < 0) return;

    const bool open = (pending == 1);
    // Idempotent: an observation that matches what has already been applied runs
    // no side effects. This is also why the callback needs no edge detection of
    // its own -- a steady DTR level re-published is a no-op here.
    if (open == g_host_open) return;
    g_host_open = open;

    if (open) {
        // `hello` first (spec 4.5), then the config reply run so the app can
        // render without having to ask for anything.
        if (g_router != nullptr) g_router->OnConnected();
        // Spec 7.3: LED_STAT is solid with a host attached, breathing without.
        // Display only -- spec 6.6 keeps every button path independent of this.
        if (g_sys != nullptr) g_sys->SetUsbConnected(true);
        ESP_LOGI(TAG, "host opened the app port");
    } else {
        g_cdc.ResetSession();
        // Discards any half-received config run: an interrupted transfer must
        // never be applied (spec 4.2).
        if (g_router != nullptr) g_router->OnDisconnected();
        if (g_sys != nullptr) g_sys->SetUsbConnected(false);
        ESP_LOGI(TAG, "host closed the app port");
    }
}

// The device's raw write: queue into the CDC FIFO and flush. Returns the bytes
// ACCEPTED, which is exactly what `UsbCdc::ServiceTx` retries against. The queue
// copies only what fits in the FIFO, so a frame larger than
// `CONFIG_TINYUSB_CDC_TX_BUFSIZE` (512 B by default) is delivered over several
// rounds and the retry path is exercised on any frame over that size -- which
// includes every 512-byte config chunk reply.
size_t CdcRawWrite(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    const size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    return queued;
}

}  // namespace

void UsbLinkStart(IHAL *hal, SystemOrchestrator *sys)
{
    if (g_started) return;

    // Static, in BSS, never freed: the config slots and the config itself are
    // large (spec 3.5 carries 32 bindings) and the device runs until it is reset.
    // This MUST be one store -- a fresh ConfigStore per command would report an
    // empty NVS the first time it looked, and a `config_get` would answer with
    // defaults over a device that is actually configured.
    static ConfigStore store(hal);
    static CommandRouter router(hal, sys, &store);

    g_sys = sys;

    // FR-31: the headless learn persists through this same store. It must be the
    // ONE store -- a second instance would write to the same NVS keys but with its
    // own idea of the config, so a headless learn and an app config_set could
    // each save and then overwrite the other's result.
    if (sys != nullptr) sys->SetStore(&store);

    // Both directions of the router<->transport binding, in one call in a TU
    // that has no device dependency so the wiring itself is host-testable
    // (`LinkWiringTest.cpp`). Inlining it here is how the two directions got
    // crossed -- see LinkWiring.h.
    LinkBind(router, g_cdc, &CdcRawWrite, nullptr);
    g_router = &router;

    // Gestures are recognized by the poll loop, not by an inbound command, so
    // they reach the link through a sink rather than through a reply. Registered
    // unconditionally: the sink checks its own null router, and a device with a
    // failed USB install simply never has a host to emit to.
    if (sys != nullptr) sys->SetGestureSink(&GestureSinkThunk, &router);

    // The same shape as the gesture sink, and for the same reason: FR-18's clamp
    // warning is produced inside the poll loop, so it needs a sink rather than a
    // reply. A device with no host simply has no one to log to (spec 6.6).
    if (sys != nullptr) sys->SetLogSink(&LogSinkThunk, &router);

    tinyusb_config_t tusb_cfg = {};
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    // The task config is NOT optional and has no zero-valued default that passes:
    // tinyusb_driver_install rejects size 0 AND priority 0, so a zeroed struct
    // fails install with ESP_ERR_INVALID_ARG. 4096 is esp_tinyusb's own default
    // stack.
    //
    // **The priority is ABOVE the poll loop, not below it, and the design depends
    // on that.** `app_main` runs at `ESP_TASK_MAIN_PRIO` = 1 (`esp_task.h`), and
    // the idle task is 0, so 5 PREEMPTS `SystemOrchestrator::Tick` at any
    // instruction -- both are pinned to core 0 (`CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0`
    // and `xCoreID = 0`). An earlier comment here claimed the opposite ("the
    // priority is below the app loop so a USB burst cannot starve the poll loop"),
    // which is unachievable rather than merely wrong: no priority below 1 exists
    // above the idle task. What actually keeps a USB burst off the key path is the
    // CALLBACK SPLIT, not the priority -- `CdcRxCallback` only copies bytes into the
    // SPSC ring and `CdcLineStateCallback` only publishes a DTR level, so the
    // preemption window is a memcpy and an atomic store, and all protocol and
    // orchestrator work runs on the poll task in `UsbLinkService`. That invariant
    // is what `tools/check_task_ownership.py` gates; a reader who believed this
    // comment and lowered the priority to "fix" a real-time concern would instead
    // let a large burst delay the USB task's own servicing.
    tusb_cfg.task.size = 4096;
    tusb_cfg.task.priority = 5;
    tusb_cfg.task.xCoreID = 0;
    // Descriptors left NULL on purpose: esp_tinyusb supplies its own defaults for
    // a CDC device (descriptors_control.c). A custom descriptor set is a product
    // identity concern, not part of getting the link working.

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        // Reported, NOT fatal. A device with no USB still serves every press
        // (FR-42), so a failed link is a degraded bench condition rather than a
        // reason to refuse to boot.
        ESP_LOGW(TAG, "tinyusb driver install failed: %s; running without the app link",
                 esp_err_to_name(err));
        return;
    }

    tinyusb_config_cdcacm_t acm_cfg = {};
    // No `usb_dev` member: esp_tinyusb 2.x dropped it (the peripheral is chosen
    // by the driver config above), so setting it does not compile.
    acm_cfg.cdc_port = TINYUSB_CDC_ACM_0;
    acm_cfg.callback_rx = &CdcRxCallback;
    acm_cfg.callback_rx_wanted_char = nullptr;
    acm_cfg.callback_line_state_changed = &CdcLineStateCallback;
    acm_cfg.callback_line_coding_changed = nullptr;
    err = tusb_cdc_acm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cdc acm init failed: %s; running without the app link",
                 esp_err_to_name(err));
        return;
    }

    g_started = true;
    // NOT a session reset here: the driver is up, but no host has opened the
    // port, so `hello` waits for the DTR callback. Sending now would put the
    // opening frame into a FIFO nobody is draining.
    // The console is on UART0 (TP7/TP8), NOT on this port (spec 4.1, N-16), so
    // these `ESP_LOG*` lines stay readable after TinyUSB takes the USB PHY.
    ESP_LOGI(TAG, "app link initialised on TinyUSB CDC; console is on UART0 (TP7/TP8)");
}

void UsbLinkService()
{
    if (!g_started) return;

    // EVERYTHING below runs on the poll task (`app_main`), which is the point:
    // the TinyUSB callbacks only stage bytes and record the DTR transition, and
    // all protocol and orchestrator state is touched here, next to `Tick`.
    //
    // Order matters. The DTR transition first, so a session that just opened is
    // marked connected before its first frame is parsed -- otherwise `hello` and
    // the config reply run would be armed after the app's own opening frame
    // arrived. Then the staged input, which is where the command handlers run.
    ServiceLineState();

    // Parse whatever the callback staged, delivering each complete frame to the
    // router. Bounded by the staged bytes, not by a frame count, so a single call
    // cannot spin on an empty ring.
    g_cdc.DrainRx();

    // The router emits at most one deferred frame per call (a large config reply
    // is chunked), so calling both keeps a reply moving without a burst that the
    // transport's two-frame buffer would drop.
    if (g_router != nullptr) {
        // Time-based frames AFTER the inbound work (spec 4.4's 2 s status), so a
        // command answered this tick is not delayed by a status frame. `Process()`
        // is also called from host tests that never advance a clock, which is why
        // the periodic status is driven here rather than from `Process()`.
        g_router->Tick();
        g_router->Process();
    }
    g_cdc.ServiceTx();
}

void UsbLinkPublishMaintenance(const MaintenanceInfo &info, uint32_t failures)
{
    // Before Start, or after a failed install, there is no router to tell -- and
    // that is correct rather than a gap: with no host there is nobody to show the
    // secrets to (spec 6.6 keeps the device fully functional with no app).
    if (g_router != nullptr) g_router->SetMaintenanceInfo(info, failures);
}
