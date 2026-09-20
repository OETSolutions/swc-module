// On-device tests for the USB CDC app link. Unity, run with
// `pio test -e esp32s3 -f test_hw`.
//
// These assert what only real silicon can answer: that the CDC interface
// enumerates, that it carries bytes, and -- spec 4.1's HARD requirement -- that
// the CONSOLE is not on it. Everything with logic (the TX buffer, retry, RX
// assembly) is covered by test_native/test_link/UsbCdcTest.cpp, so nothing here
// re-tests that.
//
// BLOCKED until the board exists. The plan says to report that as blocked, not
// green.
//
// The test-definition macro takes an UNQUOTED identifier and stringifies it
// inside the macro (see test/unity_config.h).

#include "unity.h"

#include "Link/UsbCdc.h"

#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#include <string.h>

static UsbCdc cdc;

// The raw write the transport is injected with on device. Counting calls here is
// what makes "short writes were retried" observable on real hardware rather than
// only against the host FIFO double.
static size_t raw_write_calls = 0;
static size_t raw_bytes_accepted = 0;

static size_t DeviceRawWrite(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    ++raw_write_calls;
    const size_t written = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    raw_bytes_accepted += written;
    return written;
}

static void HostSink(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    (void)line;
    (void)len;
}

void setUp(void)
{
    raw_write_calls = 0;
    raw_bytes_accepted = 0;
    cdc.Init(&DeviceRawWrite, NULL, &HostSink, NULL);
}

void tearDown(void)
{
}

// The transport must be able to accept a maximum frame without a host attached:
// the buffer is a fixed member, so this is really asserting that the frame cap
// and the buffer size agree. A mismatch here would show up as a dropped frame
// only under load.
TEST(ATransportAcceptsAMaximumFrameWithNoHostAttached, "[hw]")
{
    char line[kNdjsonMaxFrame - 1];
    memset(line, 'a', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    TEST_ASSERT_TRUE(cdc.Send(line, sizeof(line) - 1));
    // Two of them, because the buffer is sized for two -- the case where the app
    // is mid-write when the next frame is emitted.
    TEST_ASSERT_TRUE(cdc.Send(line, sizeof(line) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, cdc.DroppedFrames());
    for (int i = 0; i < 2000 && cdc.PendingTx() > 0; ++i) {
        cdc.ServiceTx();
    }
    TEST_ASSERT_EQUAL_UINT(0, cdc.PendingTx());
}

// A short write against the REAL FIFO must still deliver everything. The host
// suite proves the retry logic against a double; this proves the double's
// assumption about `tinyusb_cdcacm_write_queue`'s return value is right -- it
// returns the bytes ACCEPTED (`MIN(in_size, size_available)`, tinyusb_cdc_acm.c),
// not the bytes requested.
//
// The frame must be LARGER THAN THE TX FIFO or no short write happens and this
// test silently proves nothing. That size is CONFIG_TINYUSB_CDC_TX_BUFSIZE, set
// to 512 in sdkconfig.defaults; a frame of 1500 leaves no way for one call to
// take it all. (An earlier version used 512 and a comment claiming a "64-byte-ish
// FIFO" -- both stale, and the test would have passed without exercising retry.)
TEST(ShortWritesAgainstTheRealFifoStillSendTheWholeFrame, "[hw]")
{
    enum { kFrame = 1500 };
    TEST_ASSERT_TRUE(kFrame + 1 > CONFIG_TINYUSB_CDC_TX_BUFSIZE);

    char line[kFrame];
    memset(line, 'b', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    TEST_ASSERT_TRUE(cdc.Send(line, sizeof(line) - 1));
    for (int i = 0; i < 2000 && cdc.PendingTx() > 0; ++i) {
        cdc.ServiceTx();
    }
    TEST_ASSERT_EQUAL_UINT(0, cdc.PendingTx());
    TEST_ASSERT_EQUAL_UINT32(0, cdc.DroppedFrames());
    // The retry path was actually entered: more than one call was needed for one
    // frame, which is the whole point of `ServiceTx`.
    TEST_ASSERT_TRUE(raw_write_calls > 1);
    TEST_ASSERT_EQUAL_UINT(kFrame, raw_bytes_accepted);
}

// Spec 4.1's HARD requirement, asserted rather than trusted: the console must not
// be the CDC interface. `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` selects the ROM
// peripheral; `CONFIG_ESP_CONSOLE_USB_CDC` would move the console onto TinyUSB
// and let a debug printf be parsed as a protocol frame.
TEST(TheConsoleIsNotConfiguredOntoTheCdcPort, "[hw]")
{
#if defined(CONFIG_ESP_CONSOLE_USB_CDC)
    TEST_FAIL_MESSAGE("console is on the USB CDC port -- spec 4.1 forbids this");
#endif
    TEST_ASSERT_TRUE(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG);
}

// The CDC interface must exist at all -- this is the whole of spec 4.1's app link
// and it did not exist before esp_tinyusb was added.
//
// Asserted as a Kconfig fact rather than as `TINYUSB_CDC_ACM_0 >= 0`: that enum is
// 0x0, so a comparison against 0 is constant-true and would pass on a build where
// CDC was never compiled in. `CONFIG_TINYUSB_CDC_ENABLED` is what decides whether
// tinyusb_cdc_acm.c is added to the build at all (esp_tinyusb's CMakeLists), so it
// is the symbol that can actually be false.
TEST(TheCdcInterfaceIsCompiledIntoTheBuild, "[hw]")
{
    TEST_ASSERT_EQUAL_INT(1, CONFIG_TINYUSB_CDC_ENABLED);

    tinyusb_config_cdcacm_t cfg = {};
    cfg.cdc_port = TINYUSB_CDC_ACM_0;
    TEST_ASSERT_EQUAL_INT(TINYUSB_CDC_ACM_0, cfg.cdc_port);
}
