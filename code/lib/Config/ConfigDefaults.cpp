#include "Config/ConfigDefaults.h"

#include <string.h>

#include "Output/GainPolicy.h"   // kDacMaxCode: full scale is the SAFE idle (spec 6.7)

Config ConfigDefault() {
    Config c{};
    c.schema_version = kConfigSchemaVersion;
    // A NON-EMPTY device id. `ReadStr` refuses an empty string, so a config with
    // `device_id: ""` encodes fine and then cannot be decoded by the very codec
    // that wrote it -- a round-trip violation (FR-27). ConfigValidate now refuses
    // an empty id as well, so this can only be reached by a caller that builds a
    // config by hand.
    strncpy(c.device_id, "SWC-0000", sizeof(c.device_id) - 1);
    c.settings.timings = GestureTimingsDefault();
    c.settings.gain_policy = GainPolicy::kAuto;
    c.settings.buzzer_level = 2;
    c.settings.led_level = 2;
    c.settings.maintenance_timeout_ms = 300000;

    c.channel_count = kMaxChannels;
    for (uint8_t i = 0; i < kMaxChannels; ++i) {
        ChannelConfig &ch = c.channels[i];
        // Disabled: nothing has been learned yet, so there is no ladder to
        // classify against. The channel is still NAMED and its idle reference is
        // still plausible, so the config validates -- see the header.
        ch.enabled = false;
        strncpy(ch.name, (i == 0) ? "SWC1" : "SWC2", sizeof(ch.name) - 1);
        ch.ladder.learned_idle_mv = 2835;   // spec 3.7's rail, measured at the pin
        ch.ladder.count = 0;
        ch.output.gain_mode = GainMode::kAmplified;
        // Full scale is the safe state: the output stage only sinks, so a high
        // command RELEASES the line (spec 6.7). Never low.
        ch.output.idle_dac_code = static_cast<uint16_t>(kDacMaxCode);
    }
    c.aux_count = 0;
    c.binding_count = 0;   // no bindings: pass-through, nothing acts on a gesture
    return c;
}
