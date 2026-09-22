#include "Bindings/BindingResolver.h"

#include <string.h>

#include "Bindings/ActionLibrary.h"

ResolvedBinding BindingResolve(const Config &cfg, uint8_t channel_index,
                               const GestureEvent &ev) {
    ResolvedBinding out{};
    out.found = false;
    out.action_count = 0;
    if (channel_index >= cfg.channel_count) return out;
    // NOT gated on `cfg.channels[channel_index].enabled`. Spec 3.4: `enabled`
    // gates whether the channel's ladder is CLASSIFIED, not whether its bindings
    // resolve -- that is `Binding.enabled`, checked per binding below. Gating here
    // was a live defect: `ConfigDefault` ships every channel disabled, so this
    // returned not-found for every binding on a fresh device, and the caller's
    // spec-6.6-rule-4 fallback then presented the button's own level in place of
    // the action the user bound -- a SILENTLY WRONG key voltage rather than no
    // output at all.
    // Every count in a Config that arrives from storage is untrusted, and the
    // SAME rule applies to each: a count past its array is a corrupt config, so
    // refuse rather than index. `binding_count` is a uint8_t and `kMaxBindings`
    // is 32, so an unchecked loop reads past the end of the table -- the same
    // fault the action_count check below exists to prevent, on a bigger array.
    if (cfg.binding_count > kMaxBindings) return out;

    // The button the gesture fired on, by id. The event carries an index into
    // that channel's ladder (Task 6), and the binding names the id, so this is
    // the join between the two.
    const LadderProfile &ladder = cfg.channels[channel_index].ladder;
    if (ladder.count > kLadderMaxButtons) return out;
    if (ev.button_index >= ladder.count) return out;
    const char *button_id = ladder.buttons[ev.button_index].id;

    // `channel_index` indexes the two steering-wheel channels only. The AUX
    // inputs are a separate table (`cfg.aux`) with their own `source` field and
    // no ladder, so they do not arrive here in v1. Mapping an AUX input onto a
    // channel_index would be inventing spec: the binding's channel field can
    // name AUX1..3, but nothing in the spec says what index resolves them.
    const uint8_t as_swc = channel_index == 0
        ? static_cast<uint8_t>(BindingChannel::kSwc1)
        : static_cast<uint8_t>(BindingChannel::kSwc2);

    for (uint8_t i = 0; i < cfg.binding_count; ++i) {
        const Binding &b = cfg.bindings[i];
        if (!b.enabled) continue;                    // disabled: do not match at all
        if (b.gesture != ev.gesture) continue;
        if (b.channel != as_swc &&
            b.channel != static_cast<uint8_t>(BindingChannel::kAny)) continue;
        if (strcmp(b.button, button_id) != 0) continue;
        // A count past the array is a corrupt config. Refuse rather than
        // truncate: a shortened action list is a binding that fires differently
        // from the one that was stored.
        if (b.action_count > kMaxActionsPerBinding) return out;
        // An empty list is legal and MEANS "swallow the gesture" (spec 3.5), so
        // it is found-and-inert, not not-found: `found` true with a zero count.
        if (b.action_count == 0) { out.found = true; return out; }
        // The whole ordered list, executed best-effort by the caller (spec 3.5).
        // EVERY action is checked, not just the first: an action that cannot be
        // executed is refused whole, because silently dropping it would fire a
        // different binding than the one stored -- and now that the caller runs
        // the list, the un-checked tail is reachable rather than dead.
        for (uint8_t a = 0; a < b.action_count; ++a) {
            if (!ActionIsExecutable(b.actions[a])) return out;
            out.actions[a] = b.actions[a];
        }
        out.action_count = b.action_count;
        out.found = true;
        return out;
    }
    return out;
}
