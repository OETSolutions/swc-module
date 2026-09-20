#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Decides whether a released version may be installed (spec 9.5).
 *
 * **The manifest is NESTED with semver strings**, per spec 9.5 and the Shared
 * contract:
 *
 *   { "latest_version": "1.4.0", "channel": "stable",
 *     "firmware": { "version": "1.4.0", "url": "...", "size_bytes": 1234567,
 *                   "sha256": "..." },
 *     "min_from_version": "1.0.0" }
 *
 * An earlier revision of this task used a FLAT manifest with `version_code`,
 * `board` and `min_from_version_code` -- the exact shape the Shared contract
 * warns against. Its stated consequence is why this matters: a flat manifest
 * "would make the device refuse every real release", because the pipeline
 * publishes the nested form and a device looking for `version_code` finds
 * nothing and reports a malformed manifest forever.
 *
 * **There is no `board` field, deliberately.** Spec 9.5's manifest has none, and
 * the identity it would carry already exists as the `hw_id` in every `hello`
 * (spec 4.3). A second home for "which hardware is this" is the defect class this
 * project keeps re-discovering, and a WRONG second home here means flashing
 * another board's image -- worse than not updating. A caller that wants the
 * check has `hw_id`; this module does not invent a manifest field for it.
 */

// Semver strings, sized from spec 9.5's examples with room to spare.
constexpr size_t kReleaseVersionLen = 24;
constexpr size_t kReleaseSha256Len  = 65;
constexpr size_t kReleaseUrlLen     = 256;

struct ReleaseInfo {
    char   latest_version[kReleaseVersionLen];   // the top-level "latest_version"
    char   firmware_version[kReleaseVersionLen]; // "firmware"."version"
    char   sha256_hex[kReleaseSha256Len];
    size_t size_bytes;
    char   url[kReleaseUrlLen];
    char   min_from_version[kReleaseVersionLen]; // empty when absent
    char   channel[kReleaseVersionLen];
};

enum class ReleaseCheckResult {
    kUpToDate = 0,        // same version as the running one
    kNewer,               // installable
    kNotNewer,            // older than the running version: never offer a downgrade
    kTooOldToUpgradeFrom, // running version is below min_from_version
    kWrongBoard,          // reserved: see the header note; unreachable by design
    kMalformed,           // not parseable, or missing a required field
};

// Parses the manifest and decides. `current_version` is the running firmware's
// semver string (the same value `hello` reports).
ReleaseCheckResult ReleaseCheckParse(const char *json, ReleaseInfo *out,
                                     const char *current_version);
