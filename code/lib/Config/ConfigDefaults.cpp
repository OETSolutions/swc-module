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
        // ENABLED. Spec 3.4: `enabled` gates whether this channel's ladder is
        // CLASSIFIED, not whether its bindings resolve. Nothing has been learned
        // yet -- `count` is 0 -- and that is what the flag used to be made to
        // stand in for, which was wrong: it made a config that serialises back
        // to the app carry "off" for both channels, and (before the binding
        // resolve was un-gated) it made every app-pushed binding unfindable on a
        // fresh device, so a bound action was silently replaced by the
        // pass-through default. A channel with no learned buttons is described
        // by its zero `count`; a channel that is not present at all is described
        // by `channel_count`.
        ch.enabled = true;
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
