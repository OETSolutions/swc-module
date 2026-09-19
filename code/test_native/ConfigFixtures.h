#pragma once

#include <cstdio>
#include <cstring>

#include "Config/ConfigCodec.h"
#include "Gesture/PressClassifier.h"   // GestureTimingsDefault

// The one valid config both config suites encode. Task 8's codec tests and Task 9's
// store tests must agree on what "a valid config" is, and two copies of this would
// drift -- the store's tests would then pass against a config the codec refuses.
// It lives in a header rather than in a .cpp because both suites are separate
// translation units and neither owns the other -- so it must be `inline`, or the
// two suites link two definitions of the same symbol and the suite fails to
// build with `duplicate symbol 'swctest::MakeConfig()'`.
//
// Every field this sets is asserted in JsonRoundTripsEveryFieldThatWasSet. If you
// add a field here, add its assertion there.
namespace swctest {

inline Config MakeConfig() {
    Config c{};
    c.schema_version = kConfigSchemaVersion;
    std::strncpy(c.device_id, "SWC-0001", sizeof(c.device_id) - 1);
    c.updated_at_ms = 1700000000000ULL;
    c.settings = DeviceSettings{};
    c.settings.timings = GestureTimingsDefault();
    c.settings.gain_policy = GainPolicy::kAuto;
    c.settings.buzzer_level = 2;
    c.settings.led_level = 2;
    c.settings.temp_comp_enabled = true;
    c.settings.maintenance_timeout_ms = 300000;   // spec 5-minute maintenance window
    c.channel_count = 1;
    c.channels[0].enabled = true;
    std::strncpy(c.channels[0].name, "SWC1", sizeof(c.channels[0].name) - 1);
    c.channels[0].ladder.learned_idle_mv = 2835;
    c.channels[0].ladder.count = 1;
    // The initializer sets the whole struct including id; a separate strncpy
    // before it would be overwritten and is not there. Fields are spec 3.4's
    // millivolts: 1430 mV at the 2835 idle is the 504 permille the classifier
    // derives (1430 x 1000 / 2835 = 504).
    c.channels[0].ladder.buttons[0] = {"VOL_UP", "Volume Up", 1430, 120, 3300, 235, 200, 98};
    c.channels[0].output.gain_mode = GainMode::kAmplified;
    c.channels[0].output.idle_dac_code = 4095;    // spec 3.7's default; full scale is the safe state (6.7)
    // Bindings are TOP-LEVEL (spec 3.1/3.5), keyed by (channel, button, gesture).
    c.binding_count = 2;
    std::strncpy(c.bindings[0].id, "b1", sizeof(c.bindings[0].id) - 1);
    c.bindings[0].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(c.bindings[0].button, "VOL_UP", sizeof(c.bindings[0].button) - 1);
    c.bindings[0].gesture = Gesture::kSingle;
    c.bindings[0].enabled = true;
    c.bindings[0].action_count = 1;
    c.bindings[0].actions[0].kind = ActionKind::kHwKey;
    c.bindings[0].actions[0].key_resistance_mohm = 24000;
    // The second binding is the product's core case (spec 3.5/3.6): one button
    // whose SINGLE drives the head unit while its DOUBLE tells the app, with a
    // data payload. A single-action, id-keyed Binding could not express this,
    // which is why the round trip below asserts BOTH actions survive.
    std::strncpy(c.bindings[1].id, "b2", sizeof(c.bindings[1].id) - 1);
    c.bindings[1].channel = static_cast<uint8_t>(BindingChannel::kSwc1);
    std::strncpy(c.bindings[1].button, "VOL_UP", sizeof(c.bindings[1].button) - 1);
    c.bindings[1].gesture = Gesture::kDouble;
    c.bindings[1].enabled = true;
    c.bindings[1].action_count = 2;
    c.bindings[1].actions[0].kind = ActionKind::kHwKeyRelease;
    c.bindings[1].actions[1].kind = ActionKind::kAppIntent;
    std::strncpy(c.bindings[1].actions[1].target, "com.oetsolutions.swc.ACTION_NAVIGATE",
                 sizeof(c.bindings[1].actions[1].target) - 1);
    std::strncpy(c.bindings[1].actions[1].payload, "geo:40.7608,-111.8910",
                 sizeof(c.bindings[1].actions[1].payload) - 1);
    return c;
}

}  // namespace swctest
namespace swctest {

// A config that SPANS SEVERAL NVS CHUNKS, which MakeConfig() does not.
//
// Every store test driven by MakeConfig exercises exactly one chunk, so the
// chunked path -- the entire reason kConfigChunkBytes exists and the reason the
// worst case is budgeted against the partition -- would otherwise never run.
// A store that wrote only chunk 0 would pass every test above.
//
// Sizes: 8 bindings x 2 actions with every string field filled gives a blob well
// past 2048 B, so the slot occupies several keys. The names are distinct per
// index so a chunk landing in the wrong slot is visible.
inline Config MakeBigConfig() {
    Config c{};
    c.schema_version = kConfigSchemaVersion;
    std::strncpy(c.device_id, "SWC-BIG", sizeof(c.device_id) - 1);
    c.updated_at_ms = 1700000000000ULL;
    c.settings.timings = GestureTimingsDefault();
    c.settings.gain_policy = GainPolicy::kAuto;
    c.settings.maintenance_timeout_ms = 300000;

    c.channel_count = 1;
    c.channels[0].enabled = true;
    std::strncpy(c.channels[0].name, "SWC1", sizeof(c.channels[0].name) - 1);
    c.channels[0].ladder.learned_idle_mv = 2835;
    c.channels[0].ladder.count = kLadderMaxButtons;
    for (int i = 0; i < kLadderMaxButtons; ++i) {
        char id[16], name[16];
        std::snprintf(id, sizeof(id), "btn_%02d", i);
        std::snprintf(name, sizeof(name), "Button %02d", i);
        c.channels[0].ladder.buttons[i] = LadderButton{};
        std::strncpy(c.channels[0].ladder.buttons[i].id, id,
                     sizeof(c.channels[0].ladder.buttons[i].id) - 1);
        std::strncpy(c.channels[0].ladder.buttons[i].name, name,
                     sizeof(c.channels[0].ladder.buttons[i].name) - 1);
        c.channels[0].ladder.buttons[i].mv_center = static_cast<MilliVolt>(200 + i * 10);
        c.channels[0].ladder.buttons[i].mv_tolerance = 5;
        c.channels[0].ladder.buttons[i].learned_at_rail_mv = 3300;
        c.channels[0].ladder.buttons[i].temp_c_at_learn = 235;
        c.channels[0].ladder.buttons[i].sample_count = 200;
        c.channels[0].ladder.buttons[i].confidence = 98;
    }
    c.channels[0].output.gain_mode = GainMode::kAmplified;
    c.channels[0].output.idle_dac_code = 4095;

    c.binding_count = 8;
    for (int i = 0; i < 8; ++i) {
        Binding &b = c.bindings[i];
        char id[16], btn[16];
        std::snprintf(id, sizeof(id), "bnd_%02d", i);
        std::snprintf(btn, sizeof(btn), "btn_%02d", i);
        std::strncpy(b.id, id, sizeof(b.id) - 1);
        b.channel = static_cast<uint8_t>(BindingChannel::kSwc1);
        std::strncpy(b.button, btn, sizeof(b.button) - 1);
        b.gesture = Gesture::kSingle;
        b.enabled = true;
        b.action_count = 2;
        // Long-ish strings so the encoded blob comfortably exceeds one chunk.
        std::strncpy(b.actions[0].target, "com.oetsolutions.swc.ACTION_NAVIGATE",
                     sizeof(b.actions[0].target) - 1);
        b.actions[0].kind = ActionKind::kAppIntent;
        std::strncpy(b.actions[0].payload, "geo:40.7608,-111.8910?q=Home",
                     sizeof(b.actions[0].payload) - 1);
        b.actions[1].kind = ActionKind::kHwKey;
        b.actions[1].key_resistance_mohm = 24000;
    }
    return c;
}

}  // namespace swctest
