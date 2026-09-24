#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#include "Config/ConfigCodec.h"
#include "Config/ConfigStore.h"
#include "HAL/IHAL.h"
#include "Learning/LearnSession.h"
#include "Link/Ndjson.h"
#include "Maintenance/MaintenanceInfo.h"
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

class UsbCdc;

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

    /*
     * Publish the maintenance window's facts to the app (spec 8.3 option 1).
     *
     * Called by the poll loop with what the device-only radio module reports. The
     * router emits a `maintenance` frame whenever the state CHANGES -- including
     * the first time, and including a change back to closed -- so the app sees the
     * window open and close without polling. A no-op when nothing changed, because
     * this is called every tick.
     *
     * **Why the router and not the radio module.** The radio module is device-only
     * and cannot emit a frame; the frame path and the sink live here, and this file
     * is host-tested, so the emission (the value the app actually receives) is
     * covered rather than merely wired.
     */
    void SetMaintenanceInfo(const MaintenanceInfo &info, uint32_t failures);

    // The window's facts as last published, so a caller can assert them and so a
    // connecting app can be sent the current state.
    const MaintenanceInfo &MaintenanceState() const { return maintenance_info_; }

    /*
     * A synchronous TX flush, for the one reply that must leave BEFORE the device
     * stops servicing the poll loop: the reboot ack.
     *
     * `Emit` only QUEUES into the transport; `UsbCdc::ServiceTx` -- the thing that
     * actually writes bytes to the USB FIFO -- runs on the poll loop, and
     * `HandleReboot` calls `hal_->reboot()` in the same call. So on the device
     * path nothing between the `Emit` and the reset ever drains the buffer, and
     * the ack is lost with the reset: the app cannot tell a successful reboot from
     * a dropped link, which is the exact outcome the ordering exists to prevent.
     * A host test could not see it because its capture sink records at `Emit`
     * time. Measured 2026-09-22: at `reboot()`, 0 bytes had reached the transport.
     */
    using TxFlush = void (*)(void *ctx);
    void SetTxFlush(TxFlush flush, void *ctx);

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

    /*
     * The highest seq RECEIVED from the peer, or 0 when none has been.
     *
     * **The `seen_any_` guard is the fix (N-65).** The field is `expected_seq_ - 1`,
     * and `expected_seq_` starts at 1, so before the first frame this would have
     * reported "seq 0 was received" when nothing had been. No caller read it, so no
     * wrong behaviour shipped -- but a link-diagnostics view is the obvious future
     * consumer, and it would have read a value false exactly in the case it is most
     * likely to be asked ("has anything arrived yet?").
     */
    uint32_t LastSeenSeqReceived() const { return seen_any_ ? expected_seq_ - 1 : 0; }

    /*
     * The two link-loss counters, reported in the `status` body.
     *
     * Both were countable and READ BY NOBODY: `UsbCdc::DroppedFrames()` (an
     * outbound frame the TX buffer refused) had a test as its only reader, while
     * its own doc-comment says a silent drop "is the failure this class exists to
     * prevent, so it must be observable"; `RxOverflows()` (staged input the ring
     * refused) had a test and not even that claim. What was missing is
     * reachability TO A USER, not the counting -- and the only channel the router
     * has to a user is a frame it emits. That is why these are pushed in at the
     * wiring point rather than pulled at each `Send`, which cannot report anything
     * (the sink returns void, so a refused frame fails entirely inside the
     * transport and nothing returns to the caller that asked for it).
     *
     * Both default to zero, so a router used in isolation reads zero.
     */
    void SetLossCounters(const UsbCdc *cdc) { cdc_ = cdc; }

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
    // Records the level the stream is reporting into `session_`, so the samples
    // learn_commit accepts are the ones the app saw (spec 4.3).
    void RecordLearnSample();
    // FR-33: the two USB maintenance triggers. The window itself is the
    // orchestrator's; the radio is device-only work elsewhere.
    void HandleMaintenanceEnter(uint32_t for_seq);
    void HandleMaintenanceExit(uint32_t for_seq);
    // USB OTA (spec 9.3, FR-36/FR-41). The frames are thin adapters over
    // `Update/OtaUsb`: `ota_begin` opens the run, `ota_chunk` appends and feeds
    // the verifier, `ota_end` verifies and (only then) commits. Neither path
    // re-implements the gate -- the WiFi path calls the same three functions.
    void HandleOtaBegin(const cJSON *root, uint32_t for_seq);
    void HandleOtaChunk(const cJSON *root, uint32_t for_seq);
    void HandleOtaEnd(uint32_t for_seq);
    // Replies to `ping`/`status` request: the same body as the periodic status,
    // but carrying the `for_seq` that makes it a reply.
    void ReplyStatus(uint32_t for_seq);
    // Spec 4.4's periodic status. Private: it is an implementation of `Tick()`,
    // not part of the router's surface.
    void SendStatus();
    // The single writer of the `status` body, parameterized by whether the frame
    // is a REPLY (carrying `for_seq`) or the unsolicited periodic keepalive
    // (carrying none -- `for_seq` names the frame being answered, and a keepalive
    // answers nothing).
    void EmitStatusBody(bool with_for_seq, uint32_t for_seq);
    void BeginConfigReplyRun();

    // A nack names the failing check, because "config is corrupt" is not
    // actionable and the two checks fail for different reasons.
    void Nack(uint32_t for_seq, const char *err, const char *detail);

    void ResetRun();

    // EVERY piece of state that belongs to the LINK rather than to the device,
    // discarded in one place. Two callers end a link -- `OnDisconnected` (a real
    // transport event) and `NoteSilenceIfStale` (the peer went quiet for 10 s) --
    // and spec 4.4 makes the same promise for both: a link drop leaves no
    // half-finished work behind. Kept as one function because the two callers had
    // DRIFTED: the silence reap closed the `config_set` run and the learn STREAM
    // but left the `config_get` REPLY run open, so `Process` -- which gates only on
    // `reply_open_`, never on `connected_` -- kept emitting `config_chunk` into a
    // FIFO nobody was draining until the 2 KB TX buffer filled, and a genuine
    // reply was then refused. The learn SESSION was left behind by BOTH paths, so
    // a `learn_commit` after a reconnect committed samples the previous session
    // recorded (see `ForgetLearnSession`).
    void ResetLinkState();

    // A learn stream and the samples it accumulated, discarded together. The
    // session must be emptied, not merely left with `learn_open_` cleared: the
    // commit path is keyed on the SESSION's channel (see `HandleLearnCommit`), and
    // a session that survives the link lets a peer with no open stream commit the
    // PREVIOUS session's measurement -- `session_.Commit` would accept it and stamp
    // `learned_idle_mv` from a rail measured before the disconnect (spec 4.4 makes
    // reconnect stateless). `Start` with an empty neighbour set is what zeroes the
    // session, so calling it here is the discard.
    void ForgetLearnSession();

    // FR-5's live sample, one per Process() while a run is open.
    void EmitLadderSample();

    IHAL              *hal_;
    SystemOrchestrator *sys_;
    ConfigStore       *store_;
    FrameSink          sink_ = nullptr;
    void              *sink_ctx_ = nullptr;
    TxFlush            tx_flush_ = nullptr;
    void              *tx_flush_ctx_ = nullptr;

    uint32_t seq_sent_ = 0;

    // The maintenance window's facts as last PUBLISHED (spec 8.3 option 1), and
    // whether anything has been published yet. The first publish always emits, so
    // an app connecting into an already-open window learns the state without
    // asking; after that only a CHANGE emits, because `SetMaintenanceInfo` runs
    // every poll tick.
    //
    // Held by value rather than as a pointer into the radio module: the radio is
    // device-only and its state is not visible to this host-testable file, so the
    // router stores what it was told. A test sets it directly.
    MaintenanceInfo maintenance_info_{};
    uint32_t        maintenance_failures_ = 0;
    bool            maintenance_published_ = false;
    // The last values actually EMITTED, compared against on every call. Kept
    // separately from `maintenance_info_` because the two differ exactly between a
    // call that changed the state and the emit that follows it -- and on a link
    // that is down, `Emit` writes nothing, so the snapshot must advance only when
    // a frame really went out.
    MaintenanceInfo published_info_{};
    uint32_t        published_failures_ = 0;
    // Emits the frame if anything changed (or nothing has been sent yet). One
    // home, so `SetMaintenanceInfo` and `OnConnected` cannot disagree about the
    // wire shape.
    void PublishMaintenanceIfChanged();
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
    // The config a `config_get` reply is encoded FROM. A member rather than a
    // local in `BeginConfigReplyRun`: `sizeof(Config)` is 8,912 B and that
    // function runs on the 4 KB TinyUSB task, where a local Config (its own
    // 17,856-byte frame, ~35.8 KB with its Load chain) overflows the stack. It
    // also has to outlive the call, since the reply goes out one chunk per
    // `Process()` -- but `reply_buf_` is what the chunks are read from, so this
    // only needs to hold the source for the encode.
    Config   reply_config_{};
    // The scratch every INBOUND handler that needs a whole config works in
    // (`config_end`, `config_patch`, `learn_start`'s seed, `learn_commit`). One
    // member for all four because they cannot run concurrently: each is entered
    // from `OnLine`, which is called from the TinyUSB RX callback, so at most one
    // is live at a time -- and none of them is reentrant.
    //
    // A local in any of them is a stack overflow: `sizeof(Config)` is 8,912 B
    // against a 4,096-byte task stack, and each of those handlers also calls
    // `ConfigStore::Load`, whose own decode reaches `ConfigDecodeJson`. Measured
    // frames were 9,088 B (`config_end`) to 17,920 B (`config_patch`), ~35 KB
    // peaking through the Load chain. Invisible on the host, where the suite's
    // stacks are megabytes, and never exercised because the board is unflashed.
    Config   command_config_{};
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
    // -1 MEANS "no session", and it is the value the member starts at. The
    // sentinel must be the initializer, not merely a value `ForgetLearnSession`
    // assigns: `HandleLearnCommit` refuses `no_session` on `learn_channel_ < 0`
    // BEFORE comparing it to the frame's channel, so a default of `0` let a
    // `learn_commit{channel:0}` on a freshly constructed router clear BOTH the
    // no-session guard and the channel-match guard, reach `session_.Commit` on an
    // empty session, and answer `learn_rejected: too_few_samples` -- naming a
    // sample count as the cause when the truth is that no stream was ever opened
    // (the un-actionable reason FR-29 forbids). `learn_open_` alone is not the
    // guard: spec 4.3's flow is start -> stream -> STOP -> commit, and a stop
    // leaves the samples and this channel behind on purpose.
    int          learn_channel_ = -1;
    uint32_t     learn_samples_ = 0;

    // The transport whose loss counters `status` reports. Null unless wired;
    // see `SetLossCounters`. Held as a pointer only to READ the two counters.
    const UsbCdc *cdc_ = nullptr;
};
