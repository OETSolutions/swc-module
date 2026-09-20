#include "Link/UsbLink.h"

#include <string.h>

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
// Whether the HOST has opened the port (DTR). Tracked separately from `g_started`
// because the driver being installed does not mean anything is reading.
bool           g_host_open = false;

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

// TinyUSB hands us the bytes the host sent. They go straight into the transport's
// assembler, which calls the sink once per COMPLETE frame -- so neither this
// callback nor the router ever sees a partial line.
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
void CdcLineStateCallback(int itf, cdcacm_event_t *event)
{
    if (itf != TINYUSB_CDC_ACM_0) return;
    if (event == nullptr) return;

    const bool open = event->line_state_changed_data.dtr;
    if (open && !g_host_open) {
        g_host_open = true;
        g_cdc.NoteConnected();
        // `hello` first (spec 4.5), then the config reply run so the app can
        // render without having to ask for anything.
        if (g_router != nullptr) g_router->OnConnected();
        // Spec 7.3: LED_STAT is solid with a host attached, breathing without.
        // Display only -- spec 6.6 keeps every button path independent of this.
        if (g_sys != nullptr) g_sys->SetUsbConnected(true);
        ESP_LOGI(TAG, "host opened the app port");
    } else if (!open && g_host_open) {
        g_host_open = false;
        g_cdc.NoteDisconnected();
        // Discards any half-received config run: an interrupted transfer must
        // never be applied (spec 4.2).
        if (g_router != nullptr) g_router->OnDisconnected();
        if (g_sys != nullptr) g_sys->SetUsbConnected(false);
        ESP_LOGI(TAG, "host closed the app port");
    }
}

// The device's raw write: queue into the CDC FIFO and flush. Returns the bytes
// ACCEPTED, which is exactly what `UsbCdc::ServiceTx` retries against. The queue
// copies only what fits in the FIFO, so a maximum frame (1024 B) is delivered
// over two rounds and the retry path is exercised on every large frame.
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
    // stack; the priority is below the app loop so a USB burst cannot starve the
    // poll loop that drives the KEY line.
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
    // NOT NoteConnected() here: the driver is up, but no host has opened the
    // port, so `hello` waits for the DTR callback. Sending now would put the
    // opening frame into a FIFO nobody is draining.
    ESP_LOGI(TAG, "app link initialised on TinyUSB CDC; console stays on USB-Serial-JTAG");
}

void UsbLinkService()
{
    if (!g_started) return;
    // The router emits at most one deferred frame per call (a large config reply
    // is chunked), so calling both keeps a reply moving without a burst that the
    // transport's two-frame buffer would drop.
    if (g_router != nullptr) g_router->Process();
    g_cdc.ServiceTx();
}
