#!/usr/bin/env python3
"""Every frame must have a handler on the receiving side -- in EVERY direction.

This pins the defect class that produced N-22, N-24 and N-45, stated as one
rule: a frame in the vocabulary that the app never handles is a message the
device sends into a void. The three shapes it has actually taken:

  - N-22: `status` declared fields (`rail_mv`, `temp_c`, `heap_free`) with no
    producer -- the app read a field that never arrived.
  - N-24: `UsbCdc`'s two loss counters were produced and reported to nobody
    (the frame-side repair put them in `status`; this guard covers the reverse
    direction, a frame the app never looks at).
  - N-45: `AppViewModel.actionOutcomes` was produced and rendered nowhere.
  - `link_gap` was the mirror image on the frame axis: declared in the contract,
    defined as a Kotlin constant, and handled by NOTHING for a whole revision,
    so a dropped frame vanished with no explanation on either end.

THREE checks, one per way a frame can fail to arrive anywhere:

  1. every `fw2app`/`both` frame has an app-side handler (and a generated
     constant to name it by);
  2. every `app2fw`/`both` frame is accepted by the router at all -- a name the
     app can send that `IsKnownCommand` omits is nacked `unknown_type` at
     runtime, so nothing looks wrong until a user tries the command;
  3. every ACCEPTED command reaches a real branch, rather than falling into the
     trailing `else` and being nacked `not_implemented` -- which names the wrong
     reason. `ota_*` is the one deliberate exception in this build.

They are deliberately structural rather than semantic: none of them can tell a
handler that does nothing useful from one that works. What they can tell is that
the frame NAME arrives somewhere, which is the part that silently rots -- adding
a frame and forgetting the other side is a one-line omission with no other
observable symptom.

`Contract.kt` is GENERATED from `contract_schema.py`, so this reads the schema
rather than the Kotlin file: the Kotlin constant is not what decides whether a
frame is on the wire.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CODE = os.path.dirname(HERE)
APP = os.path.join(CODE, "android", "app", "src", "main", "java", "com",
                   "oetsolutions", "swc")

sys.path.insert(0, HERE)
import contract_schema  # noqa: E402


def _app_sources():
    out = {}
    for root, _dirs, files in os.walk(APP):
        for fn in files:
            if fn.endswith(".kt"):
                p = os.path.join(root, fn)
                out[p] = open(p, encoding="utf-8").read()
    return out


def _router_known_commands():
    """The command names the firmware's dispatcher accepts.

    Read from `IsKnownCommand` rather than a second hand-kept list, for the same
    reason `_router_emitted_fields` reads the router: a list kept beside the code
    is the thing that goes stale. An unknown command is nacked `unknown_type`, so
    a frame in the contract that this array omits is one the app can send and the
    device will never accept.
    """
    text = open(os.path.join(CODE, "lib", "Link", "CommandRouter.cpp"),
                encoding="utf-8").read()
    m = re.search(r"bool IsKnownCommand\(const char \*type\) \{(.*?)\n\}", text, re.S)
    if not m:
        return None
    return set(re.findall(r'"([a-z_0-9]+)"', m.group(1)))


def _router_dispatched_commands():
    """The command names the router actually ROUTES, i.e. appear in a `strcmp`
    inside the dispatch chain of `OnLine`.

    Separate from `IsKnownCommand` on purpose: that array decides whether a
    frame is nacked `unknown_type`, while the dispatch chain decides whether it
    reaches a handler. A name in the first and not the second falls into the
    trailing `else` and is nacked `not_implemented` -- which is the CORRECT
    answer for `ota_*` in this build, and a silent wrong answer for anything
    else. Only the names reachable from a real branch are listed here.
    """
    text = open(os.path.join(CODE, "lib", "Link", "CommandRouter.cpp"),
                encoding="utf-8").read()
    m = re.search(r"if \(strcmp\(h\.type, \"ping\"\) == 0\).*?\n\}", text, re.S)
    if not m:
        return None
    return set(re.findall(r'strcmp\(h\.type, "([a-z_0-9]+)"\)', m.group(0)))


def main():
    srcs = _app_sources()
    if not srcs:
        print("check_frame_handlers: no app sources found under %s" % APP)
        return 1

    # Where frames are dispatched. One file owns the inbound switch today; any
    # file that references the constant counts as handling it, so the guard does
    # not break if the dispatch moves.
    blob = "\n".join(srcs.values())

    # Which frames the app could receive. `app2fw` frames are sent BY the app,
    # so no handler is expected; `both` (the config_* trio) is included because
    # the firmware really does emit them as a config_get reply.
    expected = [f.name for f in contract_schema.FRAMES if f.direction in ("fw2app", "both")]

    missing = []
    for name in expected:
        const = "Frames." + re.sub(r"[^A-Za-z0-9]", "_", name).upper()
        # The constant is generated from the same schema, so a name with no
        # constant is a schema/generator drift, not an app gap -- report it too,
        # because the app literally cannot name the frame.
        if not re.search(r"const val %s\b" % re.escape(const.split(".", 1)[1]),
                         open(os.path.join(APP, "contract", "Contract.kt"),
                              encoding="utf-8").read()):
            missing.append((name, "no generated constant " + const))
            continue
        # A handler is any reference to the constant OUTSIDE the generated
        # contract file itself.
        uses = 0
        for p, s in srcs.items():
            if p.endswith("contract/Contract.kt"):
                continue
            uses += len(re.findall(r"\b%s\b" % re.escape(const), s))
        if uses == 0:
            missing.append((name, "no app-side handler references " + const))

    if missing:
        print("check_frame_handlers: FAILED")
        for name, why in missing:
            print("  frame `%s`: %s" % (name, why))
        print(
            "A frame the firmware emits and the app never handles is a message\n"
            "the device sends into a void -- the shape of N-22/N-24/N-45 on the\n"
            "frame axis (and of `link_gap`, which sat unhandled for a revision).\n"
            "Handle it, or remove it from the schema's fw2app direction."
        )
        return 1

    # The INVERSE direction. A frame the app is told it may send, that the
    # firmware's dispatcher does not accept, is nacked `unknown_type` at runtime
    # -- the app has a generated constant for it, so the omission is invisible
    # until someone tries the command. `IsKnownCommand` is the closed set the
    # nack is decided on, so that is what this is compared against.
    known = _router_known_commands()
    if known is None:
        print("check_frame_handlers: could not locate IsKnownCommand in the router")
        return 1
    app2fw = [f.name for f in contract_schema.FRAMES if f.direction in ("app2fw", "both")]
    unhandled = sorted(n for n in app2fw if n not in known)
    if unhandled:
        print("check_frame_handlers: FAILED")
        for n in unhandled:
            print("  frame `%s`: the app can send it and the router never accepts it" % n)
        print("It would be nacked `unknown_type` at runtime, with the app holding a\n"
              "generated constant for it -- so nothing looks wrong until a user\n"
              "tries the command. Add it to IsKnownCommand, or correct the\n"
              "schema's direction.")
        return 1

    # Every accepted command must REACH a branch. `ota_*` is the one deliberate
    # exception: this build nacks it `not_implemented`, which is a true answer
    # rather than a no-op, and the spec records that as an open item. Anything
    # else accepted-but-not-dispatched is a command the app can send that gets a
    # nack naming the wrong reason.
    DISPATCH_EXEMPT = {"ota_begin", "ota_chunk", "ota_end"}
    dispatched = _router_dispatched_commands()
    if dispatched is None:
        print("check_frame_handlers: could not locate the router's dispatch chain")
        return 1
    undelivered = sorted(n for n in known - dispatched - DISPATCH_EXEMPT)
    if undelivered:
        print("check_frame_handlers: FAILED")
        for n in undelivered:
            print("  command `%s`: accepted by IsKnownCommand but no branch handles it" % n)
        print("It reaches the trailing `else` and is nacked `not_implemented` --\n"
              "which names the wrong reason. Add a branch, or move it to the\n"
              "exempt set if this build truly does not implement it.")
        return 1

    print("check_frame_handlers: OK (%d firmware->app frames handled, %d app->firmware "
          "frames accepted, %d dispatched)" % (len(expected), len(app2fw), len(dispatched)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
