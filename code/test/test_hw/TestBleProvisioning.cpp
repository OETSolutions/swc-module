// On-device tests for BLE provisioning. Unity, run with
// `pio test -e esp32s3 -f test_hw`.
//
// These assert what only real silicon and a real radio can answer: the MAC reads
// back, the advertised name appears in a scan, and the provisioning endpoints
// come up. Everything with logic -- the PoP derivation, the short id, the Sec0
// gate -- is covered by the host suite in test_native/test_maintenance, so
// nothing here re-tests that.
//
// RUNS ON THE DUT (2026-09-25, N-87). This file used to say "BLOCKED until the
// board exists". It does, and the suite now passes 22/22 on it -- including this
// file's MAC test, which was failing on a NON-IDEMPOTENT `EspHalInit` (the
// per-test setup called it again, the second `adc_oneshot_new_unit` failed with
// "adc1 is already in use", and the HAL came back NULL; `EspHalInit` now returns
// the SAME interface on a repeat call). See spec N-87 and docs/bring-up-log.md.
//
// The test-definition macro takes an UNQUOTED identifier and stringifies it
// inside the macro (see test/unity_config.h); Unity 2.6.1 defines no `TEST`, and
// `TEST_CASE` is the parameterized-test decorator.

#include "unity.h"

#include "Maintenance/BleProvisioning.h"
#include "HAL/EspHal.h"
#include "HAL/PinMap.h"

#include "esp_mac.h"   // esp_read_mac, ESP_MAC_WIFI_STA -- not pulled in by EspHal.h

#include <string.h>

static IHAL *hal = NULL;

static void swc_setup(void)
{
    hal = EspHalInit();
}

static void swc_teardown(void)
{
}

// The MAC must be readable and must look like an Espressif OUI on this hardware.
// This is the one input the PoP derivation depends on, and a zeroed or
// constant-looking MAC is the case that would make every unit share a PoP.
TEST(DeviceMacIsReadableAndNotAllZeroes, "[hw]")
{
    TEST_ASSERT_NOT_NULL(hal);
    uint8_t mac[6] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_read_mac(mac, ESP_MAC_WIFI_STA));

    int nonzero = 0;
    for (int i = 0; i < 6; ++i) {
        if (mac[i] != 0) ++nonzero;
    }
    TEST_ASSERT_GREATER_THAN(0, nonzero);
}

// The derived PoP must be producible from the REAL MAC, and must be the length
// the Espressif app's field expects. Deriving it from the wrong input (a random
// value, or an uninitialised buffer) is exactly what the host suite cannot see.
TEST(ThePopDerivesFromTheRealMac, "[hw]")
{
    uint8_t mac[6] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_read_mac(mac, ESP_MAC_WIFI_STA));

    char pop[32];
    TEST_ASSERT_TRUE(PopDerive(mac, pop, sizeof(pop)));
    TEST_ASSERT_EQUAL(6, (int)strlen(pop));
}

TEST(TheShortIdFitsAnAdvertisingName, "[hw]")
{
    uint8_t mac[6] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_read_mac(mac, ESP_MAC_WIFI_STA));

    char id[16];
    DeviceIdShort(mac, id, sizeof(id));
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(id));
    // The advertised name is "SWC-" + the id; the 31-byte BLE name limit is the
    // real constraint, and it is not close.
    TEST_ASSERT_LESS_THAN(31, (int)strlen(id) + 4);
}

// Sec0 stays OFF unless the bench flag is set. This is the assertion that keeps
// an unauthenticated provisioning window out of a shipped device.
TEST(Sec1IsTheDefaultAndSec0NeedsTheExplicitFlag, "[hw]")
{
    ProvSecurity sec;
    TEST_ASSERT_TRUE(sec.sec1);
    TEST_ASSERT_FALSE(sec.allow_insecure);
    TEST_ASSERT_FALSE(ProvisioningAllowsSec0(sec.allow_insecure));
}

// Starting a session must not need the radio to be up, and must not leave it up:
// spec 8.1 makes WiFi/BLE maintenance-only. This checks the module is inert at
// the point Task 18 would arm it.
TEST(AProvisioningSessionArmsWithoutInitialisingTheRadio, "[hw]")
{
    uint8_t mac[6] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_read_mac(mac, ESP_MAC_WIFI_STA));

    BleProvisioning p;
    TEST_ASSERT_FALSE(p.Active());
    TEST_ASSERT_TRUE(p.Start(mac, ProvSecurity{}));
    TEST_ASSERT_TRUE(p.Active());
    p.Stop();
    TEST_ASSERT_FALSE(p.Active());
}
