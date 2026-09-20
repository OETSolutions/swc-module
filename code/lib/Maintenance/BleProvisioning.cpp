#include "Maintenance/BleProvisioning.h"

#include <stdio.h>
#include <string.h>

#include "Util/Sha256.h"

namespace {

// The salt that keeps the PoP from being the MAC in the clear. A constant is
// correct here: it is not a secret (it ships in the firmware), it only has to
// make the digest of a known MAC non-obvious, and the PER-DEVICE part comes from
// the MAC itself. An attacker who has the salt and can guess the MAC still has
// to be in radio range during the provisioning window, which is the window Sec1
// exists to bound.
constexpr char kPopSalt[] = "swc-pop-v1";

// The PoP is a short digest prefix rendered in a form a person can type. Six
// uppercase hex characters is 24 bits -- enough that guessing inside one
// provisioning window is not viable, and short enough to read aloud.
constexpr size_t kPopDigits = 6;

}  // namespace

bool PopDerive(const uint8_t mac[6], char *out_pop, size_t out_len) {
    if (mac == nullptr || out_pop == nullptr) return false;
    // Six hex digits, plus an optional separator, plus the NUL.
    if (out_len < kPopDigits + 1) return false;

    char material[64];
    const int n = snprintf(material, sizeof(material), "%s:%02X%02X%02X%02X%02X%02X",
                           kPopSalt, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(material)) return false;

    char hex[65];
    if (!Sha256Hex(reinterpret_cast<const uint8_t *>(material), strlen(material), hex)) {
        return false;
    }

    // Uppercase, because a PoP is typed by a person off a screen.
    for (size_t i = 0; i < kPopDigits; ++i) {
        const char c = hex[i];
        out_pop[i] = (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    out_pop[kPopDigits] = '\0';
    return true;
}

void DeviceIdShort(const uint8_t mac[6], char *out, size_t out_len) {
    if (out == nullptr || out_len == 0) return;
    if (mac == nullptr) {
        out[0] = '\0';
        return;
    }
    // The last two MAC octets: unique in practice for units on one bench, and
    // short enough to fit an advertising name. The OUI is the same on every unit
    // of this product, so including it would waste the name's bytes on a
    // constant.
    snprintf(out, out_len, "%02X%02X", mac[4], mac[5]);
}

bool ProvisioningAllowsSec0(bool allow_insecure_provisioning) {
    return allow_insecure_provisioning;
}

bool BleProvisioning::Start(const uint8_t mac[6], const ProvSecurity &sec) {
    allow_insecure_ = sec.allow_insecure;

    if (mode_ == PopMode::kFixedBench) {
        // A fixed PoP is a bench convenience. It is a compile-and-call-site
        // decision, never a shipped default, and the short name still comes from
        // the MAC so two bench units remain distinguishable.
        snprintf(pop_, sizeof(pop_), "%s", "123456");
    } else if (!PopDerive(mac, pop_, sizeof(pop_))) {
        // Refuse the session rather than advertising with an empty or truncated
        // PoP: a half-set secret fails later as "wrong password", which points
        // nowhere near the real cause.
        pop_[0] = '\0';
        active_ = false;
        return false;
    }

    DeviceIdShort(mac, name_, sizeof(name_));
    active_ = true;
    return true;
}

void BleProvisioning::Stop() {
    active_ = false;
    pop_[0] = '\0';
    name_[0] = '\0';
}
