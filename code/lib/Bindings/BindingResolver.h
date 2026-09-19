#pragma once

#include <stdint.h>

#include "Config/ConfigModel.h"
#include "Gesture/GestureStateMachine.h"

// The outcome of looking up a gesture. `found` and the action are separate facts
// on purpose:
//
//   found == false  the gesture matches no binding. Nothing may happen, and a
//                   lower-priority source is free to claim it.
//   found == true   the gesture IS bound. `action.kind == kNone` means it is
//                   deliberately inert (an empty action list, spec 3.5) -- the
//                   gesture is consumed and nothing else may take it.
//
// Collapsing those two into a single nullable action is what makes "swallow the
// gesture" and "not bound" indistinguishable at the call site.
struct ResolvedAction {
    bool   found;
    Action action;
};

// Resolve a gesture on a channel to the action its binding names.
//
// Takes the WHOLE Config and a channel INDEX rather than a ChannelConfig, because
// spec 3.5 puts `bindings` in a top-level table whose entries carry their own
// `channel` (SWC1|SWC2|AUX1..3|ANY). Resolving against one channel could not see a
// binding's channel field, could not honour ANY, and could not resolve the
// `LadderButton.id` in `Binding.button` -- the ladder lives on the channel while
// the binding lives at the top level, so both must be addressable at once.
ResolvedAction BindingResolve(const Config &cfg, uint8_t channel_index,
                              const GestureEvent &ev);
