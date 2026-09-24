#include "Link/CommandRouter.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "Config/ConfigDefaults.h"
#include "Feedback/BuzzerGrammar.h"
#include "Link/UsbCdc.h"
#include "Update/OtaUsb.h"
#include "Util/Base64.h"
#include "Util/FwVersion.h"
#include "Util/Sha256.h"

namespace {

// The frame vocabulary (spec 4.3). A closed set -- an unknown `type` is a peer
// bug and is nacked, never guessed at.
bool IsKnownCommand(const char *type) {
    static const char *kCmds[] = {
        "ping", "config_get", "config_begin", "config_chunk", "config_end",
        "config_patch", "learn_start", "learn_stop", "learn_commit", "test_key",
        "identify", "reboot", "time_sync", "ota_begin", "ota_chunk", "ota_end",
        "maintenance_enter", "maintenance_exit",
    };
    for (const char *c : kCmds) {
        if (strcmp(c, type) == 0) return true;
    }
    return false;
}

const cJSON *Num(const cJSON *root, const char *name) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(v) ? v : nullptr;
}

const cJSON *Str(const cJSON *root, const char *name) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    return (cJSON_IsString(v) && v->valuestring != nullptr) ? v : nullptr;
}

// A peer-supplied number, converted to an integer ONLY after proving it is in
// range. A bare `static_cast` is not a conversion safety net: it truncates a
// fraction, wraps a value past the width, and (for a value past the target's
// range) is undefined behavior. Measured on `config_patch`: `buzzer_level = 259`
// became 3 and `send_duration_ms = 1e10` became 4294967295.
//
// These mirror `ReadU32`/`ReadU8` in ConfigCodec, which refuse the same values --
// one rule for "is this number legal", so a patch and a chunked config_set cannot
// disagree about the config they produce. Fractions are REFUSED rather than
// rounded, because a millisecond count of 2.7 is a caller bug, not 2 ms.
bool NumToU32(const double v, uint32_t *out) {
    if (v < 0.0 || v > 4294967295.0) return false;
    if (v != static_cast<double>(static_cast<uint32_t>(v))) return false;   // fractional
    *out = static_cast<uint32_t>(v);
    return true;
}

bool NumToU8(const double v, uint8_t *out) {
    if (v < 0.0 || v > 255.0) return false;
    if (v != static_cast<double>(static_cast<uint8_t>(v))) return false;
    *out = static_cast<uint8_t>(v);
    return true;
}

// A channel index is read by several handlers that each range-check it against
// `kMaxChannels` afterwards. That check is on the CONVERTED int, so it cannot see
// a fractional part: `channel = 1.9` became 1 and drove the first channel, a
// command accepted as channel 1.9 but executed on channel 1. Same rule as the
// codec's integer fields -- refuse the fraction, do not round it.
bool NumToChannel(const double v, uint8_t *out) {
    if (v < 0.0 || v >= static_cast<double>(kMaxChannels)) return false;
    if (v != static_cast<double>(static_cast<uint8_t>(v))) return false;
    *out = static_cast<uint8_t>(v);
    return true;
}

}  // namespace

CommandRouter::CommandRouter(IHAL *hal, SystemOrchestrator *sys, ConfigStore *store)
    : hal_(hal), sys_(sys), store_(store) {
    memset(staging_, 0, sizeof(staging_));
    memset(reply_buf_, 0, sizeof(reply_buf_));
}

void CommandRouter::SetSink(FrameSink sink, void *ctx) {
    sink_ = sink;
    sink_ctx_ = ctx;
}

void CommandRouter::SetTxFlush(TxFlush flush, void *ctx) {
    tx_flush_ = flush;
    tx_flush_ctx_ = ctx;
}

void CommandRouter::Emit(const char *type, const char *body_fields) {
    if (sink_ == nullptr) return;
    NdjsonWriter w;
    w.Write(type, seq_sent_++, body_fields);
    // The sink contract: exactly one frame, WITHOUT its trailing newline. The
    // transport owns the newline.
    sink_(sink_ctx_, w.Line(), w.LineLen() - 1);
}

void CommandRouter::Nack(uint32_t for_seq, const char *err, const char *detail) {
    // `detail` is often a PEER-SUPPLIED string echoed back (`h.type` for an
    // unknown type, `path` for an unknown patch path, `pattern` for an unknown
    // identify pattern), so it is copied with its JSON quoting -- and any control
    // byte -- replaced by `_`, and BOUNDED.
    //
    // Both halves are load-bearing. An embedded `"` closed the detail string
    // early: measured, a frame whose `type` carried a quote produced
    // `..."detail":"bo"gus"}` -- a nack that no longer parses as JSON, so the app
    // lost the error it was being sent and saw only a malformed line. And an
    // unbounded `%s` of a long value let `snprintf` cut the body mid-string,
    // which is invalid JSON for the same reason. This is the sanitisation
    // `EmitLog` and `EmitGesture` already apply to their peer-facing strings; the
    // nack path simply omitted it. `err` is a static literal at every call site,
    // so only `detail` needs this.
    char det[192];
    size_t n = 0;
    while (n + 1 < sizeof(det) && detail != nullptr && detail[n] != '\0') {
        const char c = detail[n];
        det[n] = (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20u) ? '_' : c;
        ++n;
    }
    det[n] = '\0';
    // Sized so the longest realistic `err` can never truncate the body: with the
    // detail bounded to 192 and the wrapper ~43 bytes, this leaves ~85 bytes for
    // `err` (the longest today is 17), so adding an error code cannot silently
    // reintroduce the mid-string cut this function was fixed to remove.
    char body[320];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"err\":\"%s\",\"detail\":\"%s\"",
             static_cast<unsigned>(for_seq), err, det);
    Emit("nack", body);
}

void CommandRouter::ResetRun() {
    staging_len_ = 0;
    expected_len_ = 0;
    expected_crc_ = 0;
    run_open_ = false;
}

void CommandRouter::ForgetLearnSession() {
    // `Start` zeroes every accumulator; the neighbour set is irrelevant because
    // nothing will be committed from this session. See the header for why the
    // samples -- not just `learn_open_` -- must go.
    session_.Start(LadderProfile{});
    learn_open_ = false;
    learn_channel_ = -1;
    learn_samples_ = 0;
}

void CommandRouter::ResetLinkState() {
    // An interrupted run is discarded wholesale: a partial config is never
    // applied (spec 4.2).
    ResetRun();
    // The `config_get` REPLY run is link-scoped for the same reason: `Process`
    // emits one chunk per call and gates on `reply_open_` alone, so a run left
    // open past the end of its link streams the rest of a reply to nobody, filling
    // the TX buffer until a real reply is refused. `OnDisconnected` always cleared
    // this; the silence reap did not -- that drift is what made this one function.
    reply_open_ = false;
    reply_off_ = 0;
    reply_len_ = 0;
    reply_sent_begin_ = false;
    // The learn stream AND its samples: the app session that opened the stream is
    // what ends it, and a commit must not run on a dead stream's measurements.
    ForgetLearnSession();
}

void CommandRouter::OnDisconnected() {
    // Every link-scoped run is discarded in one place (spec 4.2, spec 4.4): a
    // partial config is never applied, a half-sent reply is not resumed, and a
    // learn session does not survive the session that opened it.
    ResetLinkState();
    // Reconnect is stateless (spec 4.4), so the peer's sequence baseline is
    // re-established by the next frame rather than remembered across a link drop.
    seen_any_ = false;
    // No peer, so no periodic status: a status written now would sit in a FIFO
    // nobody is draining.
    connected_ = false;
    // The liveness clock restarts with the link; the next frame seeds it again.
    rx_clock_seeded_ = false;
    last_rx_ms_ = 0;
    link_down_ = false;
}

void CommandRouter::OnConnected() {
    connected_ = true;
    // A fresh link has no silence history: seed the liveness clock on the frame
    // that follows rather than letting a stale `last_rx_ms_` from a previous
    // connection reap the new one.
    rx_clock_seeded_ = false;
    last_rx_ms_ = 0;
    link_down_ = false;
    // Seed the periodic-status clock so a status is not sent on the same tick as
    // `hello`; the first periodic status lands one period after connect.
    status_clock_seeded_ = false;
    // hello first (spec 4.5's version negotiation), then the config run so the
    // app can render immediately without asking.
    //
    // `caps` lists what this build can ACTUALLY do, not what the product will
    // eventually do. It used to OMIT "ota" while the dispatch below nacked every
    // `ota_*` frame as `not_implemented` (spec open item N-14) -- a client that
    // trusted `caps[]` would decline to offer an update flow that was in fact
    // unavailable. `ota` is now advertised because the router IS wired to
    // `OtaUsb`; the guard against re-drifting is the test
    // `HelloAdvertisesOtaOnlyIfTheDispatcherImplementsIt`, which asserts this
    // string against the dispatcher's real behaviour rather than against a second
    // hardcoded copy.
    char body[192];
    snprintf(body, sizeof(body),
             "\"fw_version\":\"%s\",\"hw_id\":\"SWC-S3\",\"protocol_v\":%u,\"caps\":[\"config\",\"learn\",\"ota\"]",
             FwVersionString(), static_cast<unsigned>(kNdjsonProtocolVersion));
    Emit("hello", body);
    // A connecting app must learn the window's state without asking. `hello`
    // carries no maintenance fields (spec 4.3), so the frame that does is sent
    // here -- which is also the only place that reaches an app which did not see
    // the window OPEN (it was already open, or the app was not attached yet).
    // Forced by clearing the published flag, so the unchanged-state short circuit
    // does not suppress it.
    maintenance_published_ = false;
    PublishMaintenanceIfChanged();
    BeginConfigReplyRun();
}

void CommandRouter::BeginConfigReplyRun() {
    // Encode the CURRENT config -- stored if there is one, else the shared
    // default. Encoding the stored blob instead would put the wire form and the
    // NVS form on two different code paths that could drift.
    //
    // **The scratch is a member, not a local.** `sizeof(Config)` is 8,912 B, and
    // this runs on the 4,096-byte TinyUSB task (it is reached from `OnConnected`
    // on the RX callback). As a local it measured a 17,856-byte frame, and with
    // its `Load` -> `ConfigDecodeJson` chain the peak was ~35.8 KB -- a stack
    // overflow the moment a host connects. A member is also what makes it legal
    // to keep the config across the chunked reply, which is emitted one frame per
    // `Process()` and so cannot hold a local.
    Config &c = reply_config_;
    ConfigDefault(&c);
    if (store_ != nullptr) {
        // `kRecoveredFromBackup` IS a loaded config: it is what `Boot` is running
        // (spec 6.8 makes the other slot authoritative after a torn write), so
        // replying with defaults here would draw an empty app over a configured
        // device -- and the app's next save would then overwrite the recovered
        // config. Only a synthesized default (`kFellBackToDefaults`) or an absent
        // config (`kNoConfig`) is not the user's.
        if (!ConfigLoadResultIsUsable(store_->Load(&c))) ConfigDefault(&c);
    }
    reply_len_ = ConfigEncodeJson(c, reply_buf_, sizeof(reply_buf_));
    reply_off_ = 0;
    reply_sent_begin_ = false;
    reply_seq_ = seq_sent_;
    reply_open_ = (reply_len_ > 0);
    if (reply_len_ == 0) {
        // Encoding failed, which means the config did not fit the bound. That is
        // a firmware bug, so say so rather than silently sending nothing.
        Nack(reply_seq_, "encode_failed", "config exceeds ConfigMaxSerializedSize");
    }
}

void CommandRouter::Process() {
    // ONE frame per call, and the CONFIG REPLY WINS when both are pending: a reply
    // answers a request the peer is blocked on, whereas the learn stream is
    // continuous and can resume on the next call. So the learn sample is emitted
    // only when no reply run is in flight -- `!reply_open_` below.
    //
    // An earlier comment here said the opposite ("a learn run streams whether or
    // not a config reply is in flight"), which is not what the guard does. The code
    // is the intended behaviour -- giving the stream priority instead would stall a
    // `config_get` reply for as long as a learn stayed open, and the reply is
    // finite (its own chunks drain in one-frame-per-call steps) so the pause it
    // imposes on the stream is bounded. The comment was corrected rather than the
    // guard, because a reader who "fixed" the guard to match it would strand the
    // reply.
    if (learn_open_ && !reply_open_) {
        EmitLadderSample();
        // The streamed sample is RECORDED, not merely sent. Spec 4.3 and this
        // router's own header both say learn_commit "accepts the streamed
        // samples": the session is the accumulator that makes that true. Without
        // this the sample went out over the wire and nowhere else -- measured, a
        // 30-frame stream followed by one learn_commit returned
        // `learn_rejected: too_few_samples`, because AddSample was called only
        // from the commit itself (once) while `LearnSession::Commit` requires
        // >=10 samples over >=100 ms. The protocol the spec describes could not
        // succeed at all; the host suite masked it by firing 20 learn_commit
        // frames, which is not the flow anyone would write.
        RecordLearnSample();
        ++learn_samples_;
        return;
    }
    if (!reply_open_) return;

    // config_begin is emitted EXACTLY ONCE. Keying it off `reply_off_ == 0`
    // re-emitted it on every call, because the offset does not advance until a
    // chunk goes out -- so the run never got past its own header.
    if (!reply_sent_begin_) {
        char body[96];
        snprintf(body, sizeof(body), "\"total_len\":%u,\"crc32\":%u",
                 static_cast<unsigned>(reply_len_),
                 static_cast<unsigned>(Crc32(reinterpret_cast<const uint8_t *>(reply_buf_),
                                             reply_len_)));
        Emit("config_begin", body);
        reply_sent_begin_ = true;
        return;   // one frame per call
    }

    const size_t remaining = reply_len_ - reply_off_;
    if (remaining > 0) {
        const size_t n = remaining < kConfigWireChunkBytes ? remaining : kConfigWireChunkBytes;
        // The base64 text and the frame body are FILE-LOCAL STATICS, not locals.
        // Together they are 2,112 B of scratch, which made this function's frame
        // 2,352 B -- and `Process` runs on the 3,584-byte main task, three frames
        // below `Emit` (1,072 B) and `Nack` (544 B) on the same path. The chain
        // measured 4,080 B against that 3,584-byte stack: a guaranteed overflow
        // the first time a `config_get` reply was chunked out. BSS is where the
        // rest of this class's scratch already lives (`reply_buf_`, `staging_`),
        // and `Process` is the only writer of either buffer.
        static char b64[1024];
        const size_t bl = Base64Encode(reinterpret_cast<const uint8_t *>(reply_buf_ + reply_off_),
                                       n, b64, sizeof(b64));
        if (bl == 0) {
            Nack(reply_seq_, "encode_failed", "chunk did not fit the frame");
            reply_open_ = false;
            return;
        }
        static char body[sizeof(b64) + 64];
        snprintf(body, sizeof(body), "\"offset\":%u,\"data_b64\":\"%s\"",
                 static_cast<unsigned>(reply_off_), b64);
        reply_off_ += n;
        Emit("config_chunk", body);
        return;   // one frame per call
    }

    // All chunks are out; close the run with the digest.
    char hex[65];
    Sha256Hex(reinterpret_cast<const uint8_t *>(reply_buf_), reply_len_, hex);
    char body[128];
    snprintf(body, sizeof(body), "\"sha256\":\"%s\"", hex);
    Emit("config_end", body);
    reply_open_ = false;
}

void CommandRouter::ReplyStatus(uint32_t for_seq) {
    EmitStatusBody(true, for_seq);
}

void CommandRouter::SendStatus() {
    // The periodic keepalive replies to NOTHING, so it carries no `for_seq`.
    //
    // It used to pass the firmware's own outbound counter (`seq_sent_`). That
    // made every unsolicited status look like the answer to whatever request
    // happened to share that number: the app completes a pending request on a
    // matching `for_seq`, and both counters start near zero, so a keepalive that
    // landed between a nacked `config_chunk` and its own nack could complete the
    // waiter first and report the refusal as success -- "config saved" for a
    // config the device never accepted. `for_seq` is a reply field; an
    // unsolicited frame must not guess at one.
    EmitStatusBody(false, 0);
}

void CommandRouter::EmitStatusBody(bool with_for_seq, uint32_t for_seq) {
    const bool vbus = (hal_ != nullptr) && hal_->gpio_read(hal_->ctx, GPIO_VBUS_VALID);
    // `config_state` reports the CONFIG's state, not the output's. It used to be
    // derived from `SafeIdleEstablished()`, which answers "is the KEY line safe"
    // -- so a device whose config fell back to defaults, the exact case spec 6.8
    // requires be reported, answered `"ok"` and the app could not tell a healthy
    // config from a lost one. The output's own state is `output_safe`, a
    // separate field, and neither name can now be read as the other.
    const char *cfg = (sys_ != nullptr) ? sys_->ConfigStateWord() : "unknown";
    char reply_field[32];
    if (with_for_seq) {
        snprintf(reply_field, sizeof(reply_field), "\"for_seq\":%u,", static_cast<unsigned>(for_seq));
    } else {
        reply_field[0] = '\0';
    }
    // The two link-loss counters. Emitted on EVERY status, including the
    // periodic keepalive, because they are not a reply to anything -- they
    // describe the link, and the periodic frame is the app's one guaranteed
    // chance to notice a degrading cable without asking.
    //
    // A refused INBOUND frame is counted by the transport and then, until now,
    // dropped on the floor: `RxOverflows` had a test and nothing else, so an app
    // whose burst overflowed the staging ring saw a command silently vanish --
    // the exact `link_gap` failure spec 4.3's frame exists to make visible, one
    // layer down. `tx_dropped` is N-24's outbound twin.
    const uint32_t tx_dropped = (cdc_ != nullptr) ? cdc_->DroppedFrames() : 0u;
    const uint32_t rx_overflows = (cdc_ != nullptr) ? cdc_->RxOverflows() : 0u;
    // Spec 4.3's two diagnostic fields (open item N-22). `temp_c` is the last
    // good NTC reading, written as a DECIMAL exactly as the config codec writes
    // `temp_c_at_learn` (ConfigCodec.cpp's `AddTenths`): one wire convention for a
    // temperature, so a reader parses both the same way. The device keeps tenths
    // internally; the split is done with integer math because the xtensa `printf`
    // is the newlib-nano one with `%f` disabled, and a `%f` here would print
    // nothing at all on the device while working on the host.
    // Read from the orchestrator rather than sampled here, so formatting a
    // keepalive never triggers an ADC conversion as a side effect -- and the
    // sentinel (no reading) is reported as JSON null, never a fabricated 0 C.
    // `heap_free` comes from the HAL, the only layer that knows the platform's
    // allocator; null means the platform could not answer.
    const int temp_tenths = (sys_ != nullptr) ? sys_->LastNtcTenthsC() : 0;
    const bool have_temp = (sys_ != nullptr) && (temp_tenths != SystemOrchestrator::kTempNotMeasuredTenths);
    char temp_field[24];
    if (have_temp) {
        const int whole = temp_tenths / 10;
        int frac = temp_tenths % 10;
        const char *sign = "";
        if (temp_tenths < 0) {
            // C integer division truncates toward zero, so a negative tenths
            // gives a negative whole and a negative remainder; take the absolute
            // remainder and keep the sign on the whole part so "-0.5" is written
            // "-0.5" and not "0.-5".
            if (frac < 0) frac = -frac;
            if (whole == 0) sign = "-";
            snprintf(temp_field, sizeof(temp_field), "%s%d.%d", sign, whole, frac);
        } else {
            snprintf(temp_field, sizeof(temp_field), "%d.%d", whole, frac);
        }
    } else {
        snprintf(temp_field, sizeof(temp_field), "null");
    }
    const uint32_t heap_free = (hal_ != nullptr && hal_->heap_free != nullptr)
                                   ? hal_->heap_free(hal_->ctx) : 0u;
    // Why the chip last started, from the HAL (spec 4.3). Reported so a peer can
    // see a watchdog/brownout reset, which is otherwise unobservable on this
    // board (the console is on the ROM USB-Serial-JTAG, which the firmware stops
    // writing to once TinyUSB owns the PHY). 0 = "unknown", matching `heap_free`.
    const int reset_reason = (hal_ != nullptr && hal_->reset_reason != nullptr)
                                 ? hal_->reset_reason(hal_->ctx) : 0;
    // Spec 3.2's "fall back AND report that it did": whether the ADC is on its
    // linear approximation instead of the eFuse curve. Surfaced so a blank part is
    // visible to the app and to FR-2's "the calibrated path is applied" check.
    const bool cal_degraded = (hal_ != nullptr && hal_->calibration_degraded != nullptr)
                                  && hal_->calibration_degraded(hal_->ctx);
    // Gain mode is PER CHANNEL (FR-14 selects it per channel; spec 6.2 samples
    // `/SENSEn` per channel), so a 3 V channel and a 5 V channel on the same device
    // legitimately resolve to 1.00 and 1.82 at once and one scalar cannot describe
    // both. Spec N-60. The fields are therefore named by INDEX: `gain_mode_0`, and
    // `gain_mode_1` which is JSON `null` when the running config carries no second
    // channel (the same "null rather than a fabricated value" convention `temp_c`
    // uses), so the frame never claims a mode for a channel that is not there.
    const char *gain0 = (sys_ != nullptr)
                            ? (sys_->ChannelGainMode(0) == GainMode::kAmplified ? "amplified"
                                                                               : "tracking")
                            : "unknown";
    char gain1[16];
    if (sys_ != nullptr && sys_->ChannelCount() > 1) {
        snprintf(gain1, sizeof(gain1), "\"%s\"",
                 sys_->ChannelGainMode(1) == GainMode::kAmplified ? "amplified" : "tracking");
    } else {
        snprintf(gain1, sizeof(gain1), "null");
    }
    char body[448];
    snprintf(body, sizeof(body),
             "%s\"vbus_present\":%s,\"gain_mode_0\":\"%s\",\"gain_mode_1\":%s,"
             "\"uptime_ms\":%llu,"
             "\"config_state\":\"%s\",\"output_safe\":%s,"
             "\"tx_dropped\":%u,\"rx_overflows\":%u,\"temp_c\":%s,\"heap_free\":%u,"
             "\"reset_reason\":%d,\"calibration_degraded\":%s",
             reply_field, vbus ? "true" : "false",
             gain0, gain1,
             static_cast<unsigned long long>(hal_ ? hal_->now_ms(hal_->ctx) : 0ULL),
             cfg,
             ((sys_ != nullptr) && sys_->SafeIdleEstablished()) ? "true" : "false",
             static_cast<unsigned>(tx_dropped), static_cast<unsigned>(rx_overflows),
             temp_field, static_cast<unsigned>(heap_free), reset_reason,
             cal_degraded ? "true" : "false");
    Emit("status", body);
}

/*
 * Spec 8.3 option 1: the BLE PoP and the web token are derived per device and
 * shown to the user over USB, because the board has no display and no printed
 * label to carry either secret. This is that delivery path.
 *
 * **It emits on CHANGE, not every tick.** `SetMaintenanceInfo` is called from the
 * poll loop, so an unconditional emit would flood the link at 100 Hz with a frame
 * that only ever changes when the window opens or closes. The `published_` flag
 * makes the FIRST call emit too, so an app that connects into an already-open
 * window receives the state once `hello` has gone out rather than never.
 */
void CommandRouter::PublishMaintenanceIfChanged() {
    if (maintenance_published_) {
        const MaintenanceInfo &a = maintenance_info_;
        const MaintenanceInfo &b = published_info_;
        const bool same = (a.active == b.active) &&
                          (strcmp(a.pop, b.pop) == 0) &&
                          (strcmp(a.token, b.token) == 0) &&
                          (strcmp(a.page_url, b.page_url) == 0) &&
                          (strcmp(a.ble_name, b.ble_name) == 0) &&
                          (maintenance_failures_ == published_failures_);
        if (same) return;
    }

    char body[512];
    snprintf(body, sizeof(body),
             "\"active\":%s,\"pop\":\"%s\",\"token\":\"%s\",\"page_url\":\"%s\","
             "\"ble_name\":\"%s\",\"ble_failures\":%u,\"trigger\":\"%s\"",
             maintenance_info_.active ? "true" : "false",
             maintenance_info_.pop, maintenance_info_.token,
             maintenance_info_.page_url, maintenance_info_.ble_name,
             static_cast<unsigned>(maintenance_failures_),
             // WHY the window opened (spec N-61). Read from the orchestrator rather
             // than carried on `MaintenanceInfo`, so there is one home for the value
             // (`MaintenanceMode`) instead of a copy a caller must remember to fill
             // -- the N-61 shape, where a recorded value reached nobody. The `active`
             // gate is what keeps the field honest: a closed window reports "none",
             // so a trigger from the previous window cannot be read as this one's.
             (sys_ != nullptr && sys_->MaintenanceActive())
                 ? MaintenanceTriggerName(sys_->MaintenanceTriggeredBy())
                 : "none");
    Emit("maintenance", body);

    published_info_ = maintenance_info_;
    published_failures_ = maintenance_failures_;
    maintenance_published_ = true;
}

void CommandRouter::SetMaintenanceInfo(const MaintenanceInfo &info, uint32_t failures) {
    maintenance_info_ = info;
    maintenance_failures_ = failures;
    // Emitting from here rather than from `Tick` keeps the change and the frame
    // together: the poll loop reports the radio's state as it sees it, and the
    // frame goes out on the transition, not one tick later.
    PublishMaintenanceIfChanged();
}

void CommandRouter::NoteSilenceIfStale(uint64_t now) {
    if (link_down_) return;              // already reaped; idempotent
    if (!rx_clock_seeded_) return;       // no peer has ever spoken
    if (now - last_rx_ms_ <= kLinkSilenceMs) return;
    // Link down (spec 4.4). EVERY link-scoped run is discarded, through the same
    // teardown `OnDisconnected` uses -- a half-received config_set (spec 4.2 -- a
    // partial config is never applied), a half-sent config_get reply, and a learn
    // stream with its samples. Reaping HERE is what recovers an interrupted
    // transfer, because the router's `OnDisconnected` is only called on a real
    // transport event, which a link that merely went quiet does not produce.
    ResetLinkState();
    // Stop the periodic status until a frame re-arms the link: a status written
    // now would sit in a FIFO nobody is draining, exactly as with no peer at all.
    connected_ = false;
    link_down_ = true;
}

void CommandRouter::Tick() {
    if (hal_ == nullptr) return;
    const uint64_t now = hal_->now_ms(hal_->ctx);

    // Link liveness runs BEFORE the connected check, deliberately: the case that
    // matters is the link that was connected and then went quiet, and that state
    // is exactly the one the connected check would skip past.
    NoteSilenceIfStale(now);

    // The periodic status belongs to a CONNECTED link only (spec 4.4). Writing
    // one with no peer would fill the TX buffer with frames nothing drains, and
    // the buffer is small enough that it would then refuse a real reply.
    if (!connected_) return;

    if (!status_clock_seeded_) {
        // First tick of a connection: seed the clock rather than sending, so
        // `hello` and a periodic status do not arrive on the same tick.
        status_clock_seeded_ = true;
        last_status_ms_ = now;
        return;
    }
    if (now - last_status_ms_ < kStatusPeriodMs) return;
    last_status_ms_ = now;
    SendStatus();
}

void CommandRouter::OnLine(const char *line, size_t len) {
    if (line == nullptr || len == 0) return;
    // A frame from the peer proves a peer exists, so the periodic status is
    // armed even if the transport never called `OnConnected` (a test may drive
    // `OnLine` directly; the device path sets this in `OnConnected`).
    connected_ = true;
    // A frame IS the proof of liveness (spec 4.4): it clears a silence that had
    // already gone stale, so a link that recovers without re-enumerating resumes
    // rather than staying marked down until the next connect event.
    if (hal_ != nullptr) {
        const uint64_t now = hal_->now_ms(hal_->ctx);
        last_rx_ms_ = now;
        rx_clock_seeded_ = true;
        if (link_down_) {
            // A frame recovered a link that had gone down, without a reconnect.
            // Recovery is a fresh start: seed the status clock rather than firing
            // a status on the heels of the first frame back. The REAP itself is
            // `Tick`'s -- it runs every poll and is the only thing that needs to
            // time out, so a second reap here would be a branch nothing reaches.
            link_down_ = false;
            status_clock_seeded_ = false;
            connected_ = true;
        }
    }
    if (len > kNdjsonMaxFrame) {
        Nack(0, "bad_frame", "line exceeds the frame cap");
        return;
    }

    FrameHeader h{};
    if (!NdjsonParseEnvelope(line, &h)) {
        // A malformed line is REPORTED, not dropped: silently ignoring it leaves
        // the app waiting for a reply that will never come.
        Nack(0, "bad_frame", "not a frame");
        return;
    }

    // Version mismatch is its own error so the app learns the versions disagree
    // rather than misparsing every field that follows.
    if (h.v != kNdjsonProtocolVersion) {
        Nack(h.seq, "version_mismatch", "protocol version differs");
        return;
    }

    // A gap is surfaced as an event, not tolerated silently. `link_gap` is an
    // event TYPE (spec 4.3 has no such command), carrying both numbers.
    //
    // The FIRST frame only establishes the baseline: the peer's starting `seq`
    // is not known at connect, so comparing against a hardcoded 1 would flag
    // every first frame as a gap.
    if (seen_any_ && h.seq > expected_seq_) {
        char body[96];
        snprintf(body, sizeof(body), "\"channel\":-1,\"button\":\"NONE\",\"gesture\":\"NONE\","
                                     "\"expected_seq\":%u,\"got_seq\":%u",
                 static_cast<unsigned>(expected_seq_), static_cast<unsigned>(h.seq));
        Emit("link_gap", body);
    }
    if (!seen_any_ || h.seq >= expected_seq_) expected_seq_ = h.seq + 1;
    seen_any_ = true;

    if (!IsKnownCommand(h.type)) {
        Nack(h.seq, "unknown_type", h.type);
        return;
    }

    // cJSON is re-parsed here rather than passed through from the envelope
    // parser, which deliberately does not hand out its tree (it deletes it).
    cJSON *root = cJSON_Parse(line);
    if (root == nullptr) {
        Nack(h.seq, "bad_frame", "not a frame");
        return;
    }

    if (strcmp(h.type, "ping") == 0) {
        ReplyStatus(h.seq);
    } else if (strcmp(h.type, "config_get") == 0) {
        BeginConfigReplyRun();
    } else if (strcmp(h.type, "config_begin") == 0) {
        HandleConfigBegin(root, h.seq);
    } else if (strcmp(h.type, "config_chunk") == 0) {
        HandleConfigChunk(root, h.seq);
    } else if (strcmp(h.type, "config_end") == 0) {
        HandleConfigEnd(root, h.seq);
    } else if (strcmp(h.type, "config_patch") == 0) {
        HandleConfigPatch(root, h.seq);
    } else if (strcmp(h.type, "test_key") == 0) {
        HandleTestKey(root, h.seq);
    } else if (strcmp(h.type, "identify") == 0) {
        HandleIdentify(root, h.seq);
    } else if (strcmp(h.type, "reboot") == 0) {
        HandleReboot(root, h.seq);
    } else if (strcmp(h.type, "learn_start") == 0) {
        HandleLearnStart(root, h.seq);
    } else if (strcmp(h.type, "learn_stop") == 0) {
        HandleLearnStop(root, h.seq);
    } else if (strcmp(h.type, "learn_commit") == 0) {
        HandleLearnCommit(root, h.seq);
    } else if (strcmp(h.type, "maintenance_enter") == 0) {
        HandleMaintenanceEnter(h.seq);
    } else if (strcmp(h.type, "maintenance_exit") == 0) {
        HandleMaintenanceExit(h.seq);
    } else if (strcmp(h.type, "ota_begin") == 0) {
        HandleOtaBegin(root, h.seq);
    } else if (strcmp(h.type, "ota_chunk") == 0) {
        HandleOtaChunk(root, h.seq);
    } else if (strcmp(h.type, "ota_end") == 0) {
        HandleOtaEnd(h.seq);
    } else if (strcmp(h.type, "time_sync") == 0) {
        // Accepted and acked: the firmware has no RTC and no wall-clock use, so
        // storing it would be a field nothing reads. Acking is honest -- the
        // frame was understood -- and refusing would make the app think the link
        // is broken.
        char body[64];
        snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(h.seq));
        Emit("ack", body);
    } else {
        // Every known command now has a handler; this is reached only for a type
        // that `IsKnownCommand` accepted and the dispatch above does not name,
        // i.e. the two lists have drifted. Saying `not_implemented` names the
        // frame rather than failing silently.
        Nack(h.seq, "not_implemented", h.type);
    }

    cJSON_Delete(root);
}

void CommandRouter::HandleConfigBegin(const cJSON *root, uint32_t for_seq) {
    const cJSON *total = Num(root, "total_len");
    const cJSON *crc = Num(root, "crc32");
    if (total == nullptr || crc == nullptr) {
        Nack(for_seq, "bad_frame", "config_begin needs total_len and crc32");
        return;
    }
    if (run_open_) {
        // Two runs interleaved would splice two configs into one buffer whose
        // CRC passes -- over the wrong bytes.
        Nack(for_seq, "run_open", "a config run is already in progress");
        return;
    }
    const double tl = total->valuedouble;
    // RANGE FIRST, and written so it also rejects a non-finite value. The order
    // matters: the integrality test below applies `static_cast<size_t>`, and
    // casting a value outside `size_t`'s range is UNDEFINED BEHAVIOUR (measured:
    // `(size_t)inf` returned 8443871320 on this toolchain). A JSON number literal
    // cannot be NaN, but it CAN overflow to +inf -- cJSON parses with `strtod` and
    // does not check `ERANGE`, so `total_len: 1e999` arrives as `inf`. The
    // original guard (`tl < 0.0 || tl > max`) happened to catch `inf` and return
    // before the cast, so it was safe by ordering; putting the integrality check
    // first would have handed `inf` to the cast. `!(tl >= 0.0 && tl <= max)` is
    // the NaN-safe form: for NaN both comparisons are false, so the negation is
    // true and it is refused rather than falling through to the cast. Up front,
    // from the declared length: the staging buffer is fixed and a peer-supplied
    // length must never size it.
    if (!(tl >= 0.0 && tl <= static_cast<double>(ConfigMaxSerializedSize()))) {
        Nack(for_seq, "too_large", "total_len exceeds the staging buffer");
        return;
    }
    // Now the value is finite and in `[0, max]`, so the cast is defined.
    // `total_len` is a BYTE COUNT, and the same rule that makes `NumToU32` refuse
    // a fraction applies at this size: a bare `static_cast<size_t>` truncates it,
    // so `total_len: 500.9` was acked as 500 and the sender's own number was not
    // the one the receiver used. Refused rather than rounded -- a byte count of
    // 500.9 is a caller bug, not 500 (see `NumToU32`).
    if (tl != static_cast<double>(static_cast<size_t>(tl))) {
        Nack(for_seq, "bad_frame", "total_len must be a whole number of bytes");
        return;
    }
    ResetRun();
    expected_len_ = static_cast<size_t>(tl);
    // The CRC is a uint32 and arrived unbounded, so a peer-sent `crc32 = 1e10`
    // was cast to a wrapped 32-bit value that no real CRC equals -- every chunk
    // would then be rejected as corrupt with no visible reason. Bounded here so
    // the peer's own number is what the final compare uses.
    if (!NumToU32(crc->valuedouble, &expected_crc_)) {
        Nack(for_seq, "bad_frame", "crc32 out of range");
        return;
    }
    run_open_ = true;
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleConfigChunk(const cJSON *root, uint32_t for_seq) {
    if (!run_open_) {
        Nack(for_seq, "no_run", "config_chunk without config_begin");
        return;
    }
    const cJSON *off = Num(root, "offset");
    const cJSON *data = Str(root, "data_b64");
    if (off == nullptr || data == nullptr) {
        Nack(for_seq, "bad_frame", "config_chunk needs offset and data_b64");
        return;
    }
    const double od = off->valuedouble;
    // RANGE FIRST, same reason as `total_len`: the integrality test below casts
    // to `size_t`, and casting a value outside its range (including the `inf` a
    // `1e999` literal overflows to) is UNDEFINED BEHAVIOUR. The NaN-safe negated
    // form refuses a non-finite value instead of letting it reach the cast.
    if (!(od >= 0.0 && od <= static_cast<double>(ConfigMaxSerializedSize()))) {
        Nack(for_seq, "bad_offset", "offset out of range");
        return;
    }
    // Same rule as `total_len`: an offset is a byte index, and a fraction must be
    // refused rather than truncated. Measured on the sibling handlers, a bare cast
    // made `offset: 9.5` satisfy the contiguity test at 9 -- the frame said one
    // thing and the device did another, which is exactly what `NumToU32`'s integer
    // check exists to prevent.
    if (od != static_cast<double>(static_cast<size_t>(od))) {
        Nack(for_seq, "bad_frame", "offset must be a whole number of bytes");
        return;
    }
    // `offset` is what makes a gap or an overlap detectable. A receiver that
    // ignores it silently splices two runs into a config that passes its own CRC.
    if (static_cast<size_t>(od) != staging_len_) {
        Nack(for_seq, "gap", "chunk offset is not the next expected byte");
        ResetRun();
        return;
    }

    uint8_t decoded[kConfigWireChunkBytes];
    size_t dn = 0;
    if (!Base64Decode(data->valuestring, strlen(data->valuestring), decoded, sizeof(decoded), &dn)) {
        Nack(for_seq, "bad_frame", "data_b64 is not valid base64");
        ResetRun();
        return;
    }
    if (staging_len_ + dn > sizeof(staging_)) {
        Nack(for_seq, "too_large", "run exceeds the staging buffer");
        ResetRun();
        return;
    }
    memcpy(staging_ + staging_len_, decoded, dn);
    staging_len_ += dn;

    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleConfigEnd(const cJSON *root, uint32_t for_seq) {
    if (!run_open_) {
        Nack(for_seq, "no_run", "config_end without config_begin");
        return;
    }
    const cJSON *sha = Str(root, "sha256");
    if (sha == nullptr) {
        Nack(for_seq, "bad_frame", "config_end needs sha256");
        ResetRun();
        return;
    }

    // The length the run actually reached must match what config_begin declared;
    // otherwise the CRC below is being taken over a short buffer.
    if (staging_len_ != expected_len_) {
        Nack(for_seq, "length_mismatch", "staged bytes do not match total_len");
        ResetRun();
        return;
    }

    // CRC FIRST, then SHA-256. Both are checked and neither is optional: the CRC
    // is the transfer check and the SHA-256 the integrity check, and a run can
    // pass one while failing the other. The order fixes which one is blamed.
    const uint32_t crc = Crc32(staging_, staging_len_);
    if (crc != expected_crc_) {
        Nack(for_seq, "crc", "crc32 mismatch");
        ResetRun();
        return;
    }
    char hex[65];
    Sha256Hex(staging_, staging_len_, hex);
    if (strncmp(hex, sha->valuestring, 64) != 0) {
        Nack(for_seq, "sha256", "sha256 mismatch");
        ResetRun();
        return;
    }

    Config &c = command_config_;
    c = Config{};
    if (!ConfigDecodeJson(reinterpret_cast<const char *>(staging_), staging_len_, &c)) {
        Nack(for_seq, "decode", "staged bytes are not a valid config");
        ResetRun();
        return;
    }
    // No separate `ConfigValidate` here, and that is deliberate rather than an
    // omission. `ConfigDecodeJson` ends by validating the config it just built, so
    // by this point the check has already run; a second call would be a branch that
    // can never be taken, which reads as protection that is not there. (It was
    // there -- and a test asserting the "invalid" nack could not fail, because the
    // decode above rejects first with "decode". The test now asserts the code that
    // actually comes back.)
    //
    // The order still matters and is preserved: nothing is written until BOTH the
    // staged bytes and the decoded result are known good.
    //
    // **`Save` does NOT re-validate, and an earlier version of this comment claimed
    // it did** ("`ConfigEncodeBlob` refuses an invalid config"). It does not:
    // `ConfigEncodeBlob` only encodes. That claim mattered, because it was the
    // stated reason no validation was needed on other write paths -- and one of
    // those paths, `SystemOrchestrator::ApplyLearnedProfile`, could commit a profile
    // this validator refuses, which `Save` then persisted. The validation that
    // protects a headless learn now lives where the profile is chosen
    // (`LearnSession::Commit`, via `LadderWindowsAreDistinguishable`), because that
    // is the only place that knows what the learn is about to build.
    if (store_ == nullptr || !store_->Save(c)) {
        // The OLD config is untouched by a failed Save, which is the point of the
        // A/B slots. Report the failure rather than acking a config that is not
        // persisted.
        Nack(for_seq, "save_failed", "could not persist the config");
        ResetRun();
        return;
    }

    // Spec 4.2: "committed" is BOTH halves -- persisted AND running. The app
    // adopts what it pushed as the device's live state the moment this `ack`
    // arrives and offers no reboot affordance, so a device still classifying
    // against the previous config shows the user bindings it will not honour
    // until a power cycle. Applied only AFTER a successful Save, so a rejected
    // config changes nothing in either place.
    if (sys_ != nullptr) sys_->ApplyConfig(c);

    ResetRun();
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleConfigPatch(const cJSON *root, uint32_t for_seq) {
    // Scope: `settings.*` scalars only. The spec calls config_patch a "single-field
    // change", and a partial implementation of an arbitrary JSON-path language
    // would be a second, weaker config editor beside the chunked run -- the exact
    // two-homes defect. A path this cannot express is refused, not approximated.
    const cJSON *path = Str(root, "path");
    const cJSON *value = Num(root, "value");
    if (path == nullptr || value == nullptr || store_ == nullptr) {
        Nack(for_seq, "bad_frame", "config_patch needs path and a numeric value");
        return;
    }

    Config &c = command_config_;
    c = Config{};
    const ConfigLoadResult lr = store_->Load(&c);
    if (lr == ConfigLoadResult::kFellBackToDefaults) {
        // A patch is read-modify-write, so an unreadable config must NOT be
        // patched over: doing so writes defaults plus one field, discarding the
        // user's bindings, channels and settings. Measured before the fix: a
        // patch over a corrupted config took 3 bindings to 0 with an `ack`.
        // `kNoConfig` may still fall back -- a device that has never been
        // configured has nothing to lose.
        Nack(for_seq, "config_unreadable",
             "the stored config could not be read; refusing to overwrite it");
        return;
    }
    if (lr == ConfigLoadResult::kNoConfig) ConfigDefault(&c);

    // BOUNDED before the cast, exactly as the config codec's `ReadU32`/`ReadU8`
    // are. A raw `static_cast` is not a range check: measured before this fix,
    // `buzzer_level = 259` persisted as 3 (259 & 0xFF), and
    // `long_press_ms = 1e19` / `send_duration_ms = 1e10` both persisted as
    // 4294967295 -- a ~49-DAY KEY-line hold, which is the phantom-key hazard
    // FR-15/FR-39 exist to prevent. `ConfigValidate` only checks that these are
    // NONZERO and internally ordered, so it accepts any of those. The codec
    // refuses such a config on the way in; this path wrote it happily, so the two
    // had two different answers to "what is a legal value".
    const double v = value->valuedouble;
    // A path the patch vocabulary does not carry is its own error, distinct from
    // a value out of range: "I do not know that field" and "that number is not
    // legal" need different fixes, and the user is reading this in a log view.
    bool known_path = true;
    bool in_range = true;
    if (strcmp(path->valuestring, "settings.timings.debounce_ms") == 0) {
        in_range = NumToU32(v, &c.settings.timings.debounce_ms);
    } else if (strcmp(path->valuestring, "settings.timings.double_press_off_ms") == 0) {
        in_range = NumToU32(v, &c.settings.timings.double_press_off_ms);
    } else if (strcmp(path->valuestring, "settings.timings.long_press_ms") == 0) {
        in_range = NumToU32(v, &c.settings.timings.long_press_ms);
    } else if (strcmp(path->valuestring, "settings.timings.send_duration_ms") == 0) {
        in_range = NumToU32(v, &c.settings.timings.send_duration_ms);
    } else if (strcmp(path->valuestring, "settings.buzzer_level") == 0) {
        in_range = NumToU8(v, &c.settings.buzzer_level);
    } else if (strcmp(path->valuestring, "settings.led_level") == 0) {
        in_range = NumToU8(v, &c.settings.led_level);
    } else if (strcmp(path->valuestring, "settings.maintenance_timeout_ms") == 0) {
        // The scope above is "`settings.*` scalars only", and this is one -- it was
        // the single numeric settings scalar the table omitted, so a client could
        // patch every timing and both levels but not the maintenance window. Its
        // RANGE (zero, or past `kMaintenanceTimeoutMaxMs`) is enforced by the
        // `ConfigValidate` call at the end of this handler, exactly like the others.
        in_range = NumToU32(v, &c.settings.maintenance_timeout_ms);
    } else {
        known_path = false;
    }

    if (!known_path) {
        Nack(for_seq, "unknown_path", path->valuestring);
        return;
    }
    if (!in_range) {
        // Refused, not clamped: the codec refuses the same value on the way in, so
        // a clamp here would make an app-written config decode differently from
        // one written over the chunked run -- two answers to one question.
        Nack(for_seq, "bad_value", path->valuestring);
        return;
    }
    // A patch that would make the config invalid is refused: the old config stays.
    if (!ConfigValidate(c) || !store_->Save(c)) {
        Nack(for_seq, "invalid", "patch produced an invalid config");
        return;
    }
    // Spec 4.2 again: a patched config is a committed one, so it takes effect now
    // (a `settings.long_press_ms` patch that only landed on the next boot would be
    // exactly the silent no-op the chunked run's apply path exists to end).
    if (sys_ != nullptr) sys_->ApplyConfig(c);
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleTestKey(const cJSON *root, uint32_t for_seq) {
    const cJSON *key_mv = Num(root, "key_mv");
    if (key_mv == nullptr) {
        Nack(for_seq, "bad_frame", "test_key needs key_mv");
        return;
    }
    if (sys_ == nullptr || hal_ == nullptr) {
        Nack(for_seq, "unavailable", "no orchestrator");
        return;
    }
    // Spec 4.3 spells the frame `channel`, `key_mv`, `hold_ms`. This handler read
    // ONLY `key_mv` and hardcoded channel 0 with the default hold, so the bench
    // could not exercise the second output at all and a caller's hold time was
    // silently replaced -- a command that accepts a field and ignores it is worse
    // than one that refuses it, because the app's own test button then measures
    // something other than what it asked for.
    const cJSON *ch = Num(root, "channel");
    const cJSON *hold = Num(root, "hold_ms");
    uint8_t channel = 0;
    if (ch != nullptr && !NumToChannel(ch->valuedouble, &channel)) {
        Nack(for_seq, "bad_param", "channel out of range");
        return;
    }
    // `key_mv` is bounded BEFORE any cast, which the previous revision only
    // claimed: `static_cast<double>(static_cast<int>(mvd))` in the comparison
    // itself cast the raw value FIRST, so an out-of-range one was UB before the
    // bounds were consulted. That is reachable, not theoretical -- cJSON's
    // `parse_number` runs `strtod` and ignores `ERANGE`, so `key_mv: 1e999`
    // arrives as `+inf` and `static_cast<int>(inf)` is undefined (measured:
    // garbage). The range test therefore uses the NaN-safe negated form and casts
    // only once the value is known to be in `int`'s range.
    const double mvd = key_mv->valuedouble;
    if (!(mvd >= -32768.0 && mvd <= 32767.0) ||
        mvd != static_cast<double>(static_cast<int>(mvd))) {
        Nack(for_seq, "bad_param", "key_mv is not an integer in range");
        return;
    }
    const int mv = static_cast<int>(mvd);
    // `hold_ms` is bounded rather than taken as sent. A hold is time the OUTPUT
    // is driven, so an unbounded value pins the KEY line; 0 means "use the
    // default", matching the firmware's other hold sentinels.
    uint32_t hold_ms = kDefaultTestKeyHoldMs;
    if (hold != nullptr) {
        const double hd = hold->valuedouble;
        uint32_t bounded = 0;
        if (hd != 0.0 && !NumToU32(hd, &bounded)) {
            Nack(for_seq, "bad_param", "hold_ms out of range");
            return;
        }
        if (bounded > kTestKeyMaxHoldMs) {
            Nack(for_seq, "bad_param", "hold_ms out of range");
            return;
        }
        if (bounded > 0) hold_ms = bounded;
    }
    if (!sys_->TestDriveKeyMv(channel, mv, hold_ms, hal_->now_ms(hal_->ctx))) {
        Nack(for_seq, "out_of_range", "key_mv is outside the output envelope");
        return;
    }
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

/*
 * Spec 8.2's USB triggers. The WINDOW is the orchestrator's; the radio that the
 * window exists for is device-only work in the maintenance translation units, so
 * what this does is open or close the state and acknowledge.
 *
 * It goes through the orchestrator rather than reaching for a MaintenanceMode of
 * its own: a second instance would have its own idea of whether the window is
 * open, and the AUX1 path and the USB path could then disagree about the same
 * device. (The same reasoning as the single ConfigStore.)
 */
void CommandRouter::HandleMaintenanceEnter(uint32_t for_seq) {
    if (sys_ == nullptr) {
        Nack(for_seq, "unavailable", "no orchestrator");
        return;
    }
    sys_->EnterMaintenance(MaintenanceTrigger::kUsbCommand, hal_->now_ms(hal_->ctx));
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleMaintenanceExit(uint32_t for_seq) {
    if (sys_ == nullptr) {
        Nack(for_seq, "unavailable", "no orchestrator");
        return;
    }
    // Idempotent: exiting a closed window is not an error, because the app may
    // exit after the 5-minute timeout already closed it and a nack there would
    // read as a failed command.
    sys_->ExitMaintenance();
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

// Map the shared gate's reason onto a wire word. The app matches on the WORD
// (spec 4.3), so two spellings of one cause is a lookup that misses with no
// error anywhere -- which is why this is one function and not three `snprintf`s
// at the call sites.
namespace {
const char *OtaErrWord(OtaResult r) {
    switch (r) {
        case OtaResult::kOk:            return "ok";
        case OtaResult::kNotStarted:    return "no_run";
        case OtaResult::kAlreadyStarted:return "run_open";
        case OtaResult::kTooLarge:      return "too_large";
        case OtaResult::kVerifyFailed:  return "verify_failed";
        case OtaResult::kFlashFailed:   return "flash_failed";
        case OtaResult::kSetBootFailed: return "set_boot_failed";
        case OtaResult::kNotSupported:  return "not_supported";
    }
    return "unknown";
}
}  // namespace

/*
 * USB OTA (spec 9.3). A THIN adapter: every byte goes through `Update/OtaUsb`,
 * which owns the verification gate and the single commit point. This layer only
 * parses the frame, enforces the same range-first ordering the config transport
 * uses, and turns the result into an ack or a nack.
 *
 * **Nothing here re-implements a check.** Spec 9's design rule is that the
 * checksum, slot-writing, rollback and health-confirmation logic exists exactly
 * once and takes a byte stream; a second implementation of the verification path
 * is how one route ends up less safe than the others. `OtaUsb` and `OtaWifi`
 * therefore call the SAME `OtaBegin`/`OtaChunk`/`OtaEnd`.
 */
void CommandRouter::HandleOtaBegin(const cJSON *root, uint32_t for_seq) {
    const cJSON *size = Num(root, "size");
    const cJSON *sha = Str(root, "sha256");
    if (size == nullptr || sha == nullptr) {
        Nack(for_seq, "bad_frame", "ota_begin needs size and sha256");
        return;
    }
    // RANGE FIRST, the same ordering `config_begin` documents: the integrality
    // test below casts to `size_t`, and casting a value outside its range
    // (including the `+inf` a `1e999` literal overflows to, since cJSON ignores
    // `ERANGE`) is UNDEFINED BEHAVIOUR. The negated form is NaN-safe.
    const double sz = size->valuedouble;
    if (!(sz >= 0.0 && sz <= static_cast<double>(kAppSlotBytes))) {
        // `OtaBegin` would refuse this too, as `kTooLarge`, but refusing here
        // keeps the cast below defined -- which is the whole reason for the
        // ordering rule rather than a duplicated bound.
        Nack(for_seq, "too_large", "image size exceeds the app slot");
        return;
    }
    if (sz != static_cast<double>(static_cast<size_t>(sz))) {
        Nack(for_seq, "bad_frame", "size must be a whole number of bytes");
        return;
    }
    const OtaResult r = OtaBegin(static_cast<size_t>(sz), sha->valuestring, kAppSlotBytes);
    if (r != OtaResult::kOk) {
        Nack(for_seq, OtaErrWord(r), "the image was refused");
        return;
    }
    // The ack confirms the run is OPEN. It deliberately carries no progress: the
    // APP tracks `sent` itself from the chunks it has already acked (see
    // `SwcClient.pushFirmware`'s `onProgress`), so a device-side byte count here
    // would be a second home for a number the app already has -- and one that
    // `ImageVerifyBytesSoFar()` would have had to be wired up to produce (spec
    // N-72). A partial transfer is resumed by the app re-beginning, not by the
    // device reporting where it got to.
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleOtaChunk(const cJSON *root, uint32_t for_seq) {
    const cJSON *off = Num(root, "offset");
    const cJSON *data = Str(root, "data_b64");
    if (off == nullptr || data == nullptr) {
        OtaAbort();
        Nack(for_seq, "bad_frame", "ota_chunk needs offset and data_b64");
        return;
    }
    const double od = off->valuedouble;
    // Range first, for the same UB reason as `config_chunk`. The upper bound is
    // the slot, not the config staging buffer.
    if (!(od >= 0.0 && od <= static_cast<double>(kAppSlotBytes))) {
        OtaAbort();
        Nack(for_seq, "bad_offset", "offset out of range");
        return;
    }
    if (od != static_cast<double>(static_cast<size_t>(od))) {
        OtaAbort();
        Nack(for_seq, "bad_frame", "offset must be a whole number of bytes");
        return;
    }
    // **`offset` is what makes a gap or an overlap detectable, and spec 9.3
    // requires both be rejected.** `OtaBytesWritten()` is the running byte count
    // the USB path has accepted, so a chunk whose offset is not exactly there is
    // out of order: refused AND the run aborted, because a spliced image is one
    // whose digest will fail at the end anyway -- aborting now names the real
    // cause instead of a `verify_failed` an hour later.
    if (static_cast<size_t>(od) != OtaBytesWritten()) {
        OtaAbort();
        Nack(for_seq, "gap", "chunk offset is not the next expected byte");
        return;
    }

    uint8_t decoded[kConfigWireChunkBytes];
    size_t dn = 0;
    // `Base64Decode` refuses output that does not fit `decoded`, so a chunk whose
    // decoded form exceeds the wire bound fails HERE rather than being truncated.
    if (!Base64Decode(data->valuestring, strlen(data->valuestring), decoded, sizeof(decoded), &dn)) {
        OtaAbort();
        Nack(for_seq, "bad_frame", "data_b64 is not valid base64 or exceeds the wire bound");
        return;
    }
    if (dn > 0) {
        const OtaResult r = OtaChunk(decoded, dn);
        if (r != OtaResult::kOk) {
            Nack(for_seq, OtaErrWord(r), "the chunk was refused");
            return;
        }
    }
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleOtaEnd(uint32_t for_seq) {
    const OtaResult r = OtaEnd();
    if (r != OtaResult::kOk && r != OtaResult::kNotSupported) {
        Nack(for_seq, OtaErrWord(r), "the image failed verification or could not be committed");
        return;
    }
    // **A host build reaches here with `kNotSupported`** (it has no partitions),
    // and reporting that honestly is what keeps a host test from asserting on an
    // install that never happened. The ack therefore carries the result word so
    // the app can distinguish "installed" from "this build cannot install".
    char body[96];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true,\"result\":\"%s\"",
             static_cast<unsigned>(for_seq), OtaErrWord(r));
    Emit("ack", body);
}

void CommandRouter::HandleIdentify(const cJSON *root, uint32_t for_seq) {
    const cJSON *pattern = Str(root, "pattern");
    if (pattern == nullptr) {
        Nack(for_seq, "bad_frame", "identify needs pattern");
        return;
    }
    if (sys_ == nullptr) {
        Nack(for_seq, "unavailable", "no orchestrator");
        return;
    }
    // Two patterns only, because the spec's `pattern` field is a user-facing
    // "which unit is this" request rather than a way to drive the LEDs
    // arbitrarily. An unknown pattern is refused rather than silently ignored.
    //
    // The two patterns do DIFFERENT things, and used to be the same call -- an
    // "accepts a field and ignores it" defect: a client asking for `buzz` got the
    // LED double-flash too. `flash` borrows LED_STAT for the burst; `buzz` sounds
    // the buzzer only, for a user looking at the wheel rather than the box.
    if (strcmp(pattern->valuestring, "flash") == 0) {
        sys_->Identify(true, false);
    } else if (strcmp(pattern->valuestring, "buzz") == 0) {
        sys_->Identify(false, true);
    } else {
        Nack(for_seq, "unknown_pattern", pattern->valuestring);
        return;
    }
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

/*
 * FR-5's live stream. `ladder_sample` carries the FILTERED level so the app can
 * render the ladder while the user holds a button -- which is what makes learn
 * usable at all: the user needs to see the reading move as they press.
 *
 * It is emitted one per Process() call while a learn run is open, not in a burst,
 * for the same reason the config reply is chunked: the transport's TX buffer is
 * two frames deep, and a burst would overflow it and drop samples.
 *
 * Spec 4.3 says >=20 samples/s reaches the link with bounded latency. At the
 * 10 ms poll rate that is satisfied with margin; the bound here is that Process()
 * emits AT MOST one sample per tick, so the stream cannot starve the key path.
 */
void CommandRouter::EmitLadderSample() {
    if (!learn_open_) return;
    // `sys_ == nullptr` reports 0 rather than suppressing the frame. The stream's
    // contract is "a sample per tick while the run is open" (FR-5), and a reader
    // that suddenly gets NO frames cannot tell a missing level from a dead link.
    // 0 mV is unambiguous: it is below the ladder's floor, so the app renders it
    // as "no reading" rather than as a real level.
    const int level_mv = (sys_ != nullptr)
                             ? sys_->FilteredLevelMv(static_cast<uint8_t>(learn_channel_))
                             : 0;
    char body[160];
    snprintf(body, sizeof(body),
             "\"channel\":%d,\"level_mv\":%d,\"n\":%u",
             learn_channel_, level_mv, static_cast<unsigned>(learn_samples_));
    Emit("ladder_sample", body);
}

/*
 * Feed the value the stream is reporting into the session that learn_commit will
 * commit. `EmitLadderSample` reads the level from the same call, so the frame and
 * the sample are the same measurement -- recording a separate read would measure
 * a value the app never saw.
 *
 * The idle reference is the channel's LIVE idle (spec 6.3), the same denominator
 * the classifier uses and the same one the headless wizard captures. `Commit`
 * records the button's `mv_center` in absolute millivolts AT this reference and
 * the config stores this reference as `learned_idle_mv`, so the pair is
 * self-consistent however far the rail has moved since the last learn. Passing
 * the STORED learned idle instead would record centres against the old rail while
 * stamping them as belonging to the current one, so every subsequent classify
 * would derive them against a denominator the learn never used -- and on a moved
 * rail the recorded ratio would be wrong by the rail's own deviation.
 *
 * Out-of-range levels are still recorded: the session counts them and reports
 * `out_of_range` (a wiring fault), rather than dropping them and reporting the
 * vaguer `too_few_samples`.
 */
void CommandRouter::RecordLearnSample() {
    if (sys_ == nullptr) return;
    const uint8_t ch = static_cast<uint8_t>(learn_channel_);
    const int level_mv = sys_->FilteredLevelMv(ch);
    const int idle_mv = sys_->IdleReferenceMv(ch);
    // The third argument is the +3V3 RAIL, not the idle. It used to be `idle_mv`
    // again, which recorded the wheel's idle level (~2835 mV) in a field defined
    // as the regulated rail (~3300 mV, spec 3.4/FR-30) -- a field the app displays
    // and uses to detect a sagging regulator. The headless wizard records the
    // nominal rail; the app path recorded a different quantity for the same field,
    // so the two learn paths disagreed. Both now use `kNominalRailMv`.
    // The fourth argument is the NTC temperature (FR-1's first clause / N-67). It
    // used to be a literal 0 with the comment that the channel is never converted
    // -- true then, and fixed here: `SampleNtcTenthsC` reads `ADC_CH_TEMP` through
    // the divider and the B3380 model. The same call the headless wizard makes, so
    // the two learn paths record the same quantity for the same field, exactly as
    // the rail above.
    const int temp_tenths = (sys_ != nullptr) ? sys_->SampleNtcTenthsC() : 0;
    session_.AddSample(level_mv, idle_mv, kNominalRailMv,
                       static_cast<int16_t>(temp_tenths),
                       hal_ != nullptr ? hal_->now_ms(hal_->ctx) : 0);
}

void CommandRouter::EmitLog(const char *level, const char *msg) {
    if (level == nullptr || msg == nullptr) return;
    char body[kNdjsonMaxFrame / 2];
    // Both strings are bounded copies with their JSON quoting stripped by hand,
    // the same treatment `EmitGesture` gives the button id and for the same
    // reason: escaping costs a routine, and these are values this firmware
    // generates rather than values a peer sends.
    char lv[17], ms[161];
    size_t n = 0;
    while (n + 1 < sizeof(lv) && level[n] != '\0') {
        const char c = level[n];
        lv[n] = (c == '"' || c == '\\') ? '_' : c;
        ++n;
    }
    lv[n] = '\0';
    n = 0;
    while (n + 1 < sizeof(ms) && msg[n] != '\0') {
        const char c = msg[n];
        ms[n] = (c == '"' || c == '\\') ? '_' : c;
        ++n;
    }
    ms[n] = '\0';
    snprintf(body, sizeof(body), "\"level\":\"%s\",\"msg\":\"%s\"", lv, ms);
    Emit("log", body);
}

void CommandRouter::EmitGesture(const SystemOrchestrator::GestureEventRecord &ev) {
    // The wire spelling is the CONFIG codec's, deliberately: the app matches this
    // gesture name against `Binding.gesture`, which is encoded by the same table.
    // A second spelling here ("LONG_PRESS", say) would make the app's lookup miss
    // and the event would render as an unknown gesture with no error anywhere.
    const char *g = "NONE";
    switch (ev.gesture) {
        case Gesture::kSingle: g = "SINGLE"; break;
        case Gesture::kDouble: g = "DOUBLE"; break;
        case Gesture::kLong:   g = "LONG";   break;
        case Gesture::kNone:   g = "NONE";   break;
    }
    char body[192];
    char button[2 + kLadderIdLen];
    if (ev.button_id == nullptr) {
        // FR-12: an unrecognised press is reported with a JSON null, not an empty
        // string and not a guessed id. A frame that named a button here would be
        // the guess FR-12 forbids, and it would make the app highlight a button
        // the driver never touched.
        button[0] = 'n'; button[1] = 'u'; button[2] = 'l'; button[3] = 'l';
        button[4] = '\0';
    } else {
        // The id reaches BOTH a JSON string and a `%s`, so it is bounded and its
        // quoting is stripped by hand. `kLadderIdLen` is 16, so 31 bytes of any
        // hostile value survive into `id`; escaping it here would cost a
        // JSON-escape routine for a field the config codec already validates.
        size_t n = 0;
        while (n + 1 < kLadderIdLen && ev.button_id[n] != '\0') {
            const char c = ev.button_id[n];
            button[n] = (c == '"' || c == '\\') ? '_' : c;
            ++n;
        }
        button[n] = '\0';
        button[n + 1] = '\0';
        // Re-quote in place: shift right to make room for the opening quote.
        const size_t len = n;
        for (size_t i = len + 1; i > 0; --i) button[i] = button[i - 1];
        button[0] = '"';
        button[len + 1] = '"';
        button[len + 2] = '\0';
    }
    snprintf(body, sizeof(body),
             "\"channel\":%u,\"button\":%s,\"gesture\":\"%s\",\"t_ms\":%llu,\"level_mv\":%d,"
             "\"idle_mv\":%d",
             static_cast<unsigned>(ev.channel_index), button, g,
             static_cast<unsigned long long>(ev.at_ms), ev.level_mv, ev.idle_mv);
    Emit("event", body);
}

void CommandRouter::HandleLearnStart(const cJSON *root, uint32_t for_seq) {
    // No orchestrator is required HERE. `learn_start` only opens the stream; the
    // orchestrator is needed to SAMPLE (`learn_commit`), and requiring it here
    // would refuse a legitimate stream in any host build without one.
    const cJSON *ch = Num(root, "channel");
    if (ch == nullptr) {
        Nack(for_seq, "bad_param", "channel is required");
        return;
    }
    uint8_t channel = 0;
    if (!NumToChannel(ch->valuedouble, &channel)) {
        Nack(for_seq, "bad_param", "channel out of range");
        return;
    }
    // A learn run that is already open is RE-STARTED rather than refused: the app
    // sends learn_start again when the user switches button, and refusing would
    // make the UI show an error for a legitimate action.
    learn_open_ = true;
    learn_channel_ = channel;
    learn_samples_ = 0;
    // The session is seeded with the channel's CURRENT ladder so a learn that
    // overlaps an existing button can be refused (LearnReject::kTooCloseToExisting)
    // rather than silently creating two windows that classify the same level.
    //
    // The app names the button it is re-learning (`button_id`), so that entry is
    // removed from the neighbour set: the app path replaces a button too, and a
    // re-measure lands within the old window by definition, so leaving it in would
    // refuse the correction as "too close to itself". The wizard does the same
    // subtraction by generating the id; here the id arrives from the host.
    LadderProfile existing{};
    if (store_ != nullptr) {
        Config &cur = command_config_;
        cur = Config{};
        // Both loaded results carry a real ladder -- a recovered config is the one
        // the device is running, so its buttons are exactly the neighbours a
        // re-learn must stay clear of. Reading only `kLoaded` left the set empty
        // on a recovered device, so a re-measure could land on top of an existing
        // button and the classifier could no longer tell the two apart.
        if (ConfigLoadResultIsUsable(store_->Load(&cur))) {
            existing = cur.channels[channel].ladder;
        }
    }
    if (const cJSON *bid = Str(root, "button_id")) {
        uint8_t kept = 0;
        for (uint8_t i = 0; i < existing.count && i < kLadderMaxButtons; ++i) {
            if (strcmp(existing.buttons[i].id, bid->valuestring) == 0) continue;
            existing.buttons[kept++] = existing.buttons[i];
        }
        existing.count = kept;
    }
    session_.Start(existing);

    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleLearnStop(const cJSON *root, uint32_t for_seq) {
    const cJSON *ch = Num(root, "channel");
    if (ch == nullptr) {
        Nack(for_seq, "bad_param", "channel is required");
        return;
    }
    uint8_t channel = 0;
    if (!NumToChannel(ch->valuedouble, &channel)) {
        Nack(for_seq, "bad_param", "channel out of range");
        return;
    }
    // The stop NAMES the stream to close, and the channel guard belongs here for
    // the same reason `learn_commit` carries one. Without it `(void)root` meant a
    // `learn_stop{channel:1}` closed channel 0's open stream and acked -- the peer
    // could not target a stream and silently stopped the wrong one, and the two
    // sibling handlers disagreed about whether the channel field meant anything.
    //
    // The samples are KEPT (`learn_channel_` and `session_` are untouched): spec
    // 4.3's flow is start -> stream -> STOP -> commit, and the commit reads the
    // session's channel, so discarding the session here would break the flow it
    // exists to serve. Only the STREAM ends.
    if (learn_open_ && static_cast<uint8_t>(learn_channel_) != channel) {
        Nack(for_seq, "channel_mismatch",
             "learn_stop's channel differs from the open learn stream's");
        return;
    }
    // Closing the stream. The app is expected to follow with learn_commit if it
    // wants the button stored; stopping alone stores nothing.
    learn_open_ = false;
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleLearnCommit(const cJSON *root, uint32_t for_seq) {
    if (sys_ == nullptr || store_ == nullptr) {
        Nack(for_seq, "unavailable", "no orchestrator");
        return;
    }
    const cJSON *ch = Num(root, "channel");
    const cJSON *btn = Str(root, "button_id");
    const cJSON *name = Str(root, "name");
    if (ch == nullptr || btn == nullptr || name == nullptr) {
        Nack(for_seq, "bad_param", "channel, button_id and name are required");
        return;
    }
    // Refuse an EMPTY string, not merely a missing one. `Str` returns the item for
    // `""` (a valid JSON string with a non-null `valuestring`), so a present-but-
    // empty id or name passed this guard, was copied into the stored button, and
    // -- since neither `LearnSession::Commit` nor `ConfigStore::Save` validates it
    // -- was persisted. The next boot's `ConfigDecodeJson` refuses the empty string
    // (`ReadStr`), so `Load` returns `kFellBackToDefaults` and the user loses every
    // binding, both channels and all settings, reported only as a corrupt config.
    // The id/name are BOUNDED here (their own widths, minus the NUL) for the same
    // reason: `snprintf` would otherwise silently truncate a too-long value.
    //
    // The widths are the LADDER's (`LadderDecode.h`), because that is where these
    // two strings land -- `out` is a `LadderButton`, whose `id` and `name` are
    // `kLadderIdLen`/`kLadderNameLen`. They are deliberately SEPARATE constants
    // from the binding/channel widths (`kBindingIdLen`/`kChannelNameLen`) even
    // where the numbers agree, so a re-tune of one is a compile-time question
    // rather than a silent over- or under-bound here. (The copy below is into
    // `out`, not into a `Binding`, so the ladder pair is the one that governs.)
    if (btn->valuestring[0] == '\0' || name->valuestring[0] == '\0') {
        Nack(for_seq, "bad_param", "button_id and name must be non-empty");
        return;
    }
    if (strlen(btn->valuestring) >= kLadderIdLen ||
        strlen(name->valuestring) >= kLadderNameLen) {
        Nack(for_seq, "bad_param", "button_id or name is too long");
        return;
    }
    uint8_t channel = 0;
    if (!NumToChannel(ch->valuedouble, &channel)) {
        Nack(for_seq, "bad_param", "channel out of range");
        return;
    }
    // **A commit needs a SESSION, not just a matching channel.** `learn_channel_`
    // is `-1` when no session exists -- before any `learn_start`, and after a link
    // drop (see `ForgetLearnSession`). Without this the comparison below reads the
    // sentinel as a channel and reports `channel_mismatch` for a commit that has no
    // stream at all, naming the wrong cause (FR-29's "the reason is specific and
    // actionable"). Named `no_session` so the peer knows the real state.
    if (learn_channel_ < 0) {
        Nack(for_seq, "no_session", "no learn session is open; send learn_start first");
        return;
    }
    // The samples came from the STREAM's channel, so the write must land on that
    // same channel -- a commit naming a different one would store channel 0's
    // measured voltage on channel 1's ladder. Measured: learn_start(channel 0)
    // then learn_commit(channel 1) returned `ack` and wrote a 1430 mV button onto
    // channel 1 (whose own input was idle at 2835 mV).
    //
    // **Keyed on the SESSION's channel, not on `learn_open_`.** `learn_stop` is
    // the SPECIFIED flow (spec 4.3: start, stream, stop, then commit), and it
    // clears `learn_open_` while leaving the measured samples in the session. So a
    // guard that only fired while the stream was open was defeated by the ordinary
    // flow: learn_start(0) -> stream -> learn_stop -> learn_commit(1) wrote
    // channel 0's measurement onto channel 1. `learn_channel_` is the session's
    // channel and survives a stop, which is what makes it the right key.
    if (channel != static_cast<uint8_t>(learn_channel_)) {
        Nack(for_seq, "channel_mismatch",
             "learn_commit's channel differs from the open learn stream's");
        return;
    }

    // The session was fed by `RecordLearnSample` on every `Process()` tick the
    // stream was open, which is what makes the spec's "accept the streamed
    // samples" true. Deliberately NO sample is added here: the commit is the end
    // of the stream, and folding one more reading in would measure a level the
    // app never saw and could move the mean after the user stopped pressing.
    LadderButton out{};
    // id and name are the CALLER's: a slug and a display label are not facts
    // about a voltage, and learn cannot invent them (LearnSession.h).
    snprintf(out.id, sizeof(out.id), "%s", btn->valuestring);
    snprintf(out.name, sizeof(out.name), "%s", name->valuestring);
    const LearnReject r = session_.Commit(&out);

    if (r != LearnReject::kNone) {
        // FR-29: the reason is specific and actionable. "it didn't work" is not
        // something a user holding a button one-handed can act on.
        Nack(for_seq, "learn_rejected", LearnRejectReason(r));
        return;
    }

    // Persisted through the config, NOT as a side channel: a learned button that
    // is not in the config is a button that vanishes at reboot.
    //
    // **Only `kNoConfig` may fall back to defaults.** Treating ANY failed load as
    // "start from defaults" is a data-loss bug: it cannot tell "never configured"
    // from "configured but UNREADABLE", and in the second case the save below
    // overwrites the user's whole config -- bindings, the other channel, every
    // setting -- with defaults. Measured: three bindings replaced by zero on a
    // device whose slots were corrupt, from one `learn_commit`, with an `ack`. An
    // unreadable config is a fault to report, not a blank sheet.
    Config &c = command_config_;
    c = Config{};
    const ConfigLoadResult lr = store_->Load(&c);
    if (lr == ConfigLoadResult::kFellBackToDefaults) {
        Nack(for_seq, "config_unreadable",
             "the stored config could not be read; refusing to overwrite it");
        return;
    }
    if (lr == ConfigLoadResult::kNoConfig) ConfigDefault(&c);
    LadderProfile &lp = c.channels[channel].ladder;

    // Rescale the stored ladder's millivolts onto the LIVE rail this learn
    // measured at, so every button shares the ONE denominator the profile is about
    // to be stamped with.
    //
    // A `LadderProfile` has a single `learned_idle_mv` and every button's
    // millivolts are in that frame. The measured button below is in the LIVE
    // frame, and `lp.learned_idle_mv` is set to the live idle at the end of this
    // handler -- so a rail that moved since these buttons were learned would leave
    // them in the old frame under a new denominator. Classification reads them on
    // the wrong scale and the press lands in whichever window it now falls inside:
    // the WRONG button fires. Scaling both `mv_center` and `mv_tolerance` keeps
    // every permille window the same to within one permille (the quantization of
    // writing whole millivolts), so nothing the validator or the classifier
    // derives from ratios meaningfully changes here -- see `LadderProfileRebase`.
    LadderProfileRebase(lp, lp.learned_idle_mv, session_.LearnedIdleMv());

    // REPLACE IN PLACE when the id is already on the ladder, rather than always
    // appending. The host names the button it learns (`button_id`), and the app
    // sends the same id again when the user re-measures one -- so appending put
    // two buttons on the ladder with ONE id. Nothing rejects that
    // (`ConfigValidate` does not check id uniqueness), and `BindingResolve` and
    // `BindingsForButton` both look a binding up by `strcmp` on the id, so the id
    // became ambiguous between two different voltages. The headless wizard path
    // had the same defect and is fixed the same way there.
    int target = -1;
    for (uint8_t i = 0; i < lp.count; ++i) {
        if (strcmp(lp.buttons[i].id, out.id) == 0) { target = i; break; }
    }
    if (target >= 0) {
        lp.buttons[target] = out;
    } else if (lp.count < kLadderMaxButtons) {
        lp.buttons[lp.count++] = out;
    } else {
        Nack(for_seq, "no_space", "the ladder has no free button slot");
        return;
    }
    lp.learned_idle_mv = session_.LearnedIdleMv();
    if (!store_->Save(c)) {
        Nack(for_seq, "save_failed", "could not persist the learned button");
        return;
    }
    // Spec 4.2/7.3: the same rule as the other two write paths. The app's learn
    // screen is gone after this `ack`, so a device that re-derived its classifier
    // only at the next boot would leave the user with a button they just measured
    // doing nothing.
    if (sys_ != nullptr) sys_->ApplyConfig(c);

    // CLOSE the stream. The commit ends the learn run (spec 4.3's flow is
    // start -> stream -> commit, with `learn_stop` optional), so a device that
    // left it open kept emitting `ladder_sample` and kept feeding `session_` with
    // post-press idle readings -- a duplicate `learn_commit` would then re-run
    // `Commit` over idle samples, and a client that committed and stopped talking
    // left the device streaming into the link forever.
    learn_open_ = false;

    char body[192];
    snprintf(body, sizeof(body),
             "\"for_seq\":%u,\"ok\":true,\"mv_center\":%u,\"mv_tolerance\":%u",
             static_cast<unsigned>(for_seq), static_cast<unsigned>(out.mv_center),
             static_cast<unsigned>(out.mv_tolerance));
    Emit("ack", body);
}

void CommandRouter::HandleReboot(const cJSON *root, uint32_t for_seq) {
    const cJSON *target = Str(root, "boot_target");
    if (hal_ == nullptr) {
        Nack(for_seq, "unavailable", "no HAL");
        return;
    }
    // Ack BEFORE rebooting, or the ack is lost with the reset and the app cannot
    // tell a successful reboot from a dropped link.
    const char *t = (target != nullptr) ? target->valuestring : "app";
    // **BOTH declared targets are honoured, and each selects a different HAL
    // action** (spec 4.3). This was refused `bad_target` for a revision on the
    // belief that entering the ROM download loader was a power-on/BOOT-pin event
    // with no software path (§3.2). That belief was wrong for this part: the
    // ESP32-S3 ROM re-checks `RTC_CNTL_FORCE_DOWNLOAD_BOOT` on every reset, the
    // bit is in the RTC domain (survives `esp_restart()`, not a power cycle), and
    // IDF's own `esp_usb_console_before_restart` writes it for its
    // `REBOOT_BOOTLOADER`. `IHAL::reboot_to_download` is that path.
    //
    // **Why this matters enough to be a frame and not a build option.** A
    // developer affordance gated behind a special build is a trap: flashing a
    // normal image would remove the very capability needed to flash the next one,
    // and the fallback is opening the enclosure and poking a recessed BOOT pin.
    // The ROM loader's port (`303A:1001`, the same VID/PID family as an ESP32-S3
    // ROM interface with no CDC descriptor) is the one esptool can reach while the
    // app firmware -- which hands the USB PHY to TinyUSB and disables
    // USB-Serial-JTAG -- is running. So `bootloader` is what makes the device
    // flashable over its own cable.
    //
    // **Still refused by name for anything else.** An unknown target is the
    // "accepted field that changes nothing, reported as success" shape: the peer
    // then expects a destination the device did not go to.
    void (*action)(void *) = nullptr;
    const char *chosen = nullptr;
    if (strcmp(t, "app") == 0) {
        action = hal_->reboot;
        chosen = "app";
    } else if (strcmp(t, "bootloader") == 0) {
        action = hal_->reboot_to_download;
        chosen = "bootloader";
    } else {
        Nack(for_seq, "bad_target", t);
        return;
    }
    // A HAL that cannot reach the requested destination must SAY so rather than
    // fall back to the other one -- "wrong destination, reported as success" is
    // the exact failure this frame's target field exists to prevent.
    if (action == nullptr) {
        Nack(for_seq, "bad_target", chosen);
        return;
    }
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
    // FLUSH IT. `Emit` only queues into the transport; the poll loop's
    // `ServiceTx` is what writes to the USB FIFO, and it never runs again before
    // the reset below. Without this the ack is lost with the reset -- the app
    // cannot tell a successful reboot from a dropped link, which is precisely
    // what the ack-before-reset ordering exists to avoid.
    if (tx_flush_ != nullptr) tx_flush_(tx_flush_ctx_);
    action(hal_->ctx);
}
