#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * BLE provisioning (spec 8.3, FR-34): Espressif's unified provisioning with a
 * BLE transport, which is what makes the Espressif provisioning app work.
 * NimBLE, not Bluedroid -- materially smaller flash and RAM, which matters on
 * 4 MB with no PSRAM.
 *
 * **The Proof-of-Possession is the whole design, and this board cannot print
 * one.** Sec1 needs a secret the user supplies out-of-band, but there is no
 * display and no printed label (the enclosure favours windows over silkscreen),
 * so a per-device printed PoP is unavailable. Spec 8.3's recommended path is to
 * DERIVE one from the chip's MAC and show it in the Android app over the
 * already-trusted USB link, for the user to type into the Espressif app. The
 * derivation is pure logic, and it is the part of this module the host can test.
 *
 * **What is host-testable, stated honestly**: the derivation, the advertised
 * short name, and the Sec0 gate. The NimBLE stack, GAP advertising, the
 * `wifi_provisioning` endpoints and the SRP6a handshake are DEVICE-ONLY; their
 * coverage is `code/test/test_hw`, not a host double. Nothing here pretends
 * otherwise.
 *
 * **The radio is never initialised by this module.** FR-32 / spec 8.1 make WiFi
 * and BLE maintenance-only, and Task 18's `MaintenanceMode` decides when. A
 * module that started NimBLE on construction would put a radio in the
 * normal-operation path, breaking the spec's largest power/RAM/attack-surface
 * rule.
 */

struct ProvSecurity {
    bool sec1 = true;
    bool allow_insecure = false;
};

enum class PopMode {
    kDerivedFromMac,   // the product default: salted digest of the factory MAC
    kFixedBench,       // a bench convenience, never a shipped default
};

// Writes the per-device PoP into `out_pop` (NUL-terminated). Returns FALSE
// rather than a truncated or trivial value when `out_len` cannot hold the
// result -- a short PoP fails later as a misleading "wrong password", so
// refusing here is what keeps the failure diagnosable.
bool PopDerive(const uint8_t mac[6], char *out_pop, size_t out_len);

// The short device id used in the advertised name (spec 8.3), so several units
// are distinguishable in the Espressif app's scan list.
void DeviceIdShort(const uint8_t mac[6], char *out, size_t out_len);

// Spec 8.3 option 3: Sec0 is REJECTED as a default and offered only behind an
// explicit flag, for bench use. An unauthenticated provisioning window is a
// radio-range takeover, so this is a readable decision a test can assert rather
// than a build-time choice buried in a Kconfig nothing checks.
bool ProvisioningAllowsSec0(bool allow_insecure_provisioning);

class BleProvisioning {
public:
    // Arms the session. Does NOT touch the radio -- `Start` is the point at
    // which the maintenance path may do so, and it happens on device only.
    //
    // **Both `ProvSecurity` fields decide something here, which is the point.**
    // `sec1` selects the security mode and `allow_insecure` gates Sec0 through
    // `ProvisioningAllowsSec0`. A Sec0 request without the flag is REFUSED rather
    // than silently upgraded or silently downgraded, and a PoP is required only in
    // Sec1 -- Sec0 has no Proof-of-Possession by construction.
    //
    // It used to store `allow_insecure` into a member nothing read, so a bench
    // build that set the flag behaved exactly like a Sec1 build while *looking*
    // like it honoured the choice, and `ProvisioningAllowsSec0` -- whose header
    // calls it "a readable decision a test can assert" -- had no production caller
    // at all. Returns false for a session that could not be armed.
    bool Start(const uint8_t mac[6], const ProvSecurity &sec);

    void Stop();
    bool Active() const { return active_; }

    // Whether this session runs WITHOUT security (spec 8.3 option 3). True only
    // when the caller asked for Sec0 and `allow_insecure` permitted it.
    bool Sec0() const { return sec0_; }

    // The PoP the app should display for this session. Empty until Start(), and
    // empty for a Sec0 session, which has none.
    const char *Pop() const { return pop_; }
    const char *AdvertisedName() const { return name_; }

    void SetMode(PopMode m) { mode_ = m; }

private:
    bool     active_ = false;
    bool     sec0_ = false;
    PopMode  mode_ = PopMode::kDerivedFromMac;
    char     pop_[32] = {};
    char     name_[32] = {};
};
