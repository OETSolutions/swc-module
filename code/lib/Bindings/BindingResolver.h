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

/*
 * The ONE scan, by the three fields a binding actually matches on: its `channel`
 * (the input the press came from), its `button` (that input's id), and the
 * gesture. Everything below is a thin wrapper that derives those three.
 *
 * **Why the core takes the binding's `channel` ORDINAL and an id, rather than a
 * channel index.** `Binding.channel` is spec 3.5's input enum
 * (`SWC1|SWC2|AUX1..3|ANY`) and `Binding.button` is the INPUT's own id: a
 * `LadderButton.id` for a wheel channel, an `AuxButtonConfig.id` for an AUX
 * input (spec 3.1). The two input families are different tables with different
 * id sources, so a resolver that took only a channel index could not see an AUX
 * input at all -- which is exactly why an AUX binding used to be accepted and
 * never fire (open item N-26). Making the shared scan take the two facts it
 * matches on is what lets both families reach the same precedence rule.
 *
 * `as_channel` is the `BindingChannel` ordinal of the input the press came from.
 * A binding matches when its channel equals that OR is `kAny` (the wildcard). The
 * FIRST match wins and the scan stops, so a binding's POSITION is its precedence
 * (spec 3.5): an `ANY` binding ahead of a channel-specific one for the same
 * `(button, gesture)` wins, and the specific binding is dead.
 */
ResolvedBinding BindingResolveInput(const Config &cfg, uint8_t as_channel,
                                    const char *button_id, Gesture gesture);

/*
 * Resolve a gesture on a steering-wheel channel.
 *
 * Takes the WHOLE Config and a channel INDEX rather than a ChannelConfig, because
 * spec 3.5 puts `bindings` in a top-level table whose entries carry their own
 * `channel` (SWC1|SWC2|AUX1..3|ANY). Resolving against one channel could not see a
 * binding's channel field, could not honour ANY, and could not resolve the
 * `LadderButton.id` in `Binding.button` -- the ladder lives on the channel while
 * the binding lives at the top level, so both must be addressable at once.
 */
ResolvedBinding BindingResolve(const Config &cfg, uint8_t channel_index,
                               const GestureEvent &ev);

/*
 * Resolve a gesture on one of the AUX inputs (spec 3.1/3.5).
 *
 * `aux_index` indexes `cfg.aux` (0 = AUX1, 1 = AUX2, 2 = AUX3), and the binding
 * channel it resolves against is `kAux1 + aux_index` -- the same ordinal the wire
 * `event` frame reports, so a press the device acted on and the frame the app
 * reads name the same input.
 *
 * **AUX1 is index 0 and is not serviced as a gesture input**, because spec
 * 7.5/8.2 give it the programming and maintenance holds; the caller only reaches
 * here for AUX2/AUX3 (`ConfigValidate` refuses a binding on AUX1 for the same
 * reason). This wrapper does not encode that rule -- it resolves whatever index it
 * is handed -- so the single home for the decision stays in the validator and the
 * orchestrator, not in a third place.
 *
 * An AUX input carries exactly ONE button (its own id), so `ev.button_index` must
 * be 0; any other value names a button that input does not have.
 */
ResolvedBinding BindingResolveAux(const Config &cfg, uint8_t aux_index,
                                  const GestureEvent &ev);
