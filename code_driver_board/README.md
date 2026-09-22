# SWC rig driver — the second board that drives the device under test

This project is the firmware for the board that **presents stimuli to the SWC
adapter under test** so its firmware can be exercised end to end with no vehicle
and no steering-wheel pod.

It is deliberately **separate from the two other firmware projects in this repo**:

| Project | What it is | Board |
| --- | --- | --- |
| `code/` | the product firmware | the device under test (DUT) |
| `code_board_test/` | the bring-up / hardware-test tool | one board, self-contained |
| **`code_driver_board/`** | **this** — the rig driver | the second board |

It shares no build with either. The hardware facts it needs (pin map, DAC frame,
servo transfer function) are **copied** from `code_board_test`, which in turn
cross-checked them against `production/netlist.ipc`. Both boards are the same
hardware, so the pin map is identical.

## The rig

Two SWC boards. The **driver** board's analog output is wired into the **DUT**
board's analog input; the driver presents a *button* by sinking that node, exactly
as a real button on a real steering wheel does.

```
        DRIVER BOARD                            DUT BOARD (code/)
        ┌──────────────┐                        ┌──────────────┐
        │ J3.3 KEY1 ───┼──── ladder stimulus ───┼─ J2.3 SWC1   │
        │ J3.2 KEY2 ───┼────────────────────────┼─ J2.2 SWC2   │
        │ J5.4 AUX1 ───┼──── programming sw. ───┼─ J5.4 AUX1   │
        │          GND ┼────────────────────────┼─ GND         │
        └──────────────┘                        └──────────────┘
             USB (console)                    USB (console + Android app)
```

- **A press is a low KEY voltage.** The driver's servo sinks the DUT's ladder
  node; releasing lets the DUT's own 10 kΩ pull-up set the node high again. There
  is no separate "button" wire — the analog level *is* the button.
- **AUX1 is the programming switch.** The driver's AUX1 pin sinks the DUT's AUX1
  to GND (R23 is shorted on the DUT, so the 1 kΩ series resistance is gone). Closed
  = programming button held. Open = the DUT's pull-up holds it released.
- **Common ground is required.** Without it nothing reads correctly and the failure
  looks like a dead DUT.

The loopback jumpers the bring-up tool used are **not** part of this rig — those
existed only to verify a freshly-assembled board's own signal chain, and they are
not needed here.

## What it can present

| Command | Action |
| --- | --- |
| `1` | a **SINGLE** press — press, release, then wait out the DUT's double-press window |
| `2` | a **DOUBLE** press — two presses inside the window, then the window |
| `3` | a **LONG** press — hold past the DUT's long-press threshold |
| `p` / `r` | press and hold / release, for manual timing |
| `l` | **LEARN** — close AUX1 *and* hold the key, then release (the §7.4 entry gesture) |
| `L` | **MAINTENANCE** — hold AUX1 alone ~3 s (opens the ~5 min window) |
| `a` | toggle the AUX1 programming switch by hand |
| `c` | toggle which DUT channel (SWC1 / SWC2) the stimulus is presented on |
| `m` / `M` | cycle the gain mode / auto-select it from the released level |
| `+` / `-` | nudge the commanded key level by 50 mV |
| `d` | diagnostic — what the KEY line rests at, and whether a command is reachable |
| `w` | watch the DUT's KEY **output** (loopback) for ~2 s, min/max/mean |
| `s` | status |

A gesture is a **timeline**: a press of `press_on_ms`, held, at a commanded key
voltage `key_mv`, and the DUT's own limits decide whether that is a SINGLE, a
DOUBLE or a LONG. Those limits are mirrored from the product firmware
(`code/lib/Gesture/PressClassifier.h`) in `include/DriverHarness.h` and asserted
by the host tests, so a mistimed press is a **failed host test**, not a
misclassified gesture on the bench.

## Building and running

```bash
cd code_driver_board

# Host tests — the gesture-timeline geometry, no board needed (~1 s)
pio test -e host

# Device build
pio run -e swc-s3-driver
```

## The one non-obvious build fact

**`lib/swc_logic` is compiled explicitly, through `build_src_filter`, not by the
library dependency finder.** In this project LDF resolves only the framework
libraries and never the project's own `lib/` — verified by probing with a trivial
library referenced directly from `src/`, which LDF still did not find. The failure
mode is not a clear one: LDF reports success, the pure-logic sources are simply
absent from the link, and the build dies with a wall of `undefined reference to
Output::` errors that read as a code problem. Both envs compile the sources
explicitly; the device env adds `+<../lib/*/*.cpp>` to the default `+<*>`.

## Timing, and what to change when the DUT moves

`Harness::kDefaultRig`:

| Field | Default | Bound by |
| --- | --- | --- |
| `key_mv` | 1240 | the learned button's `mv_center` on the DUT channel |
| `press_on_ms` | 150 | must be ≥ debounce (25) and < long (750) |
| `inter_press_ms` | 120 | must be < the double window (500) and > 0 |
| `long_press_ms` | 750 | the DUT's long-press threshold |
| `double_window_ms` | 500 | the DUT's double-press window |

`Harness::Validate()` returns the first way the config fails to produce the
gesture it names, and `s` prints it — so a typo is a message, not a silent
misclassification.

## Reading a result

The driver only **presents** stimuli. What the DUT *does* with them it reports
itself: its buzzer and LEDs on the bench, and its USB frames (visible in the
Android app or a serial monitor on the DUT's port). The driver prints what it
drove; the DUT tells you what it resolved. Compare the two.

The **loopback** (DUT SWC out → driver SWC in) closes the other direction: `w`
reads what the DUT is actually commanding on its KEY line, so "the DUT emitted an
event but nothing reached the head unit" is told from a reporting bug.

## Findings from the first session on hardware (2026-09-24)

The rig works end to end. The analog path was proven with the DUT's own frames:
with the driver presenting 1240 mV on channel 1, the DUT's `ladder_sample`
stream read **1206 mV**, a `learn_commit` stored it as `mv_center:1205,
mv_tolerance:120`, and a subsequent synthetic SINGLE classified as
`event{channel:0, button:"rig1", gesture:"SINGLE"}` — with `config_state`
flipping `"none"` → `"ok"`.

**One DUT defect found: a learned-only button (no binding) re-emitted its gesture
every poll tick while held, and never advanced to a later gesture.** Measured on
hardware: a 2 s steady hold at the learned level produced 26–177 identical
`event{…,"gesture":"SINGLE"}` frames ~70 ms apart, versus exactly 1 at a higher
button level.

The mechanism, from the loopback (`w`): the DUT drives its KEY pulse, and its
**own** KEY sense reads that pulse below the 1800 mV envelope floor, so
`head_unit_gone` went true, the tail did `ReleaseKey + gestures.Reset()` every
tick, and the next tick re-classified the still-held press and re-emitted. A
learned-only button (the headless learn path — `ConfigDefault` ships
`binding_count = 0`) maps to the command-band floor of 1800 mV, which is what
tripped it; a bound `OUT_VOLTAGE` at a higher level did not. `gestures.Reset()` on
the recurring `head_unit_gone` condition also discarded the press state, so a held
button could never reach LONG.

Not a rig problem, and not a duplicate-emit bug in the emit path — the
head-unit-gone response acting on the DUT's own output through its own sense node.

**FIXED 2026-09-24** in `SystemOrchestrator::HeadUnitGone`. `V_KEY_idle` is the
line's IDLE, so the envelope is only meaningful on a *released* line; while the
device DRIVES, the sense node reads the device's own output and there is no
head-unit reading to take. A driven line is now judged only on a DEEP sag: every
command clamps to the band floor, so a reading at or above `kFaultSagMaxMv`
(1600 mV, the floor less a 200 mV margin) is one we produced, not a fault. A rail
collapse still drives the line far below that and still releases (FR-39's
phantom-key hazard). The verdict also now has to persist for 250 ms
(`kHeadUnitGoneSettleMs`), so a line settling after a pulse — or a rail coming up
at boot — is not mistaken for an absent head unit.

Regression test `ADrivenLineAtTheCommandFloorIsNotAHeadUnitGoneFault`
(`SystemOrchestratorTest.cpp`), mutation-checked: removing the sag guard fails
exactly that test. `ARailSagDuringAPressReleasesTheKey` still covers the deep-sag
release. Native suite 503 → 504.


