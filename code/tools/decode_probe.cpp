// Decode the Kotlin-produced fixture with the FIRMWARE's own decoder, re-encode it
// and require the bytes to be identical.
//
// This is the ONLY check that can see a Kotlin/C divergence: each side's own
// round-trip test compares that side to itself, so a field name that differs
// between them passes both and produces a config the device refuses, reported as
// "corrupt config" with nothing named. Driven by `tools/crosscheck_config.sh`.
//
// **The exit code is the gate, and it used to be wrong.** Every failure below --
// a rejected decode, a failed validate, a re-encode failure, and most importantly
// a NON-IDENTICAL round-trip -- printed its message and then returned 0, so the
// script's `set -e` never fired and CI would have passed on precisely the defect
// the check exists to detect. A check whose failing verdict is a line of prose
// rather than its exit status is not a check.
#include "Config/ConfigCodec.h"
#include "Config/ConfigModel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Sized for a config far larger than ConfigMaxSerializedSize(), so a fixture that
// somehow exceeds the legal maximum is reported by the decoder rather than
// truncated silently here.
static const size_t kBufBytes = 262144;

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: decode_probe <fixture.json>\n");
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (f == nullptr) {
        printf("FAIL: cannot open %s\n", argv[1]);
        return 2;
    }
    static char buf[kBufBytes];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (n == 0) {
        printf("FAIL: %s is empty\n", argv[1]);
        return 1;
    }

    Config c{};
    if (!ConfigDecodeJson(buf, n, &c)) {
        printf("FAIL: firmware decoder REJECTED the app's JSON\n");
        return 1;
    }
    printf("OK: firmware decoded the app's config\n");
    printf("  device_id=%s schema=%u updated_at_ms=%llu\n", c.device_id, c.schema_version,
           (unsigned long long)c.updated_at_ms);
    printf("  bindings=%u aux=%u channels=%u\n", c.binding_count, c.aux_count, c.channel_count);

    if (!ConfigValidate(c)) {
        printf("FAIL: ConfigValidate rejected it\n");
        return 1;
    }
    printf("OK: ConfigValidate accepted it\n");

    static char out[kBufBytes];
    const size_t m = ConfigEncodeJson(c, out, sizeof(out));
    if (m == 0) {
        printf("FAIL: re-encode failed\n");
        return 1;
    }
    printf("  re-encoded %zu bytes\n", m);

    if (m != n || memcmp(out, buf, n) != 0) {
        // The whole point. Show where they first differ -- a field name or a
        // scaled value -- because "not identical" alone names nothing actionable.
        size_t i = 0;
        while (i < m && i < n && out[i] == buf[i]) i++;
        printf("FAIL: re-encoded bytes differ from the input at byte %zu\n", i);
        printf("    kotlin:   ...%.90s\n", buf + (i > 40 ? i - 40 : 0));
        printf("    firmware: ...%.90s\n", out + (i > 40 ? i - 40 : 0));
        return 1;
    }

    printf("OK: byte-identical to input\n");
    return 0;
}
