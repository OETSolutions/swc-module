#pragma once

#include <stddef.h>

#include "Maintenance/BleProvisioning.h"
#include "Maintenance/WebPage.h"

/*
 * Capacities for the two derived secrets and the display strings, so a caller
 * declares a buffer of the right size instead of guessing.
 *
 * These deliberately mirror the deriving modules' own bounds. They are written
 * as constants rather than derived from those modules because `BleProvisioning.h`
 * keeps its PoP length private to the .cpp (the derivation owns it), and a
 * shorter buffer here would be a silent truncation that fails later as a
 * misleading "wrong password" -- exactly the failure `PopDerive`'s own length
 * check exists to refuse.
 */
constexpr size_t kPopLen             = 8;    // 6 hex digits + NUL, per PopDerive
constexpr size_t kMaintenanceNameLen = 32;
constexpr size_t kMaintenanceUrlLen  = 64;

/*
 * What a caller learns about an OPEN maintenance window, for the app to display.
 *
 * **Why this is a plain POD in a host-compilable header rather than part of the
 * radio module.** Spec 8.3 option 1 shows the BLE Proof-of-Possession and the web
 * token to the user over the already-trusted USB link, because the board has no
 * display and no printed label to carry either secret. The frame that carries
 * them is emitted by `CommandRouter`, which is HOST-TESTED -- so the data has to
 * cross that boundary as a value, not as a call into `MaintenanceRadio.cpp`,
 * which no host build compiles. The router holds one of these and emits on
 * change; a test sets one directly.
 *
 * **`active == false` makes every string meaningless by construction**, so a
 * caller cannot read a stale PoP off a closed window. The radio clears the whole
 * struct on stop and the router republishes; a device that never opened a window
 * never has a secret in here at all.
 */
struct MaintenanceInfo {
    bool active = false;
    /*
     * The BLE Proof-of-Possession the Espressif provisioning app will ask for
     * (spec 8.3). Empty for a Sec0 session, which has no PoP by construction --
     * an empty PoP alongside `active` is the honest report, not an omission.
     */
    char pop[kPopLen] = {};
    // The web page's `X-SWC-Token` (spec 8.4). Always present while active: the
    // page is served on whatever network the device joins, so it is never
    // unauthenticated.
    char token[kWebTokenLen + 1] = {};
    /*
     * The URL to open, WITH the token in its query string, so a user taps a link
     * instead of retyping 12 hex characters. Empty until the device has an
     * address to put in it -- the AP address is only known once the AP is up.
     */
    char page_url[kMaintenanceUrlLen] = {};
    /*
     * The BLE advertised name (spec 8.3's "advertised name includes a short
     * device id"), so a user with several units on a bench knows which one to
     * pair with.
     */
    char ble_name[kMaintenanceNameLen] = {};
};
