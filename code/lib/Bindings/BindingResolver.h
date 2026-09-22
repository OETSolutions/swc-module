#pragma once

#include <stdint.h>

#include "Config/ConfigModel.h"
#include "Gesture/GestureStateMachine.h"

// The outcome of looking up a gesture. `found` and the actions are separate facts
// on purpose:
//
//   found == false          the gesture matches no binding. Nothing may happen,
//                           and a lower-priority source is free to claim it.
//   found && count == 0     the gesture IS bound, to an EMPTY list: deliberately
//                           inert (spec 3.5's "swallow the gesture"). The
//                           gesture is consumed and nothing else may take it.
//   found && count > 0      the binding's ordered action list, which the caller
//                           executes in order and best-effort (spec 3.5).
//
// The whole LIST is carried, not just the first action. Spec 3.5 makes a
// binding's `actions` "executed in order, each independently failable" and names
// the product's core case as ONE binding carrying both halves -- "emit the
// factory key press AND tell the app". Returning only `actions[0]` was a live
// defect: a binding stored `[APP_INTENT, OUT_VOLTAGE]` drove NO key at all,
// because the caller's only executable branch tested `actions[0]` and fell
// through to a release. The app side already iterates the full list
// (`AppViewModel.runAppSideAction`); this is the firmware's matching half.
//
// A collapsed single-action result is also what made "swallow the gesture" and
// "not bound" indistinguishably nullable at the call site.
struct ResolvedBinding {
    bool     found;
    uint8_t  action_count;
    Action   actions[kMaxActionsPerBinding];
};

// Resolve a gesture on a channel to the ordered actions its binding names.
//
// Takes the WHOLE Config and a channel INDEX rather than a ChannelConfig, because
// spec 3.5 puts `bindings` in a top-level table whose entries carry their own
// `channel` (SWC1|SWC2|AUX1..3|ANY). Resolving against one channel could not see a
// binding's channel field, could not honour ANY, and could not resolve the
// `LadderButton.id` in `Binding.button` -- the ladder lives on the channel while
// the binding lives at the top level, so both must be addressable at once.
ResolvedBinding BindingResolve(const Config &cfg, uint8_t channel_index,
                               const GestureEvent &ev);
