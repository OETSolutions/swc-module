#include "Link/Ndjson.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

// ---------------------------------------------------------------------------
// NdjsonReader
// ---------------------------------------------------------------------------

NdjsonResult NdjsonReader::Push(uint8_t byte) {
    // Once latched, the reader discards input until the line that overran is
    // actually finished. The discard has to be driven by the incoming newline,
    // not by the caller: Consume() may be called mid-line, and if it simply
    // reset the counter the rest of the oversized line would be handed up as a
    // frame of its own. Self-healing on '\n' makes both call styles correct.
    if (too_long_) {
        if (byte != '\n') return NdjsonResult::kTooLong;
        len_ = 0;
        too_long_ = false;
        buf_[0] = '\0';
        return NdjsonResult::kTooLong;
    }

    if (byte == '\n') {
        // A CRLF host is common enough to be the default case, not an edge.
        if (len_ > 0 && buf_[len_ - 1] == '\r') --len_;
        buf_[len_] = '\0';
        return NdjsonResult::kComplete;
    }

    // len_ == kNdjsonMaxFrame means the line already holds the full budget and
    // this byte would push it over. Reject rather than truncate: a truncated
    // line still looks like a frame, and would be handed upward as one.
    if (len_ >= kNdjsonMaxFrame) {
        too_long_ = true;
        return NdjsonResult::kTooLong;
    }

    buf_[len_++] = static_cast<char>(byte);
    return NdjsonResult::kNeedMore;
}

void NdjsonReader::Consume() {
    len_ = 0;
    too_long_ = false;
    buf_[0] = '\0';
}

// ---------------------------------------------------------------------------
// NdjsonWriter
// ---------------------------------------------------------------------------

void NdjsonWriter::Write(const char *type, uint32_t seq, const char *json_body_fields) {
    const char *fields = json_body_fields ? json_body_fields : "";
    const bool has_fields = fields[0] != '\0';

    // Trailing comma, or not: a body of "null" or "{}" contributes no fields,
    // and `...,"type":"t","null"}` is not JSON.
    const bool body_is_null = strcmp(fields, "null") == 0 || strcmp(fields, "{}") == 0;
    const bool add_comma = has_fields && !body_is_null;

    int n = snprintf(line_, sizeof(line_), "{\"v\":%u,\"seq\":%u,\"type\":\"%s\"%s%s}\n",
                     static_cast<unsigned>(kNdjsonProtocolVersion),
                     static_cast<unsigned>(seq), type ? type : "", add_comma ? "," : "",
                     add_comma ? fields : "");

    // snprintf reports what it WOULD have written, so an oversized body is
    // detectable before it is a truncated line. Degrade to an error frame --
    // which is always short enough -- rather than emit a line the peer cannot
    // parse and cannot resynchronize from.
    if (n < 0 || static_cast<size_t>(n) > kNdjsonMaxFrame) {
        // `kNdjsonProtocolVersion`, NOT a literal `1`: this is the fallback frame
        // of last resort, so a protocol bump that missed it would send a peer an
        // envelope stamped with the OLD version -- and a peer that dispatches on
        // the envelope's `v` would refuse or mis-handle the one frame whose whole
        // job is to keep the link recoverable. The header above records that the
        // literal `"v":1` was replaced by the constant; this branch was the
        // remaining copy.
        n = snprintf(line_, sizeof(line_),
                     "{\"v\":%u,\"seq\":%u,\"type\":\"error\",\"error\":\"frame_too_long\"}\n",
                     static_cast<unsigned>(kNdjsonProtocolVersion),
                     static_cast<unsigned>(seq));
        if (n < 0) n = 0;
    }
    len_ = static_cast<size_t>(n);
}

// ---------------------------------------------------------------------------
// NdjsonParseEnvelope
// ---------------------------------------------------------------------------

bool NdjsonParseEnvelope(const char *line, FrameHeader *out) {
    if (line == nullptr || out == nullptr) return false;

    cJSON *root = cJSON_Parse(line);
    if (root == nullptr) return false;

    bool ok = false;
    do {
        if (!cJSON_IsObject(root)) break;

        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");

        // All three are required. A defaulted `seq` would silently renumber the
        // peer's frames, which is worse than dropping one.
        if (!cJSON_IsNumber(v) || !cJSON_IsNumber(seq) || !cJSON_IsString(type)) break;
        if (type->valuestring == nullptr) break;
        if (strlen(type->valuestring) >= kNdjsonTypeLen) break;

        const double vd = v->valuedouble;
        const double seqd = seq->valuedouble;
        // Reject rather than wrap. `(uint8_t)256.0` and `(uint32_t)-1.0` are
        // both silent corruptions, and both are reachable from a peer bug.
        if (vd < 0.0 || vd > 255.0) break;
        if (seqd < 0.0 || seqd > 4294967295.0) break;
        // And refuse a fraction for the same reason: `seq = 1.9` would be cast to
        // 1, so the frame is acknowledged as sequence 1 and the peer's own
        // bookkeeping disagrees with the device's -- a silent renumber, which is
        // the failure this function's required-`seq` rule above exists to avoid.
        if (vd != static_cast<double>(static_cast<uint8_t>(vd))) break;
        if (seqd != static_cast<double>(static_cast<uint32_t>(seqd))) break;

        FrameHeader h{};
        h.v = static_cast<uint8_t>(vd);
        h.seq = static_cast<uint32_t>(seqd);
        snprintf(h.type, sizeof(h.type), "%s", type->valuestring);

        *out = h;
        ok = true;
    } while (false);

    cJSON_Delete(root);
    return ok;
}
