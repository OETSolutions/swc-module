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

    /*
     * Emit a `log` frame (spec 4.3).
     *
     * **This is the frame three requirements needed and nothing emitted.** It sat
     * in the contract with an "optional, gated by a settings flag" note and no
     * producer, so FR-18's "clamp with a logged warning" and the config-fault row
     * of spec 6.8 had a specified destination and no writer. The severity is
     * carried as a WORD, not a number, for the same reason `gesture` is: the app
     * matches on it, and two spellings of "warn" is a lookup that misses with no
     * error anywhere.
     */
    void EmitLog(const char *level, const char *msg);

    // Emit anything deferred. A config reply larger than the frame cap is
    // emitted ONE CHUNK PER CALL rather than in a burst, so the transport's TX
    // buffer cannot overflow and no single call blocks the poll loop.
    void Process();

    /*
     * Drive the time-based frames. Called once per poll-loop tick by the device
     * link (`UsbLinkService`), because `Process()` is called from tests that do
     * not advance a clock.
     *
     * This is the ONLY caller of `SendStatus()` -- spec 4.4 requires the firmware
     * to send `status` every 2 s while connected, and before this existed the
     * method had no caller at all: the device emitted a status only in reply to
     * `ping`/`status_get`, so the keepalive §4.4 promises was never sent and a
     * quiet app saw nothing until it asked.
     */
    void Tick();

    // The app must not outrun the head unit's key recognition.
    static constexpr uint32_t kDefaultTestKeyHoldMs = 200;
    // The longest `test_key` hold a caller may ask for. A bench command drives the
    // KEY line for the whole hold, so an unbounded value is a way to pin it; this
    // is generous (a second) for a measurement that needs a settled reading.
    static constexpr uint32_t kTestKeyMaxHoldMs = 1000;

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
    // FR-33: the two USB maintenance triggers. The window itself is the
    // orchestrator's; the radio is device-only work elsewhere.
    void HandleMaintenanceEnter(uint32_t for_seq);
    void HandleMaintenanceExit(uint32_t for_seq);
    // Replies to `ping`/`status` request and to `hello`.
    void ReplyStatus(uint32_t for_seq);
    // Spec 4.4's periodic status. Private: it is an implementation of `Tick()`,
    // not part of the router's surface.
    void SendStatus();
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

    // --- periodic status (spec 4.4) ----------------------------------------
    // The link is up: `OnConnected` sets this, `OnDisconnected`/`OnLine` clear
    // it. The periodic status belongs to a CONNECTED link, so a window with no
    // peer does not broadcast into a FIFO nobody reads.
    bool     connected_ = false;
    // Whether the periodic-status clock has been seeded. A SEPARATE flag, not
    // `last_status_ms_ == 0`: the HAL clock legitimately reads 0 at boot, so a
    // zero sentinel would re-seed every tick at time 0 and the status would never
    // fire (caught by the test, not by inspection).
    bool     status_clock_seeded_ = false;
    uint64_t last_status_ms_ = 0;
    static constexpr uint64_t kStatusPeriodMs = 2000;   // spec 4.4

    // --- link-liveness (spec 4.4) ------------------------------------------
    // "After 10 s of silence the firmware considers the link down." The silence
    // is INBOUND: the app is specified to ping at 5 s, so 10 s with no frame
    // means the peer is gone. Before this existed the firmware had no link-down
    // notion at all -- it sent a periodic status every 2 s forever, and the two
    // link-scoped runs (a half-received config_set, an open learn stream) were
    // reaped only by a real disconnect event, which a half-dead USB link never
    // delivers. An app that died mid-`config_set` therefore left the device
    // refusing every later config with `run_open` until the cable was pulled.
    uint64_t last_rx_ms_ = 0;
    bool     rx_clock_seeded_ = false;
    // Set when silence has reaped the link. It stays true until a frame arrives,
    // because that is the only signal that the link recovered WITHOUT a
    // reconnect -- `rx_clock_seeded_` cannot carry it (the reap clears it).
    bool     link_down_ = false;
    static constexpr uint64_t kLinkSilenceMs = 10000;   // spec 4.4

    // Drop the link-scoped runs once inbound silence exceeds `kLinkSilenceMs`,
    // and stop the periodic status until a frame re-arms the link. Idempotent:
    // the reap runs once per link-down, so a second call does nothing.
    void NoteSilenceIfStale(uint64_t now);

    // --- learn run (FR-5) ---------------------------------------------------
    // A learn run is an OPEN STREAM, not a request/response: the app sends
    // learn_start, receives ladder_sample frames, then sends learn_commit. The
    // session accumulates what the stream reports.
    LearnSession session_;
    bool         learn_open_ = false;
    int          learn_channel_ = 0;
    uint32_t     learn_samples_ = 0;
};
