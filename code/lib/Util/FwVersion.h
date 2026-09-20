#pragma once

/*
 * The firmware version string, as reported in the spec 4.3 `hello` frame.
 *
 * The build passes the ALREADY-QUOTED literal (`-D SWC_FW_VERSION_RAW=\"1.2.3\"`,
 * so the macro's replacement text is `"1.2.3"` including the quotes), and this
 * header hands it straight to the consumer. It deliberately does NOT stringify.
 *
 * It used to, via `SWC_STR`, and that was a defect on EVERY build: stringifying
 * an argument that already carries quotes wraps a second pair around it, so the
 * `hello` body came out as `"fw_version":""0.0.0-ci""`. That is not valid JSON,
 * so the app's very first frame -- the one spec 4.5 relies on for version
 * negotiation -- failed to parse. The native build escaped notice because it
 * defines no version at all and fell through to the (correctly quoted) default
 * below, so the mangled case only ever appeared on a real device or in CI, the
 * two places that had no host test. `LinkWiringTest` now parses the emitted
 * `hello` as JSON, which fails on either form of the mistake.
 *
 * When the value is genuinely absent (a bare host build), the fallbacks keep
 * every consumer compiling AND producing valid JSON. It is DELIBERATELY
 * obviously-not-a-release: a device reporting "dev-unknown" is honest, and a
 * device reporting a plausible-looking version number it does not have is not.
 *
 * The fallback lives in `FwVersionString()` rather than in the `#ifdef` because
 * the build ALWAYS defines the macro -- `${sysenv.SWC_FW_VERSION}` expands to an
 * empty replacement list when the environment variable is unset, and `#ifdef`
 * cannot tell "defined as nothing" from "defined as a version". Keying the
 * fallback off `#ifdef` therefore produced an EMPTY version string on every bare
 * build, which is not the same claim as "unknown" and gives the app nothing to
 * negotiate with. The runtime check below treats empty and absent alike.
 */

#ifdef SWC_FW_VERSION_RAW
#define SWC_FW_VERSION SWC_FW_VERSION_RAW
#else
#define SWC_FW_VERSION ""
#endif

#ifdef SWC_GIT_SHA_RAW
#define SWC_GIT_SHA SWC_GIT_SHA_RAW
#else
#define SWC_GIT_SHA ""
#endif

// The version to report, never empty: an unset or empty build value becomes
// "dev-unknown" (see above), so spec 4.5's negotiation always has a token.
//
// `static inline`, not plain `inline`: this header is C-compatible and a C99
// translation unit that included it would emit no definition for a plain
// `inline` function and fail to link. `static` also keeps the internal linkage
// unambiguous for every includer.
static inline const char *FwVersionString() {
    return (SWC_FW_VERSION[0] != '\0') ? SWC_FW_VERSION : "dev-unknown";
}
