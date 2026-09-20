#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#include "Config/ConfigCodec.h"
#include "Config/ConfigStore.h"
#include "HAL/IHAL.h"
#include "Learning/LearnSession.h"
#include "Link/Ndjson.h"
#include "System/SystemOrchestrator.h"

/*
 * The frame vocabulary (spec 4.3): every command from the app, and every reply
 * the firmware emits.
 *
 * `FrameSink` is NOT declared here -- it lives in the hand-written contract
 * (`Link/SwcContract.h`) because `UsbCdc` carries it too, and two declarations
 * of one function-pointer type is the duplicate-name defect.
 */
#include "Link/SwcContract.h"

/*
 * Frame -> handler. One `OnLine` per complete frame (the transport owns framing
 * and the trailing newline), replies written through the sink.
 *
 * The router owns the chunked config transport (spec 4.2). It is the only path
 * that can carry a config in either direction: a real config is ~22 KB and the
 * line cap is 1024 B, so a single-line config_set could never carry a legal one.
 */
class CommandRouter {
public:
    // `sys` may be null: the router still answers the protocol, it just cannot
    // drive the output for test_key or reach the orchestrator's state.
    CommandRouter(IHAL *hal, SystemOrchestrator *sys, ConfigStore *store);

    void SetSink(FrameSink sink, void *ctx);

    // Feed one frame, WITHOUT its trailing newline.
    void OnLine(const char *line, size_t len);

    // The link came up: emit `hello` (spec 4.5) then begin a config_get reply
    // run, so the app can render without having to ask first.
    void OnConnected();

    // The link went down. Any half-received config run is discarded -- an
    // interrupted run must never be applied (spec 4.2).
    void OnDisconnected();

    /*
     * Spec 4.3's `event`: one recognized gesture, reported to the app.
     *
     * **Public because the orchestrator owns the recognition, not the command
     * path.** A gesture is resolved in the poll loop's own timing, so there is no
     * inbound command whose reply could carry it -- the orchestrator calls this
     * through `SystemOrchestrator::SetGestureSink`.
     *
     * It goes through `Emit` rather than the sink directly so the frame's `seq`
     * comes from the same counter as every other outbound frame; a second writer
     * with its own counter is how the app comes to see out-of-order sequence
     * numbers and report a link gap that never happened.
     */
    void EmitGesture(const SystemOrchestrator::GestureEventRecord &ev);

    // Emit anything deferred. A config reply larger than the frame cap is
    // emitted ONE CHUNK PER CALL rather than in a burst, so the transport's TX
    // buffer cannot overflow and no single call blocks the poll loop.
    void Process();

    // Periodic status (spec 4.4: every 2 s while connected).
    void SendStatus();

    // The app must not outrun the head unit's key recognition.
    static constexpr uint32_t kDefaultTestKeyHoldMs = 200;

    uint32_t LastSeenSeqSent() const { return seq_sent_; }
    uint32_t LastSeenSeqReceived() const { return expected_seq_ - 1; }

private:
    // One place that writes a frame, so `seq_sent_` cannot be forgotten on one
    // path and the sink contract (no trailing newline) is enforced once.
    void Emit(const char *type, const char *body_fields);

    void HandleConfigBegin(const cJSON *root, uint32_t for_seq);
    void HandleConfigChunk(const cJSON *root, uint32_t for_seq);
    void HandleConfigEnd(const cJSON *root, uint32_t for_seq);
    void HandleConfigPatch(const cJSON *root, uint32_t for_seq);
    void HandleTestKey(const cJSON *root, uint32_t for_seq);
    void HandleIdentify(const cJSON *root, uint32_t for_seq);
    void HandleReboot(const cJSON *root, uint32_t for_seq);
    // Learn mode (FR-5, FR-28..FR-31). learn_start opens FR-5's live stream;
    // learn_commit turns the accumulated samples into a stored button.
    void HandleLearnStart(const cJSON *root, uint32_t for_seq);
    void HandleLearnStop(const cJSON *root, uint32_t for_seq);
    void HandleLearnCommit(const cJSON *root, uint32_t for_seq);
    // Replies to `ping`/`status` request and to `hello`.
    void ReplyStatus(uint32_t for_seq);
    void BeginConfigReplyRun();

    // A nack names the failing check, because "config is corrupt" is not
    // actionable and the two checks fail for different reasons.
    void Nack(uint32_t for_seq, const char *err, const char *detail);

    void ResetRun();

    // FR-5's live sample, one per Process() while a run is open.
    void EmitLadderSample();

    IHAL              *hal_;
    SystemOrchestrator *sys_;
    ConfigStore       *store_;
    FrameSink          sink_ = nullptr;
    void              *sink_ctx_ = nullptr;

    uint32_t seq_sent_ = 0;
    // The next `seq` we expect from the peer. Per-sender monotonic (spec 4.2),
    // so a gap means a dropped frame and is surfaced as an `event`.
    uint32_t expected_seq_ = 1;

    // --- chunked config run (spec 4.2) -------------------------------------
    // A FIXED member, not a heap allocation: spec 4.2 requires the staging
    // buffer be sized to the maximum legal config as a compile-time constant, so
    // the bound is provable rather than hoped for. A heap buffer sized from the
    // peer's total_len is an overflow primitive driven from outside.
    uint8_t  staging_[ConfigMaxSerializedSize()];
    size_t   staging_len_ = 0;      // bytes landed so far
    size_t   expected_len_ = 0;     // total_len from config_begin
    uint32_t expected_crc_ = 0;
    uint64_t last_chunk_ms_ = 0;
    bool     run_open_ = false;

    // --- outbound reply run ------------------------------------------------
    // The reply is generated lazily, one frame per Process(), so a 22 KB config
    // does not need a 22 KB TX buffer nor a blocking burst.
    char     reply_buf_[ConfigMaxSerializedSize()];
    size_t   reply_len_ = 0;
    size_t   reply_off_ = 0;
    uint32_t reply_seq_ = 0;
    bool     reply_open_ = false;
    // config_begin goes out exactly once per run. Keyed off the offset instead,
    // it re-emitted on every Process() call and the run never advanced.
    bool     reply_sent_begin_ = false;

    // Whether ANY frame has been received yet. The peer's starting `seq` is not
    // known at connect, so the first frame establishes the baseline; without
    // this every first frame looks like a gap and emits a spurious link_gap.
    bool     seen_any_ = false;

    // --- learn run (FR-5) ---------------------------------------------------
    // A learn run is an OPEN STREAM, not a request/response: the app sends
    // learn_start, receives ladder_sample frames, then sends learn_commit. The
    // session accumulates what the stream reports.
    LearnSession session_;
    bool         learn_open_ = false;
    int          learn_channel_ = 0;
    uint32_t     learn_samples_ = 0;
};
