#include "Link/CommandRouter.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "Config/ConfigDefaults.h"
#include "Feedback/BuzzerGrammar.h"
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

void CommandRouter::Emit(const char *type, const char *body_fields) {
    if (sink_ == nullptr) return;
    NdjsonWriter w;
    w.Write(type, seq_sent_++, body_fields);
    // The sink contract: exactly one frame, WITHOUT its trailing newline. The
    // transport owns the newline.
    sink_(sink_ctx_, w.Line(), w.LineLen() - 1);
}

void CommandRouter::Nack(uint32_t for_seq, const char *err, const char *detail) {
    char body[256];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"err\":\"%s\",\"detail\":\"%s\"",
             static_cast<unsigned>(for_seq), err, detail ? detail : "");
    Emit("nack", body);
}

void CommandRouter::ResetRun() {
    staging_len_ = 0;
    expected_len_ = 0;
    expected_crc_ = 0;
    run_open_ = false;
}

void CommandRouter::OnDisconnected() {
    // An interrupted run is discarded wholesale: a partial config is never
    // applied (spec 4.2).
    ResetRun();
    reply_open_ = false;
    reply_off_ = 0;
    reply_len_ = 0;
    // Reconnect is stateless (spec 4.4), so the peer's sequence baseline is
    // re-established by the next frame rather than remembered across a link drop.
    seen_any_ = false;
}

void CommandRouter::OnConnected() {
    // hello first (spec 4.5's version negotiation), then the config run so the
    // app can render immediately without asking.
    char body[192];
    snprintf(body, sizeof(body),
             "\"fw_version\":\"%s\",\"hw_id\":\"SWC-S3\",\"protocol_v\":%u,\"caps\":[\"config\",\"ota\",\"learn\"]",
             SWC_FW_VERSION, static_cast<unsigned>(kNdjsonProtocolVersion));
    Emit("hello", body);
    BeginConfigReplyRun();
}

void CommandRouter::BeginConfigReplyRun() {
    // Encode the CURRENT config -- stored if there is one, else the shared
    // default. Encoding the stored blob instead would put the wire form and the
    // NVS form on two different code paths that could drift.
    Config c = ConfigDefault();
    if (store_ != nullptr) {
        Config loaded{};
        if (store_->Load(&loaded) == ConfigLoadResult::kLoaded) c = loaded;
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
    // A learn run streams whether or not a config reply is in flight. One sample
    // per call, so the stream cannot starve the reply run or the key path.
    if (learn_open_ && !reply_open_) {
        EmitLadderSample();
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
        char b64[1024];
        const size_t bl = Base64Encode(reinterpret_cast<const uint8_t *>(reply_buf_ + reply_off_),
                                       n, b64, sizeof(b64));
        if (bl == 0) {
            Nack(reply_seq_, "encode_failed", "chunk did not fit the frame");
            reply_open_ = false;
            return;
        }
        char body[sizeof(b64) + 64];
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
    const bool vbus = (hal_ != nullptr) && hal_->gpio_read(hal_->ctx, GPIO_VBUS_VALID);
    const bool safe = (sys_ != nullptr) && sys_->SafeIdleEstablished();
    char body[256];
    snprintf(body, sizeof(body),
             "\"for_seq\":%u,\"vbus_present\":%s,\"gain_mode\":\"%s\",\"uptime_ms\":%llu,"
             "\"config_state\":\"%s\"",
             static_cast<unsigned>(for_seq), vbus ? "true" : "false",
             "amplified",   // refined once per-channel gain mode is tracked here
             static_cast<unsigned long long>(hal_ ? hal_->now_ms(hal_->ctx) : 0ULL),
             safe ? "ok" : "unsafe");
    Emit("status", body);
}

void CommandRouter::SendStatus() {
    ReplyStatus(seq_sent_);
}

void CommandRouter::OnLine(const char *line, size_t len) {
    if (line == nullptr || len == 0) return;
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
    } else if (strcmp(h.type, "time_sync") == 0) {
        // Accepted and acked: the firmware has no RTC and no wall-clock use, so
        // storing it would be a field nothing reads. Acking is honest -- the
        // frame was understood -- and refusing would make the app think the link
        // is broken.
        char body[64];
        snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(h.seq));
        Emit("ack", body);
    } else {
        // ota_* belong to a later task. They are KNOWN commands (so they are not
        // "unknown_type"), but this build cannot yet execute them, and saying that
        // is better than a silent no-op.
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
    if (tl < 0.0 || tl > static_cast<double>(ConfigMaxSerializedSize())) {
        // Up front, from the declared length: the staging buffer is fixed and a
        // peer-supplied length must never size it.
        Nack(for_seq, "too_large", "total_len exceeds the staging buffer");
        return;
    }
    ResetRun();
    expected_len_ = static_cast<size_t>(tl);
    expected_crc_ = static_cast<uint32_t>(crc->valuedouble);
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
    if (od < 0.0 || od > static_cast<double>(ConfigMaxSerializedSize())) {
        Nack(for_seq, "bad_offset", "offset out of range");
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
    if (hal_ != nullptr) last_chunk_ms_ = hal_->now_ms(hal_->ctx);

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

    Config c{};
    if (!ConfigDecodeJson(reinterpret_cast<const char *>(staging_), staging_len_, &c)) {
        Nack(for_seq, "decode", "staged bytes are not a valid config");
        ResetRun();
        return;
    }
    if (!ConfigValidate(c)) {
        Nack(for_seq, "invalid", "config failed validation");
        ResetRun();
        return;
    }
    if (store_ == nullptr || !store_->Save(c)) {
        // The OLD config is untouched by a failed Save, which is the point of the
        // A/B slots. Report the failure rather than acking a config that is not
        // persisted.
        Nack(for_seq, "save_failed", "could not persist the config");
        ResetRun();
        return;
    }

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

    Config c{};
    if (store_->Load(&c) != ConfigLoadResult::kLoaded) c = ConfigDefault();

    const double v = value->valuedouble;
    bool applied = true;
    if (strcmp(path->valuestring, "settings.timings.debounce_ms") == 0) {
        c.settings.timings.debounce_ms = static_cast<uint32_t>(v);
    } else if (strcmp(path->valuestring, "settings.timings.double_press_off_ms") == 0) {
        c.settings.timings.double_press_off_ms = static_cast<uint32_t>(v);
    } else if (strcmp(path->valuestring, "settings.timings.long_press_ms") == 0) {
        c.settings.timings.long_press_ms = static_cast<uint32_t>(v);
    } else if (strcmp(path->valuestring, "settings.timings.send_duration_ms") == 0) {
        c.settings.timings.send_duration_ms = static_cast<uint32_t>(v);
    } else if (strcmp(path->valuestring, "settings.buzzer_level") == 0) {
        c.settings.buzzer_level = static_cast<uint8_t>(v);
    } else if (strcmp(path->valuestring, "settings.led_level") == 0) {
        c.settings.led_level = static_cast<uint8_t>(v);
    } else {
        applied = false;
    }

    if (!applied) {
        Nack(for_seq, "unknown_path", path->valuestring);
        return;
    }
    // A patch that would make the config invalid is refused: the old config stays.
    if (!ConfigValidate(c) || !store_->Save(c)) {
        Nack(for_seq, "invalid", "patch produced an invalid config");
        return;
    }
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
    const int mv = static_cast<int>(key_mv->valuedouble);
    if (!sys_->TestDriveKeyMv(0, mv, kDefaultTestKeyHoldMs, hal_->now_ms(hal_->ctx))) {
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
    if (strcmp(pattern->valuestring, "flash") == 0) {
        sys_->Identify();
    } else if (strcmp(pattern->valuestring, "buzz") == 0) {
        sys_->Identify();
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
             "\"channel\":%u,\"button\":%s,\"gesture\":\"%s\",\"t_ms\":%llu,\"level_mv\":%d",
             static_cast<unsigned>(ev.channel_index), button, g,
             static_cast<unsigned long long>(ev.at_ms), ev.level_mv);
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
    const int channel = static_cast<int>(ch->valuedouble);
    if (channel < 0 || channel >= kMaxChannels) {
        Nack(for_seq, "bad_param", "channel out of range");
        return;
    }
    // A learn run that is already open is RE-STARTED rather than refused: the app
    // sends learn_start again when the user switches button, and refusing would
    // make the UI show an error for a legitimate action.
    learn_open_ = true;
    learn_channel_ = channel;
    learn_samples_ = 0;
    // The session is seeded with the channel's CURRENT profile so a learn that
    // overlaps an existing button can be refused (LearnReject::kTooCloseToExisting)
    // rather than silently creating two windows that classify the same level.
    LadderProfile existing{};
    if (store_ != nullptr) {
        Config cur{};
        if (store_->Load(&cur) == ConfigLoadResult::kLoaded) {
            existing = cur.channels[channel].ladder;
        }
    }
    session_.Start(channel, existing);

    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
}

void CommandRouter::HandleLearnStop(const cJSON *root, uint32_t for_seq) {
    (void)root;
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
    const int channel = static_cast<int>(ch->valuedouble);
    if (channel < 0 || channel >= kMaxChannels) {
        Nack(for_seq, "bad_param", "channel out of range");
        return;
    }

    // The session holds every sample the stream carried. The idle reference is
    // the LEARNED one from the running config (spec 6.3) -- reading the live idle
    // here would make the ratio denominator move with the rail, which is exactly
    // what renormalizing by a pinned idle exists to prevent.
    const int level_mv = sys_->FilteredLevelMv(static_cast<uint8_t>(channel));
    const int idle_mv = sys_->IdleReferenceMv(static_cast<uint8_t>(channel));
    session_.AddSample(level_mv, idle_mv, static_cast<MilliVolt>(idle_mv), 0,
                       hal_ ? hal_->now_ms(hal_->ctx) : 0);

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
    Config c{};
    if (store_->Load(&c) != ConfigLoadResult::kLoaded) c = ConfigDefault();
    LadderProfile &lp = c.channels[channel].ladder;
    if (lp.count < kLadderMaxButtons) {
        lp.buttons[lp.count++] = out;
        lp.learned_idle_mv = session_.LearnedIdleMv();
    } else {
        Nack(for_seq, "no_space", "the ladder has no free button slot");
        return;
    }
    if (!store_->Save(c)) {
        Nack(for_seq, "save_failed", "could not persist the learned button");
        return;
    }

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
    if (strcmp(t, "app") != 0 && strcmp(t, "bootloader") != 0) {
        Nack(for_seq, "bad_target", t);
        return;
    }
    char body[64];
    snprintf(body, sizeof(body), "\"for_seq\":%u,\"ok\":true", static_cast<unsigned>(for_seq));
    Emit("ack", body);
    // A reboot into the bootloader is a serial-flash helper: the device stops
    // running the app and does nothing else. Both targets reset, so the reboot
    // request itself is the same call; the distinction is what the app does next.
    hal_->reboot(hal_->ctx);
}
