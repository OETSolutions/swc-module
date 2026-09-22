#include "Update/ReleaseCheck.h"

#include <string.h>

#include "Update/ImageVerify.h"   // SemverCompare, so there is one version comparison
#include "cJSON.h"

namespace {

const cJSON *Member(const cJSON *obj, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

// A required non-empty string. A missing field is a malformed manifest, never a
// defaulted one: a manifest with no hash must not be read as "no hash to check".
bool ReadStr(const cJSON *obj, const char *key, char *out, size_t width) {
    const cJSON *v = Member(obj, key);
    if (!cJSON_IsString(v) || v->valuestring == nullptr) return false;
    const size_t n = strlen(v->valuestring);
    if (n == 0 || n >= width) return false;
    memcpy(out, v->valuestring, n + 1);
    return true;
}

bool ReadSize(const cJSON *obj, const char *key, size_t *out) {
    const cJSON *v = Member(obj, key);
    if (!cJSON_IsNumber(v) || v->valuedouble <= 0.0) return false;
    if (v->valuedouble > 4294967295.0) return false;
    // A byte count carrying a fraction is REFUSED, not truncated -- the same rule
    // `ConfigCodec`'s ReadU32/ReadU64 and `CommandRouter`'s NumToU32/NumToU8
    // enforce, for the same reason: a bare cast accepts a value the sender did not
    // write and the receiver then acts on. Here the consequence is not an accepted
    // wrong image (OtaEnd still requires an exact byte count AND the digest) but a
    // permanent unexplained failure: `size_bytes: 1543210.9` declared a size no real
    // image has, so every download of a correct image ended in `kSizeMismatch`,
    // which names neither the manifest nor the field.
    if (v->valuedouble != static_cast<double>(static_cast<size_t>(v->valuedouble))) return false;
    *out = static_cast<size_t>(v->valuedouble);
    return true;
}

// The image must come over TLS. A plain-http URL is not a lesser preference, it
// is a downgrade attack: an attacker on the path substitutes the image, and the
// SHA-256 does not help because the hash came over the same hijacked channel.
bool IsHttpsUrl(const char *u) {
    return u != nullptr && strncmp(u, "https://", 8) == 0;
}

}  // namespace

ReleaseCheckResult ReleaseCheckParse(const char *json, ReleaseInfo *out,
                                     const char *current_version) {
    if (json == nullptr || out == nullptr || current_version == nullptr) {
        return ReleaseCheckResult::kMalformed;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == nullptr) return ReleaseCheckResult::kMalformed;

    ReleaseInfo info{};
    ReleaseCheckResult result = ReleaseCheckResult::kMalformed;

    do {
        if (!cJSON_IsObject(root)) break;
        if (!ReadStr(root, "latest_version", info.latest_version, sizeof(info.latest_version))) break;

        const cJSON *fw = Member(root, "firmware");
        if (!cJSON_IsObject(fw)) break;
        if (!ReadStr(fw, "version", info.firmware_version, sizeof(info.firmware_version))) break;
        if (!ReadStr(fw, "sha256", info.sha256_hex, sizeof(info.sha256_hex))) break;
        if (!ReadSize(fw, "size_bytes", &info.size_bytes)) break;
        if (!ReadStr(fw, "url", info.url, sizeof(info.url))) break;

        // Optional, so a manifest without it is well-formed; it just means "any
        // version may upgrade".
        const cJSON *minf = Member(root, "min_from_version");
        if (minf != nullptr) {
            if (!cJSON_IsString(minf) || minf->valuestring == nullptr) break;
            const size_t n = strlen(minf->valuestring);
            if (n >= sizeof(info.min_from_version)) break;
            memcpy(info.min_from_version, minf->valuestring, n + 1);
        }
        // Optional too; carried for display.
        const cJSON *chan = Member(root, "channel");
        if (cJSON_IsString(chan) && chan->valuestring != nullptr &&
            strlen(chan->valuestring) < sizeof(info.channel)) {
            memcpy(info.channel, chan->valuestring, strlen(chan->valuestring) + 1);
        }

        if (!IsHttpsUrl(info.url)) break;

        // The top-level version is what the pipeline publishes; the firmware
        // block repeats it. If they disagree the manifest is self-contradictory,
        // and picking one silently is how a device installs a version it did not
        // choose.
        if (strcmp(info.latest_version, info.firmware_version) != 0) break;

        *out = info;

        // Order matters: "not newer" before "too old", because a manifest that is
        // both older than the running firmware AND below its own floor is best
        // described as "no upgrade here", not as a migration problem.
        const int cmp = SemverCompare(info.latest_version, current_version);
        if (cmp == 0) {
            result = ReleaseCheckResult::kUpToDate;
        } else if (cmp < 0) {
            result = ReleaseCheckResult::kNotNewer;
        } else if (info.min_from_version[0] != '\0' &&
                   SemverCompare(current_version, info.min_from_version) < 0) {
            result = ReleaseCheckResult::kTooOldToUpgradeFrom;
        } else {
            result = ReleaseCheckResult::kNewer;
        }
    } while (false);

    cJSON_Delete(root);
    return result;
}
