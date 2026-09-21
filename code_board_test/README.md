# SWC adapter — bring-up and hardware-test firmware

A PlatformIO project that turns a freshly-assembled **SWC steering-wheel-controls
adapter** board into a self-testing bench instrument. It runs one hardware
function at a time, prints what it measured, and says whether that is what the
design predicts.

This is **not the product firmware**. The product lives in
`../swc_module_pcb/code` (ESP-IDF, TinyUSB CDC link to an Android head unit).
Nothing here ships.

| | |
| --- | --- |
| **Board** | 54 × 102 mm, 4-layer, ESP32-S3 (DOIT ESPS3-32-N4), MCP4728 DAC, TLV9004 servos |
| **Framework** | Arduino (deliberate — see below) · ESP-IDF 5.5.5 underneath |
| **Tests** | 30 on-device + 48 host unit tests |
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
pio run -e swc-s3-test -t upload

# 4. Open the menu
pio device monitor -b 115200
```

The board prints a boot banner, brings up the ADC and the DAC, blinks both LEDs
twice, and then presents a menu. Type a **test number** and press ENTER.

WiFi credentials are read from `../swc_module_pcb/code/.env` at build time by
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
| 19 | Buzzer | listen | gate toggles; rhythm driven; sound = operator |
| 20 | LEDs | watch | both polarities driven; which one lit = operator |
| 21 | Gain auto-selection | loopback + pull-up | envelope, guard band, **safe 1.82 default** |
| 22 | Servo trim loop | loopback | bounds are the spec's; loop **disabled** by default |
| 23 | **Idle is high-Z** | loopback + pull-up | release needs no mode; sink-only proven both ways |
| 24 | USB link configuration | — | console is on the recoverable peripheral |
| 25 | WiFi radio + credentials | AP in range | **radio / scan / association** reported separately |
| 26 | NVS | — | `nvs_set`=0; `nvs_get`=length; **never truncates** |
| 27 | Press classification | loopback | single/double/long shapes present in real samples |
| 28 | Full pass-through sweep | both jumpers | whole chain, both modes, command-tracking |
| 29 | Connector continuity map | jumpers | all 5 inputs alive; outputs independent |
| 30 | Endurance | both jumpers | repeatability, write errors, **self-heating** |

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
| `../swc_module_pcb/production/netlist.ipc` | IPC-D-356 netlist exported from the live PCB | **every pin assignment**, including that `R15`/`R16` return to net `3V3` and not 12 V |
| `../swc_module_pcb/DESIGN.md` | engineering description | topology, divider ratios, the servo relation |
| spec §2.1–2.5 | the firmware spec's verified hardware contract | pin map, DAC frame, envelope, guard band |
| `../swc_module_pcb/code/lib/HAL/*` | the product's own HAL | the NVS contract, the DAC frame history |
| `../swc_module_pcb/code/lib/HAL/DacFrame.h` | the product's frame encoder | the **3-byte** Multi-Write layout |
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

## Known limits

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
