#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Config/ConfigModel.h"

/*
 * The token that gates the maintenance page (spec 8.4).
 *
 * **The page is reachable on whatever network the device joins**, so it is not
 * unauthenticated: "no default password, no admin/admin". The token is derived
 * the same way as the BLE PoP and shown to the user over the already-trusted USB
 * link, so there is nothing for an attacker on the LAN to guess and nothing baked
 * into the firmware.
 *
 * Host-testable, and deliberately separate from the HTTP server so that the
 * derivation and the comparison are testable without a socket.
 */

// The length of the derived token. 12 uppercase hex characters is 48 bits --
// enough that a token is not guessable over a LAN in a 5-minute window, short
// enough to copy off the USB screen without error.
constexpr size_t kWebTokenLen = 12;

// Derives the token from the factory MAC. Same reasoning as the BLE PoP
// (BleProvisioning.h): salted so the token is not the MAC in the clear, and
// per-device so one unit's token does not open another's page.
// Returns false rather than a truncated token when `out_len` is too small.
bool WebTokenDerive(const uint8_t mac[6], char *out, size_t out_len);

// Constant-time comparison, because a byte-at-a-time early-return compare leaks
// the token's prefix through timing on a link an attacker controls. The window is
// short and the token is 48 bits, but the fix costs nothing and the alternative
// is a class of bug that is invisible in review.
bool WebTokenMatches(const char *presented, const char *expected);

// Looks an asset up by request path. Returns nullptr for an unknown path, so the
// caller can answer 404 rather than serving the index page for every URL (which
// would silently make a typo'd API call return HTML).
//
// The asset struct is completed by including the generated `WebPageAssets.h`.
// This header deliberately does NOT include it: the generated file is large and
// changes on every page edit, so pulling it in here would recompile every
// consumer of the token helpers for a CSS tweak.
#include "Maintenance/WebPageAssets.h"
const WebAsset *WebPageFind(const char *path);

// The page's HTTP content type. One asset type, one answer -- a second content
// type is how a page starts being served as text/plain.
const char *WebPageContentType();
