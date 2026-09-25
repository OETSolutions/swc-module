// On-device tests for the USB CDC app link. Unity, run with
// `pio test -e esp32s3 -f test_hw`.
//
// These assert what only real silicon can answer: that the CDC interface
// enumerates, that it carries bytes (or, with the link un-installed, that it
// BUFFERS and reports bytes-accepted), and -- spec 4.1's HARD requirement -- that
// the CONSOLE is not on it. Everything with logic (the TX buffer, retry, RX
// assembly) is covered by test_native/test_link/UsbCdcTest.cpp, so nothing here
// re-tests that.
//
// RUNS ON THE DUT (2026-09-25, N-87). This file used to say "BLOCKED until the
// board exists". It does, and the suite now passes 22/22 on it. `pio test -e
// esp32s3 -f test_hw` is NOT the runner, though: the Unity output goes to the
// console and PlatformIO's espidf integration cannot parse it, so it reports "0
// test cases". Build with `pio test -e esp32s3 --without-uploading
// --without-testing`, flash, then read the DUT port with a serial capture across
// a reset. See spec N-87 and code/docs/bring-up-log.md.
//
// **A console that is unreadable is the reason this suite is worth more than it
// looks.** Installing TinyUSB moves the S3's single internal USB PHY from
// Serial-JTAG to USB-OTG (verified against Espressif's docs, 2026-09-20 -- spec
// 4.1), so when these tests run there is no console to print to: a failure here
// surfaces as a dead USB port, not as a message. Anything that must be observed
// has to be observed over the CDC link itself or on a meter.
//
// The test-definition macro takes an UNQUOTED identifier and stringifies it
// inside the macro (see test/unity_config.h).

#include "unity.h"

#include "Link/UsbCdc.h"

#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#include <string.h>

static UsbCdc cdc;

// The raw write the transport is injected with on device: the REAL
// `tinyusb_cdcacm_write_queue` plus a flush. Its return value is the whole reason
// the retry path exists -- it returns the bytes ACCEPTED (`MIN(in_size,
// size_available)`, tinyusb_cdc_acm.c), not the bytes requested.
//
// **In THIS suite the CDC driver is not installed** (nothing here calls
// `UsbLinkStart`), and `tinyusb_cdcacm_write_queue` then returns 0 for every call
// (`if (!get_acm(itf)) return 0;`). That is deliberate, not an oversight: keeping
// TinyUSB un-installed is what leaves the console and this Unity output readable
// over USB -- installing it takes the S3's single PHY to USB-OTG (spec 4.1,
// N-16). So the tests below assert the transport's behaviour in exactly that
// state, and the partial-acceptance retry path is covered on the host against a
// FIFO double (`test_native/test_link/UsbCdcTest.cpp`).
static size_t DeviceRawWrite(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    const size_t written = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    return written;
}

static void HostSink(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    (void)line;
    (void)len;
}

static void swc_setup(void)
{
    cdc.Init(&DeviceRawWrite, NULL, &HostSink, NULL);
}

static void swc_teardown(void)
{
}

// With no host attached, the transport must still BUFFER a full pair of maximum
// frames without dropping either -- that is the entire reason `kTxCapacity` is
// two frames rather than one (the app may be mid-write when the firmware emits
// the next frame). This is the "no host" claim stated as what it can actually
// mean on device: the bytes are held, counted, and not lost, because nothing is
// draining the FIFO.
//
// An earlier revision asserted `PendingTx() == 0` after a bounded `ServiceTx`
// loop, which is UNACHIEVABLE with no host: two maximum frames are 2046 bytes and
// the CDC TX FIFO is CONFIG_TINYUSB_CDC_TX_BUFSIZE (512), so the FIFO fills after
// 512 bytes and every later write reports 0. The loop could never reach zero and
// the test failed at "Expected 0 Was 2046" -- the transport was working exactly
// as designed; the assertion was wrong.
TEST(ATransportBuffersAMaximumFrameWithNoHostAttached, "[hw]")
{
    char line[kNdjsonMaxFrame - 1];
    memset(line, 'a', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    TEST_ASSERT_TRUE(cdc.Send(line, sizeof(line) - 1));
    TEST_ASSERT_TRUE(cdc.Send(line, sizeof(line) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, cdc.DroppedFrames());

    // Everything is still held, whole: two frames plus their transport newlines.
    TEST_ASSERT_EQUAL_UINT(2 * (sizeof(line) - 1 + 1), cdc.PendingTx());

    // Draining what CAN be accepted must never LOSE the rest. With no host the
    // FIFO takes at most its own size and the remainder stays buffered -- the
    // property that matters is "held", not "emptied".
    for (int i = 0; i < 50; ++i) cdc.ServiceTx();
    TEST_ASSERT_EQUAL_UINT32(0, cdc.DroppedFrames());
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(
        0, cdc.PendingTx(),
        "no host is draining, so the frames must remain buffered, not vanish");
}

// The contract the whole retry design rests on, asserted against the REAL driver
// function rather than a double: `tinyusb_cdcacm_write_queue` returns the bytes
// ACCEPTED, not the bytes requested, and returns 0 when the driver is not
// installed. A firmware that trusted the requested length would advance `tx_off_`
// past bytes that never entered the FIFO and silently deliver a frame's TAIL to
// the app ("the app missed a key").
//
// The frame is larger than the FIFO on purpose, so on a build WITH the driver up
// the call must under-accept; here, with the driver down, it must accept nothing
// at all. Either way the return is `<= len`, never `len` blindly.
TEST(TheRealFifoReportsBytesAcceptedNotBytesRequested, "[hw]")
{
    enum { kFrame = 1500 };
    TEST_ASSERT_TRUE_MESSAGE(
        kFrame + 1 > CONFIG_TINYUSB_CDC_TX_BUFSIZE,
        "the frame must exceed the FIFO or the contract is untested");

    uint8_t buf[kFrame];
    memset(buf, 'b', sizeof(buf));
    const size_t accepted =
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf, sizeof(buf));

    // Never more than it was given -- the invariant ServiceTx clamps against.
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof(buf), accepted);

    // With the link NOT installed (this suite's state), nothing enters the FIFO.
    // Asserting the exact 0 documents the no-driver contract the transport must
    // tolerate: it buffers and returns rather than spinning on a 0-accepting write.
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        0, accepted,
        "with tinyusb not installed the FIFO accepts nothing; a nonzero value here "
        "would mean the driver was installed and this suite could dark the console");
}

// Spec 4.1's HARD requirement, asserted rather than trusted: the console must not
// be the CDC interface. `CONFIG_ESP_CONSOLE_UART_DEFAULT` (UART0, on this board's
// TP7/TP8) selects a console entirely independent of the USB PHY; the secondary
// (`ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`) duplicates output to the ROM
// peripheral; `CONFIG_ESP_CONSOLE_USB_CDC` would move the console onto TinyUSB and
// let a debug printf be parsed as a protocol frame, which nothing must ever do.
//
// **Why UART0 and not USB-Serial-JTAG (N-16).** This test used to assert
// `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`, which is the SAFE configuration but a
// useless one at runtime: the S3 has one internal USB PHY, shared by both USB
// controllers, so `UsbLinkStart` installing TinyUSB moves the PHY to USB-OTG and a
// Serial-JTAG console goes DARK -- making every `ESP_LOG*` line, including the
// link's own install-failure warning, unreadable. UART0 is not on that PHY, so the
// console survives TinyUSB taking it. The board already breaks UART0 out to test
// pads TP7/TP8, so this needed no respin.
TEST(TheConsoleIsNotConfiguredOntoTheCdcPort, "[hw]")
{
#if defined(CONFIG_ESP_CONSOLE_USB_CDC)
    TEST_FAIL_MESSAGE("console is on the USB CDC port -- spec 4.1 forbids this");
#endif
    TEST_ASSERT_TRUE(CONFIG_ESP_CONSOLE_UART_DEFAULT);
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
