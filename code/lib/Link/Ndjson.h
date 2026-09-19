#pragma once

#include <stddef.h>
#include <stdint.h>

// Every frame is one JSON line, and the cap applies to the WHOLE line including
// the envelope. It is a hard cap rather than a guideline: a peer that reads past
// it has already lost the frame boundary, so nothing above this layer can recover.
constexpr size_t kNdjsonMaxFrame = 1024;

// Longest `type` string plus terminator. The frame types are a fixed vocabulary
// (spec 4.3), so this is a bound on a closed set, not on user data.
constexpr size_t kNdjsonTypeLen = 24;

struct FrameHeader {
    uint8_t  v;
    uint32_t seq;
    char     type[kNdjsonTypeLen];
};

enum class NdjsonResult {
    kComplete,    // a full line is available via Line()
    kNeedMore,    // more bytes wanted
    kTooLong,     // the line exceeded kNdjsonMaxFrame; call Consume() to resync
    kMalformed,   // reserved for a reader-level fault (see Push)
};

/*
 * Byte-at-a-time line accumulator. Push() returns kComplete once a newline ends
 * the line, and Line() then holds it WITHOUT the newline.
 *
 * On overflow the reader latches kTooLong and stops consuming, so the caller can
 * drop the rest of the bad line and resynchronize on the next newline rather than
 * feeding the remainder through as if it were a frame.
 */
class NdjsonReader {
public:
    NdjsonResult Push(uint8_t byte);

    const char *Line() const { return buf_; }
    size_t LineLen() const { return len_; }

    // Abandon the current line and start fresh. After kTooLong the caller may
    // either call this or simply keep pushing -- the reader discards the rest of
    // the overrun line on its own and is ready again at the next '\n'. Calling
    // it mid-line asserts that the line really is over, so the remaining bytes
    // are treated as a new frame.
    void Consume();

private:
    char   buf_[kNdjsonMaxFrame + 1] = {};
    size_t len_ = 0;
    bool   too_long_ = false;
};

/*
 * One frame. Write() always produces a complete, parseable line or an error
 * frame -- never a partial or oversized one, because a line the peer cannot parse
 * desynchronizes the link for every frame that follows it.
 */
class NdjsonWriter {
public:
    void Write(const char *type, uint32_t seq, const char *json_body_fields);

    const char *Line() const { return line_; }
    size_t LineLen() const { return len_; }

private:
    // +2: kNdjsonMaxFrame content bytes, then the newline, then the terminator.
    // The cap bounds the JSON text, so a maximal frame still gets its '\n'.
    char   line_[kNdjsonMaxFrame + 2] = {};
    size_t len_ = 0;
};

// Requires all three of `v`, `seq` and `type`, each with the right JSON type. A
// missing field is a failure rather than a default: an envelope the sender did
// not mean to send is not something to guess at.
bool NdjsonParseEnvelope(const char *line, FrameHeader *out);
