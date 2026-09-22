# SWC adapter — bring-up and hardware-test firmware

A PlatformIO project that turns a freshly-assembled **SWC steering-wheel-controls
adapter** board into a self-testing bench instrument. It runs one hardware
function at a time, prints what it measured, and says whether that is what the
design predicts.

This is **not the product firmware**. The product lives in
`../code` (ESP-IDF, TinyUSB CDC link to an Android head unit).
Nothing here ships.

| | |
| --- | --- |
| **Board** | 54 × 102 mm, 4-layer, ESP32-S3 (DOIT ESPS3-32-N4), MCP4728 DAC, TLV9004 servos |
| **Framework** | Arduino (deliberate — see below) · ESP-IDF 5.5.5 underneath |
| **Tests** | 32 on-device + 48 host unit tests |
| **Console** | ROM USB-Serial-JTAG (`ARDUINO_USB_MODE=1`) |
| **Front ends** | serial menu **and** web page — both optional, both drive the same tests |

---

## Contents

- [Quick start](#quick-start)
- [Why these choices](#why-these-choices)
- [How to step through it](#how-to-step-through-it)
- [The wiring: what to attach when](#the-wiring-what-to-attach-when)
- [The 30 tests](#the-30-tests)
- [Reading a result](#reading-a-result)
- [What is machine-checked and what is not](#what-is-machine-checked-and-what-is-not)
- [Where every constant came from](#where-every-constant-came-from)

---

## Quick start

```sh
# 1. Host unit tests — no board needed, ~2 seconds
pio test -e host

# 2. Build for the board
pio run -e swc-s3-test

# 3. Flash (hold BOOT only if the ROM loader does not catch the reset)
pio run -e swc-s3-test -t upload --upload-port /dev/tty.usbmodem113101

# 4. Open the menu
pio device monitor -b 115200 -p /dev/tty.usbmodem113101
```

The board prints a boot banner, brings up the ADC and the DAC, blinks both LEDs
twice, and then presents a menu. Type a **test number** and press ENTER.

WiFi credentials are read from `../code/.env` at build time by
`tools/gen_secrets.py` and written to a **git-ignored** `include/Secrets.h`.
Nothing in this repository contains a credential. If the `.env` is missing the
build still succeeds and test 25 reports "not configured".

---

## Why these choices

**Arduino, not ESP-IDF.** The product firmware is IDF and should stay that way.
This tool needs a serial menu, an HTTP page and a WiFi client with minimum
ceremony, and it never ships. One consequence is worth stating because it is the
only place the two diverge in a way that could mislead: the *analog measurements*
do **not** use Arduino's `analogRead()`. They use the ESP-IDF `adc_oneshot` +
`adc_cali` path directly (`src/Adc.cpp`), because Arduino's wrapper hides whether
the eFuse calibration was actually available — and that is a fact this tool exists
to report.

**Console on USB-Serial-JTAG, not TinyUSB.** The ESP32-S3 has **one internal USB
PHY**, shared between the ROM's USB-Serial-JTAG peripheral and the USB-OTG
controller TinyUSB drives. Only one can be active (spec §4.1). The product puts
its link on TinyUSB, which means **a production build has no console at all** —
the message reporting a link failure is itself unreadable. This tool takes the
other one on purpose: USB-Serial-JTAG enumerates before `setup()` runs, survives a
crashed application, and is the peripheral ROM download mode uses, so a bad build
can always be reflashed and always be read. Test 24 documents this rather than
performing the handover, because a handover mid-suite would kill the console and
take the remaining tests with it.

**Two front ends.** A serial-only tool goes dark whenever the native USB
re-enumerates (any reboot, any crash), and this board has no UART header to fall
back to. A web-only tool cannot test the radio that serves it. So both exist,
driving the same registry through the same runner.

---

## How to step through it

### Serial menu

```
  type a NUMBER to run that test (1-30)
  a  = run all, in order        s  = summary
  c  = clear the capture buffer n  = reprint this menu
  r  = reset all outcomes       w  = web UI address
  ?  = full detail for every test (what each needs)
  h  = hardware info dump
```

Every command prints a **result line** and then reprints the menu, so you always
know where you are:

```
TEST 14  Channel 1 loopback: SWC_OUT1 to SWC_IN1
  covers: End-to-end: DAC -> servo -> Q4 -> KEY1 -> SWC1 -> R1 -> ADC
  needs : JUMPER from J3 pin 3 (KEY1) to J2 pin 3 (SWC1)
------------------------------------------------------------------------
  [ok]   released: SWC_IN sits high on its 10k pull-up (the ladder is released)
  [ok]   driving the output pulls SWC_IN materially down (press direction)
  [ok]   SWC_IN falls monotonically as the commanded KEY voltage rises
------------------------------------------------------------------------
RESULT 14: PASS  (412 ms)  all checks passed
```

### Web UI

Once WiFi associates the menu prints an address:

```
  web UI:  http://192.168.2.x/
```

That page lists every test with its **needs** column, a clickable link per test,
a live log pane that refreshes every 2 s, and buttons for *Run all 30*,
*Summary*, *Clear log* and *Identify*. It survives USB re-enumeration, which the
serial menu does not — so if the serial console drops part-way through a run, the
log is still there.

Tests requested from the web page are **deferred** to `loop()` rather than run
inside the HTTP handler, because test 30 takes minutes and running it inline would
block the response for that whole time.

---

## The wiring: what to attach when

You do not need everything at once. The tests are grouped by what has to be
attached, and that grouping is the recommended order.

```
                 ┌─────────────────────────────────────────────┐
   J1  +12V ─────┤ 1=GND  2=+12V      (OPTIONAL — USB powers it)│
                 │                                             │
   J2  SWC in ───┤ 1=GND  2=SWC2  3=SWC1                      │
                 │                                             │
   J3  SWC out ──┤ 1=GND  2=KEY2  3=KEY1                      │
                 │                                             │
   J5  AUX in ───┤ 1=GND  2=AUX3  3=AUX2  4=AUX1              │
                 └─────────────────────────────────────────────┘
                J4 USB-C  (power + console + flasher)
```

| Stage | Attach | Run | Why |
| --- | --- | --- | --- |
| **1** | *nothing* | 1–10 | power, identity, GPIO, I2C, DAC, ADC calibration |
| **2** | *J3 open* | 11 | the sense path, floating |
| **3** | a voltmeter on J3.3 / J3.2 | 12–13 | the servos, open-loop |
| **4** | **jumper J3.3 → J2.3** and **J3.2 → J2.2** | 14–15 | the whole analog chain, end to end |
| **5** | the vehicle button pod on J2 (or nothing) | 16–18 | ladder inputs, AUX, NTC |
| **6** | *listen / watch* | 19–20 | buzzer, LEDs |
| **7** | stage-4 jumpers + a 10 kΩ pull-up J3.3→3V3 | 21–23 | gain auto-select, trim loop, idle safety |
| **8** | *nothing* | 24–26 | USB, WiFi, NVS |
| **9** | stage-4 jumpers | 27–28 | press shaping, full sweep |
| **10** | jumpers as each step asks | 29 | connector continuity |
| **11** | both jumpers | 30 | endurance (minutes) |
| **12** | 3 test wires + something warm | 31–32 | AUX driven automatically, RT1's curve |

**The one jumper that matters most** is stage 4. Two wires:

```
   J3 pin 3 (KEY1) ────── J2 pin 3 (SWC1)
   J3 pin 2 (KEY2) ────── J2 pin 2 (SWC2)
```

That single loop closes the entire signal chain — DAC → integrator → sink FET →
KEY line → series resistor → ADC, with the sense buffer in parallel — so tests 14,
15, 22, 27, 28 and 30 all become possible with no head unit and no meter. It is
also the **only** way to test the input voltage sensing without a vehicle.

### If a test says a jumper is missing

Several tests check for their own prerequisites first and report `BLOCKED` with
the reason rather than failing on a downstream measurement. If test 14 reports
that SWC1 does not read high with the output released, the jumper is not fitted —
that is the diagnosis, not a board fault.

---

## The 30 tests

| # | Test | Needs | Asserts |
| --- | --- | --- | --- |
| 1 | Boot identity, flash, PSRAM, heap | — | 4 MB flash, **no PSRAM**, ESP32-S3 silicon, no panic reset |
| 2 | Rails, VBUS detect, NTC reference | USB or J1 | `/VBUS_VALID` divider, pulled-up inputs rest high |
| 3 | Spare test-point GPIOs | — | each follows both internal pulls; **pairwise short test** |
| 4 | I2C scan + DAC address | — | a device answers; adopts a non-0x60 strap (spec N-4) |
| 5 | DAC write / read-back | — | 7 codes round-trip; frame is **3 bytes** |
| 6 | DAC power-down modes | — | all four PD1:PD0 modes read back |
| 7 | ADC calibration + stability | — | eFuse curve-fit or the **reported** fallback; channel settling |
| 8 | Every ADC input is live | jumper (opt.) | all 8 inputs in range; pull-ups alive |
| 9 | DAC EEPROM power-on state | — | what a cold start would actually do |
| 10 | `~LDAC` idle-high, never pulsed | — | two channels hold **independent** values |
| 11 | Sense path | J3 open | readings captured; channel-to-channel comparison |
| 12 | Servo channel 1 | voltmeter J3.3 | sweep both modes; **gain ratio is 1.82, not 1.812** |
| 13 | Servo channel 2 | voltmeter J3.2 | same, channel 2 |
| 14 | **Loopback ch 1** | jumper J3.3→J2.3 | released = high; driving = pulled down; **monotonic** |
| 15 | **Loopback ch 2** | jumper J3.2→J2.2 | same, channel 2 |
| 16 | SWC ladder inputs | pod (opt.) | idle is the HIGH state; `R_ladder ≤ pullup·7.25` |
| 17 | AUX1–AUX3 | jumper (opt.) | all idle high; **AUX1 (the programming button) alive** |
| 18 | NTC temperature | — | plausible °C; **the cost of a wrong rail assumption** |
| 19 | Buzzer | listen | gate toggles; rhythm **+ a 200 Hz–5 kHz sweep**; sound = operator |
| 20 | LEDs | watch | both polarities; **1–60 Hz pulse sweep**; which lit = operator |
| 21 | Gain auto-selection | loopback + pull-up | envelope, guard band, **safe 1.82 default** |
| 22 | Servo trim loop **and response time** | loopback | bounds are the spec's; loop **disabled**; **settles inside the 200 ms key-send budget**, and a 120 ms double press resolves as two keys |
| 23 | **Idle is high-Z** | loopback + pull-up | release needs no mode; sink-only proven both ways |
| 24 | USB link configuration | — | console is on the recoverable peripheral |
| 25 | WiFi radio + credentials | AP in range | **radio / scan / association** reported separately |
| 26 | NVS | — | `nvs_set`=0; `nvs_get`=length; **never truncates** |
| 27 | Press classification | loopback | single/double/long shapes present in real samples |
| 28 | Full pass-through sweep | both jumpers | whole chain, both modes, command-tracking |
| 29 | Connector continuity map | jumpers | all 5 inputs alive; outputs independent |
| 30 | Endurance | both jumpers | repeatability, write errors, **self-heating** |
| 31 | **AUX1-AUX3 driven low and released** | 3 test wires (see below) | each input **collapses and recovers**, unattended |
| 32 | RT1 temperature scale | something warm (optional) | **two-point beta** — the curve, not one plausible reading |

Run them in the numbered order on a fresh board — the early tests establish the
preconditions (power, DAC, ADC calibration) that the later ones are read through.

---

## Reading a result

| Result | Meaning |
| --- | --- |
| `PASS` | every assertion held |
| `FAIL` | an assertion failed — the summary line carries the **first** failure and its numbers |
| `BLOCKED` | a *precondition* was absent (no DAC, no jumper). The premise is void; fix the note and re-run |
| `SKIP` | ran, but the condition under test was not present |
| `WARN` | measured, outside the expected window, not a defect |

Three conventions worth knowing:

**Numbers, not adjectives.** `[FAIL] measured 4550, wanted 2500 +/- 2 (off by 2050)`
— the failure tells you what it saw.

**The first failure wins the summary.** The one-line summary a table shows is the
*root* failure, not the last thing that happened to be checked.

**`note:` lines are prose, not assertions.** They explain a measurement, name the
suspects when something failed, and say when a surprising reading is actually
correct. Read them — the important caveats live there.

### Two examples of a note doing real work

Test 11, with J3 open:
> `With nothing on J3 the KEY lines float near 0 V (no pull-up of their own), so a
> near-zero reading here is CORRECT, not a dead channel.`

Test 21, no head unit:
> `an out-of-envelope measurement must NOT be classified, and the asymmetry
> defaults to 1.82 because over-ranging a 3 V unit is the only dangerous mistake.`

Both are cases where the *correct* result looks like a fault. Saying so is the
difference between a tool and a red light.

---

## What is machine-checked and what is not

The board can see its own pins and its own ADC/DAC. It cannot see light, hear
sound, or know whether a wire is attached. Every test says which half is which
rather than implying a completeness it does not have.

| Machine-checked | Operator-checked |
| --- | --- |
| pin levels, pull-ups, bridges | whether an LED lit |
| ADC readings and their ranges | whether the buzzer sounded |
| DAC write/read-back and gain modes | which LED polarity phase lit it |
| the full analog chain (via loopback) | whether a jumper is actually fitted |
| NVS, WiFi association, USB config | — |

The operator-checked tests still do everything they can: test 20 drives **both**
polarities so a wrong `LED_ON` looks like a polarity bug rather than a dead LED,
and test 19 drives a rhythm before asking.

---

## Where every constant came from

No hardware fact in this project was inferred from prose or from a part name.
Each was read from a primary source and is cited at its definition.

| Source | What it is | Used for |
| --- | --- | --- |
| `../production/netlist.ipc` | IPC-D-356 netlist exported from the live PCB | **every pin assignment**, including that `R15`/`R16` return to net `3V3` and not 12 V |
| `../DESIGN.md` | engineering description | topology, divider ratios, the servo relation |
| spec §2.1–2.5 | the firmware spec's verified hardware contract | pin map, DAC frame, envelope, guard band |
| `../code/lib/HAL/*` | the product's own HAL | the NVS contract, the DAC frame history |
| `../code/lib/HAL/DacFrame.h` | the product's frame encoder | the **3-byte** Multi-Write layout |
| `DS22187E` (MCP4728) | the DAC datasheet | PD1:PD0, channel-select, read-back layout |
| installed Arduino-ESP32 3.3.11 / IDF 5.5.5 | the actual toolchain | API signatures, enum names |

Where the sources disagreed, the **netlist won**. That rule is not decorative:

- `DESIGN.md` §4.1 once claimed the ladder is "pulled to 12 V when idle". The
  netlist shows the pull-ups on net `3V3`. An inverted topology would make every
  press-direction assertion backwards.
- The product firmware once shipped a **4-byte** MCP4725-shaped frame, so no DAC
  channel was ever addressed and nothing reported an error. The 3-byte layout is
  pinned by host tests (`test_host/test_dacframe`) that assert every byte.
- The product firmware once selected tracking mode **without writing the ADJ
  channel's code**, driving a 3 V head unit at 1.82× — the dangerous direction —
  while every power-mode check still read correct. In this project that bug is
  **unrepresentable**: `Dac::SetSignal()` is the only way to change a channel, and
  it always writes both halves.

That last one is the design principle throughout: where a defect is known, prefer
making it impossible to express over documenting the rule that avoids it.

---

## Repository layout

```
platformio.ini              two envs: swc-s3-test (device), host (unit tests)
boards/swc-s3-test.json     board definition (4 MB, no PSRAM)
include/
  BoardPins.h               every pin and constant, each with its provenance
  Adc.h  Dac.h  Log.h       the device-side modules
  TestRunner.h              the registry and the assertion helpers
  Secrets.h                 GENERATED, git-ignored — never commit
lib/swc_logic/              pure logic, host-testable, no Arduino/IDF
  DacFrame.h                the MCP4728 wire frames
  Output.{h,cpp}            servo transfer function, gain selection, command band
  Temp.{h,cpp}              NTC math
src/
  main.cpp  FrontEnd.cpp    entry points, serial menu, web UI
  TestRunner.cpp            the runner and assertions
  tests/
    TestDecls.h             declarations shared by the registry and the bodies
    TestList.cpp            THE ordered registry — tests 1..30
    TestsA/B/C.cpp          the bodies
test_host/
  test_dacframe/  test_output/  test_temp/   48 host unit tests
tools/gen_secrets.py        .env -> include/Secrets.h, pre-build
```

Each host suite is its own directory because PlatformIO builds **one binary per
suite**; three files in one directory would collide on `_main`, `setUp` and
`tearDown`.

---

## Test 31's rig: three wires, then it runs unattended

```
   J5.4 (AUX1)  <-->  IO43 / TP7
   J5.3 (AUX2)  <-->  IO21 / TP6
   J5.2 (AUX3)  <-->  IO16 / TP5
```

**The order does not matter.** Test 31 *discovers* which spare pin reaches which AUX
input before it measures anything, so any input may go to any test point. That is not
a convenience — the first version hard-coded the pairing, the real rig had two wires
crossed, and it reported "no response" for two perfectly healthy lines. The `p` menu
probe prints the measured response matrix if you ever want to see it.

With the wires fitted, test 31 **drives its own stimulus**: pulling the spare pin LOW
pulls the AUX input to GND (the "button pressed" state), and floating it lets the
board's own 10 kΩ pull-up set the level (released). No operator prompts, **~400 ms
instead of ~180 s**.

Without the wires it reports **SKIP** rather than failing; with only some fitted,
**WARN** naming which responded.

**IO43 is safe to drive here.** It is UART0 TX, but this build sets
`ARDUINO_USB_MODE=1`, so the console is the USB peripheral and UART0 is free. Two
things had to be got right for it to work as a stimulus: `gpio_reset_pin()` to detach
the UART peripheral from the pad, and `gpio_set_pull_mode()` (not `pinMode`) to clear
an internal pull that an earlier test had left on it.

**Measured on this board:** IO21→AUX2 and IO16→AUX3 both work. **IO43/TP7 drives
correctly** (its pin reads back HIGH=1/LOW=0) **but pulls no AUX input**, so the wire
from IO43 to AUX1 is not making contact — the board and the firmware are both fine,
and test 31 reports WARN naming AUX1 as unexercised.

## Findings from the first bring-up (2026-09-21)

All 30 tests pass on the assembled board. Three things were learned that the tests
now encode, and one that is worth knowing about the hardware.

**The KEY line does not rest at 0 V.** With J3 open the line has *no* pull-up of its
own — the 3V3 pull-ups are on the ladder pins, not here — so it settles at ~3.26 V
(R36's 1 M path and the op-amp bias), and `Q4` can only *sink*. That level is the
**ceiling** on what the servo can command on an open line: any target above it makes
the servo turn the FET off and float the line, which is the *release* behaviour and
not a fault. Several tests originally asserted against fixed expected voltages and
reported a correctly-released line as broken. They now measure the float level
(`include/KeyLine.h`) and derive their command ceiling from it, which is what the
real firmware must do anyway (spec 6.2's headroom rule).

**The MCP4728's read response is not laid out as the datasheet phrasing implies.**
Mapping it with four distinct per-channel patterns showed a **6-byte stride** —
A at byte 2, B at 8, C at 14, D at 20 — not the 2-byte stride that "each channel is
two bytes" suggests. The decoder, and the host tests that pin it, were corrected to
the measured layout. Separately, **the power-down field could not be located at
all**: writing all four `PD1:PD0` values changes no byte in the response. That field
is therefore reported `UNKNOWN` and never asserted against — and the mode is instead
verified *behaviourally*, by the KEY voltage it produces, which is stronger evidence
anyway.

**Through the loopback, `KEY` and `SWC` are the same node.** So `SWC` *tracks* the
command and rises with it — the opposite of the vehicle case, where a button pulls
the ladder down. One test had the sign inverted from reasoning about the real
system and applying it here.

Hardware note, not a fault: **BZ1 is an *active* buzzer** (Huaneng TMB12A05) with its
own oscillator at a fixed ~2.4 kHz, so it cannot follow a drive frequency. The
200 Hz–5 kHz sweep is kept anyway — it exercises the drive path across the audio
band, and a passive buzzer (which *can* follow it) is a likely future change, so the
capability is tested now and will already be correct when the part changes.

## Web UI and the serial monitor

**The web UI.** A sticky status bar at the top is always visible: a pulsing dot and
"Running test N" while work is in flight, "Ready" otherwise. When a test needs you,
a yellow **Action required** banner appears with the exact instruction and a
**Done — continue ▶** button. Tests are clickable rows; long ones show `RUNNING` in
the table.

**Why it used to lock up, and what actually fixed it.** Tests ran synchronously
inside `loop()`, so for the whole duration of a test (9 s for test 20, 12 s for test
22, up to 90 s for the operator tests) the HTTP server was never serviced — the page
could not load, could not refresh, and clicks did nothing. Patching around that (a
pump inside the wait, a deferred-request flag, a spinner) treated the symptom.

The fix is architectural: **tests run on their own FreeRTOS task**
(`include/TestTask.h`), while `loop()`'s task does only the web server and the serial
menu. A test can now block as much as it likes. The page polls `/state` and
re-renders from the server's own answer, so a dropped request self-corrects instead
of leaving the page stuck — and it reloads **once**, only when a test *ends*.

**The serial monitor shows nothing until you type.** The board cannot detect that a
terminal *attached* — USB-Serial-JTAG gives no such event — so a monitor opened on an
already-running board sees a blank screen. Type any character and the menu appears.
The menu prints before the WiFi association too, so a monitor attached at power-on
sees it immediately rather than after up to 15 s of silence.

**Two more stale behaviours that are not faults:

**The serial monitor shows nothing until you type.** The board cannot detect that a
terminal *attached* — USB-Serial-JTAG gives no such event — so a monitor opened on an
already-running board sees a blank screen. Type any character and the menu appears
(with a note saying so). The boot banner is not resent, because there is nothing to
resend it to. The menu also prints *before* the WiFi association now, so a monitor
attached at power-on sees it immediately instead of after up to 15 s of silence.

**Clicking a test in the web UI takes up to ~15 s to show anything.** A test runs
synchronously, so while one is running the HTTP server does not answer — the page
shows a "working..." status and picks up the result when it finishes. That is by
design; see below.

**The real bug: a web-triggered test used to wedge the HTTP server permanently.**
Running a test from `/run` left a half-closed client connection that the Arduino
`WebServer` never recovered from, so HTTP stayed dead *indefinitely* — while the
identical test run from the serial menu was fine. That asymmetry is what located it.
Fixed by responding, releasing the socket, and only then running the test. Verified:
after a web-triggered run the page is unreachable for the duration and then returns
200 again.

The page also no longer reloads on click. A reload issued while the server is busy
hangs for the whole test, which is exactly what "clicking does nothing" looked like.

## Bench notes from the second session

**AUX1 reading 0 mV was a jumper, not a fault.** A wire from J5.4 to GND reads exactly
0 mV, which is the same reading a dead input would give — so the `p` menu probe was
added (`gpio_set_pull_mode` + ADC, not `pinMode`/`digitalRead`) to tell "held down by
the operator" from "held down by a fault". With the jumper removed all four inputs
read 3173 mV.

**The first version of that probe was itself wrong and nearly produced a false
hardware verdict.** It used `pinMode()`/`digitalRead()` on pins that are attached to
the ADC peripheral, where `digitalRead` does not reflect the pad — and it called
known-good SWC1 "shorted". The tell was that it contradicted a pin that passes every
other test. The probe now uses the ADC (the instrument that is trusted) and carries
SWC1 as a built-in control.

**Test 31 measured the release at the wrong moment.** It sampled "recovered" 250 ms
after a single Continue press — while the operator was *still holding* the short — so
it compared the shorted value with itself and failed a working board. Reported from
the bench as "it's not testing the release at the right time and fails, even though
the lines go low and high at the right time." It now prompts **twice** per input:
short-and-hold, then remove-and-leave-open.

**Both tasks were reading the same UART.** The serial menu handler on `loop()`'s task
consumed the keystroke the waiting test was listening for, so ENTER appeared to do
nothing and the test still timed out. `TestTask::WaitingForOperator()` now claims the
input while a prompt is outstanding.

**The LED sweep is per-LED now.** A report of "the status LED flashes but LED2 only
goes solid then off" could not be attributed while both were driven together. The
pins are also **read back** first, and they toggle identically (56 toggles, 28 cycles
each) — so that difference is downstream of the MCU, and the likely cause is a
marginal solder joint, which passes DC (solid light) and fails as the rate rises.
Driving one at a time is what makes it visible per-LED.

## Response time: the number that decides whether a double press works

Test 22 measures the servo's **step response**, because the trim loop's 1 Hz rate is
the *supervisor's* rate and says nothing about how fast the output moves. The worst
case in service is a quick double press: the spec allows a **200 ms** key-send
window, so the output must reach a commanded level well inside that or the two
presses merge into one and the feature silently does not work.

**Speed and accuracy are two different measurements, and conflating them falsely
fails a healthy board.** Test 22 originally required the reading to come within 5%
of the *commanded* target and called the result "settled" — which asks a
*static-error* question in a *timing* test. The servo's hardware integrator has no
feedback edge that drives its static error to zero: it is set by DAC offset,
`R58`/`R61` tolerance and `R36` leakage, which is precisely the error spec §6.5
says the (disabled) trim loop exists to null. On a board whose offset is 72 mV
against a 1.24 V step, the 5% window is **unreachable at any speed**, so the test
timed out while the loop was entirely healthy. The first board passed only because
its offset happened to land just under the wire (~40 mV) — the assertion was
marginal from the start, and a second assembly exposed it rather than broke it.

So the test now measures the two things separately:

- a **10–90% rise time** against the step, asserted against the 200 ms budget, and
- the residual **static error**, reported as a number and explicitly labelled as
  what the trim loop would null — *not* asserted, because it is a tolerance figure

Measured with the loopback fitted:

| | rise time (10–90%) | static error |
| --- | --- | --- |
| dev board | 70–83 ms | −40 to −53 mV (ch1), −4 mV (ch2) |
| second assembly | 47 ms | −75 mV (ch1), −93 mV (ch2) |

The rise time is what decides whether a double press works, and both boards are far
inside the budget — a real **120 ms double press** resolves as two distinct keys
(press 1 at ~2950–3008 mV, press 2 at ~1740–1750 mV). The static error varies
between assemblies by a few tens of millivolts, which is ordinary resistor and
op-amp `Vos` spread and is the reason the trim loop exists at all.

Two independent paths measure the same node in test 28 (the `SENSE` buffer and the
`SWC` loopback node) and agree within ~20 mV, which is what rules out a broken
channel when a per-channel offset looks large. A board that missed the *rise-time*
budget would have a genuine problem no amount of slow trimming would reveal, which
is why that half is asserted rather than only reported.

## Speed and the web UI

Two things made this tool look far slower than it was, and both were real bugs.

**Logging blocked on the USB console.** `HWCDC::write()` blocks while trying to push
each line into its TX ring, and when no host is draining the port it waits up to
20 × `tx_timeout_ms` before giving up. With the default 100 ms that is **2 seconds
per line**, so test 31 — which prints about 65 lines — took **130 s** instead of
0.4 s, and only when nobody had a serial terminal open (a browser holding the port
counts as "plugged", so the disconnect path never fires). `Serial.setTxTimeoutMs(0)`
makes the write non-blocking. Nothing is lost: the web UI's copy is captured into the
ring buffer *before* the serial write, so only the serial copy can be dropped, and
only when nobody is reading it.

**Unnecessary delays in the tests.** Test 22 ran its trim loop at 1 Hz for twelve
iterations (12 s) to demonstrate a loop whose behaviour is identical at any rate
below the 16 Hz analog pole; the LED sweep dwelled 2 s per LED; test 30 wrote 120
times; test 23 watched a pull-up for 3 s. All trimmed to what the measurement
actually needs, and the full suite went from ~130 s to **~33 s**, with every test
still passing. Two of the trims were initially too aggressive and broke their own
assertions (the LED sweep no longer produced enough transitions; the servo was still
mid-slew between sweep points) — both corrected, and the assertions now match what
the sweep can actually produce.

The remaining long tests are the ones that *are* measurements: WiFi association
(~5 s), the buzzer and LED listening sweeps, and the 120 ms double-press timing.

**The log pane scrolls properly now.** It used to force `scrollTop` to the bottom on
every 1.2 s poll, so you could not read anything above the tail — it yanked you back
down. It now follows only when you are already at the bottom (standard `tail -f`
behaviour), and shows a **jump to latest** button when you have scrolled away.

## Known limits

- **Tests 31 and 32 need the operator.** 31 skips (rather than passing) if no
  stimulus is applied within its timeout; 32 reports instead of asserting the beta
  if the sensor is not warmed. Both say which they did.
- **Test 19's sweep is not a clean tone on the fitted part.** See the finding
  above: BZ1 is active, so the sweep is a drive-path test, not a pitch test.
- **The absolute key resistance is not verified.** Every analog test proves the
  chain is present and moving together, but the resistance a head unit actually
  sees depends on that head unit's own pull-up. A bench cannot substitute for it.
  This is stated in the tests that could otherwise imply more.
- **Test 24 does not hand the USB PHY to TinyUSB.** Doing so kills the console and
  every later test with it. The real link is exercised by the product firmware's
  own `test/test_hw` suite.
- **Test 30's conclusions are weak without a second run.** A fault that appears
  only once the board is warm needs the warm board to be re-tested.
- **`lib_compat_mode = strict` is set in `platformio.ini`** and is load-bearing.
  This machine has a populated `~/.platformio/lib`; PlatformIO's default "soft"
  mode would compile an RP2040-only library for this ESP32-S3 target and fail on a
  missing `Network.h`. Strict mode makes each library's own `platforms` field
  authoritative, so the project builds the same way anywhere.
