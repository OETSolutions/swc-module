#pragma once

#include "Config/ConfigCodec.h"
#include "Config/ConfigModel.h"

// Can the firmware actually run this action?
//
// A thin alias for ActionIsWellFormed, NOT a re-derivation. The codec (which
// decides whether a config may be stored) and the resolver (which decides whether
// a stored binding may fire) must agree on what "runnable" means; two copies of
// that predicate drift the first time a kind or a required field changes, and the
// drift is silent -- a config that saves and then cannot execute.
//
// The name differs from the codec's because the two callers are asking different
// questions of the same rule: the codec asks "is this storable", the resolver asks
// "is this executable". Naming it for the caller's question is what makes the
// resolver's refusal read correctly at the call site.
bool ActionIsExecutable(const Action &a);
