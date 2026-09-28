# SWC Adapter — User Manual

This is the operating manual for the **SWC Steering Wheel Controls Adapter**: what
it does, how to install it, how to teach it your wheel's buttons, how to give
those buttons the actions you want, and how to keep its firmware up to date. It
is written for the person using the adapter, not for the person building it.

> **Coming from the old 2022 Pico board?** The gesture-programming flow you liked
> **works here too**: hold **AUX1**, do the gesture on a wheel button, and the
> adapter **holds a distinct voltage** on the KEY line for you to teach your head
> unit — release AUX1 to release the line. See [§8.2](#82-programming-a-gesture-no-app-needed).
> The rest of chapter 5 explains what the adapter does with that.

For the engineering description see [DESIGN.md](DESIGN.md); for how the firmware
and app are structured, [AGENTS.md](AGENTS.md) and the spec under
`docs/superpowers/specs/`; for ordering, [MANUFACTURING.md](MANUFACTURING.md).

## Quickstart

The shortest path from a boxed adapter to a working steering wheel.

**What you need:** the adapter, a USB-C cable (to a phone **or** any USB power
source), and the factory wheel + head-unit wiring.

1. **Wire it up.** Factory wheel ladder → `SWC_IN` (`J2`). Adapter → head unit
   `SWC_OUT` (`J3`). Ground both. Optional: 12 V to `J1`. ([§3](#3-the-board-and-what-plugs-in-where), [§4](#4-installing-it-in-the-car))
2. **Power it on.** One short beep + slow-breathing STATUS LED = healthy. No phone
   needed. ([§6](#6-first-power-on))
3. **Teach it where your buttons are — learn each button.** Hold **AUX1** ~1.5 s
   (LEDs alternate), press **one** wheel button, release AUX1. Repeat for every
   button. Beep = `LEARN_OK`. ([§8](#8-learning-and-programming-no-app-needed))
4. **Give a button's gestures their functions — program the gestures.** Hold
   **AUX1**, do the gesture (**tap** = single, **two taps** = double, **hold** =
   long) on the button, and while still holding AUX1 teach your head unit that
   voltage (**"set key function"** on the head unit). Release AUX1. Repeat for the
   other gestures. ([§8.2](#82-programming-a-gesture-no-app-needed))
5. **That's it — no app required.** Your wheel now has single/double/long
   functions on every button. ([§5](#5-how-it-behaves--the-mental-model))

**Optional — install the Android app** for extras the head unit can't do on its
own: give a gesture a *different* function, launch a phone app, or send an
Android intent. ([§9](#9-binding-gestures-to-actions-and-using-the-app),
[§10](#10-the-android-app))


## Contents

1. [What the adapter does](#1-what-the-adapter-does)
2. [What you need](#2-what-you-need)
3. [The board, and what plugs in where](#3-the-board-and-what-plugs-in-where)
4. [Installing it in the car](#4-installing-it-in-the-car)
5. [How it behaves — the mental model](#5-how-it-behaves--the-mental-model)
6. [First power-on](#6-first-power-on)
7. [Feedback reference — the LEDs and the buzzer](#7-feedback-reference--the-leds-and-the-buzzer)
8. [Learning and programming (no app needed)](#8-learning-and-programming-no-app-needed)
9. [Binding gestures to actions (and using the app)](#9-binding-gestures-to-actions-and-using-the-app)
10. [The Android app](#10-the-android-app)
11. [Updating the firmware](#11-updating-the-firmware)
12. [Maintenance mode — WiFi and Bluetooth setup](#12-maintenance-mode--wifi-and-bluetooth-setup)
13. [Troubleshooting](#13-troubleshooting)
14. [Settings reference](#14-settings-reference)

---

## 1. What the adapter does

A steering-wheel button is not a digital switch. It is a resistor dropped across
a single analog **KEY** line; the head unit pulls that line up and works out which
button was pressed from the voltage it reads. There are usually two such lines —
this board calls them **SWC1** and **SWC2** — with the buttons split between them.

That arrangement is cheap and reliable, and also completely fixed: the resistor
values, and therefore the head unit's behaviour, live in the factory button pod.
A given aftermarket head unit wants a specific resistance it was taught, and it
has no concept of a "double press" or a "long press" — it only reads a voltage and
recognises key windows.

The adapter sits between the two. It:

- **reads** the factory ladder on the SWC1/SWC2 inputs,
- **interprets** single, double and long presses,
- **drives** the head unit's KEY line with a closed-loop servo, presenting the
  voltage the head unit expects for the action you chose.

Because the mapping is software, buttons can be **re-mapped, combined, or given
press behaviours the factory wiring never had** (a long-press on *next* that opens
an app, say). And because a press can also be reported to an **Android phone over
USB**, the adapter can trigger actions the head unit's own steering-wheel input
cannot express — launching an app, sending an intent with a data payload.

Two identical channels, **SWC1** and **SWC2**.

**The adapter works with no phone attached.** The phone app is an enhancement, not
a requirement: an unconfigured adapter passes presses straight through to the head
unit so the wheel behaves like stock, and the buttons can be taught entirely on
the device itself (chapter 8).

## 2. What you need

- The SWC adapter board (in its enclosure).
- A **USB-C cable** to a USB power source or an Android phone — the board runs
  from USB alone, or from a 12 V feed.
- **Vehicle wiring**: the factory steering-wheel ladder to the adapter's input
  terminal, and the adapter's output terminal to the head unit's SWC/KEY input.
  Both need a ground.
- Optional: **12 V** from the vehicle (switched or permanent) so the adapter is
  powered without USB.
- Optional: an **Android phone** (Android 8.0 / API 26 or later) with a USB-C
  **OTG-capable** port, to use the app.
- Optional: **wires to the AUX inputs** if you want extra buttons wired into the
  adapter (chapters 3 and 8).

## 3. The board, and what plugs in where

Four screw terminals on the board's left edge carry all the wiring, and USB-C sits
on the top-left edge. Each terminal has its own ground pin, and the pin function is
silkscreened on the back of the board — the enclosure has windows over those labels
so you can read them without opening the case.

| Terminal | Function | Pins (left to right) |
| --- | --- | --- |
| **J1** | 12 V power in (optional) | GND, +12 V |
| **J2** | Steering-wheel **inputs** | GND, SWC2, SWC1 |
| **J3** | **Output** to the head unit | GND, KEY2, KEY1 |
| **J5** | Auxiliary inputs | GND, AUX3, AUX2, AUX1 |
| **J4** | USB-C | — |

The two channels share one input terminal and one output terminal. **`SWC1`/`KEY1`
is channel 1; `SWC2`/`KEY2` is channel 2.** Match each channel's input to its own
output — a ladder on `SWC1` is served on `KEY1`.

> The pin order on each terminal matches the silkscreen under the corresponding
> floor window. Read the window rather than this table if they ever differ.

### 3.1 Power

Both a **12 V feed** and **USB power** can run the board, and the 12 V feed is
**optional** — the board works from USB alone. The two sources are diode-OR'd, so
either alone is sufficient and a fault on one cannot back-feed the other. Each has
its own self-resetting fuse (a PPTC).

Use 12 V when you want the adapter powered in the car without a cable run to a USB
port. Use USB when you are setting it up on the bench or updating firmware.

### 3.2 The two LEDs

Both LEDs are green, and they mean different things — they are distinguished by
**blink pattern and position**, not colour.

| LED | Where | Role |
| --- | --- | --- |
| **STATUS** (`D6`) | — | The adapter's overall **state** — "is this thing OK?" |
| **LED2** (`D12`) | — | **Activity** — what it is doing right now |

See chapter 7 for the full pattern tables.

### 3.3 The buzzer

A single fixed-tone buzzer carries the whole no-app experience. It can only be
switched on and off — the tone is fixed by the part — so every message is a
**rhythm**: a number of beeps in a pattern. The patterns are all in chapter 7.

### 3.4 The buttons

Two small buttons on the board:

- **RESET** — restarts the adapter. It comes back up in its safe idle state and
  resumes normal operation; nothing is lost.
- **BOOT** — for firmware recovery only. It is recessed behind a hole in the lid
  and is normally never needed (chapter 11 explains the software update routes).
  **Do not hold BOOT while powering on** — that puts the chip into its raw
  serial-download mode rather than starting the adapter.

### 3.5 Test points

Eight bare pads (`TP1`–`TP8`) exist for bench diagnostics. `TP7`/`TP8` carry the
**serial console** (`115200 8N1`) — clip a USB-UART adapter between them and a
ground pad if you ever need to read the adapter's log. You will not need these in
normal use.

## 4. Installing it in the car

1. **Identify the factory ladder.** The steering-wheel buttons are a resistor
   chain whose common is tied to ground; pressing a button pulls the input *down*
   toward ground. The buttons are split across the vehicle's two SWC lines, if it
   has two.
2. **Wire the inputs.** Factory SWC line 1 → `J2` `SWC1`; SWC line 2 → `J2`
   `SWC2`; ladder common/ground → `J2` `GND`. If the vehicle has only one SWC
   line, use `SWC1` and leave `SWC2` unconnected — the two channels are
   independent, so one unused input does not affect the other.
3. **Wire the outputs.** `J3` `KEY1` → head unit's SWC/KEY input 1; `J3` `KEY2` →
   head unit SWC/KEY input 2; `J3` `GND` → head unit ground.
4. **Wire 12 V (optional).** `J1` `+12 V` and `J1` `GND`. If you skip this, power
   the adapter over USB instead.
5. **Power it up and watch the STATUS LED.** A healthy boot gives one short beep
   and the STATUS LED breathing slowly (running, no phone connected). See chapter
   6.

> The wheel's ladder must be connected **at power-on**. The adapter measures the
> ladder's idle level when it boots and treats that as the reference; a ladder
> attached later may not be adopted until the next restart.

## 5. How it behaves — the mental model

### 5.1 The ladder and the KEY line

The adapter reads the ladder on the **input** side and *servos* the head unit's
KEY line on the **output** side. The output tracks what the board actually senses,
so the mapping holds even if the supply voltage drifts.

### 5.2 Gestures

The adapter recognises three gestures on any learned button:

| Gesture | Meaning |
| --- | --- |
| **Single** | one tap |
| **Double** | two taps inside the double-press window |
| **Long** | held past the long-press threshold |

A **double press is the same button pressed twice** in quick succession. Two
*different* buttons pressed in quick succession are two separate presses, not a
double.

### 5.3 What the head unit sees for each gesture

**Your head unit is gesture-blind.** Its SWC input reads a *voltage* and matches it
to key windows; it has no idea what a "double press" is. So the only way the head
unit can act differently for single, double and long is if the adapter presents a
**different voltage** for each.

That is what the adapter does. For each learned button it sends three distinct,
predefined voltages — one for single, one for double, one for long:

| Your gesture | What the adapter sends the head unit |
| --- | --- |
| **Single** (one tap) | the button's **single** voltage |
| **Double** (two taps) | the button's **double** voltage |
| **Long** (held) | the button's **long** voltage |

Because those three voltages are **fixed and always the same** for that button —
regardless of supply voltage or temperature — you can teach your head unit once
(§8.2) and it will keep working. This is the "compensation" that keeps the same
gesture always sending the same voltage.

The three voltages come from a **fixed ascending table** laid out **from the low end
of the output band upward**, and each is a fixed fraction of that band — so they
stay correct on either a 5 V or a 3 V unit. The steps are deliberately fine: that
way a head unit which resolves only a few distinct key windows simply will not tell
two adjacent gestures apart, which you will see as "that radio will not take three
keys on one button" rather than as a firmware fault. If a radio cannot tell two of
your gestures apart, that is the radio's resolution, not the adapter's; see
chapter 13 (troubleshooting) for that case.

**You do not have to program a gesture to use the button.** A button you have only
learned still sends its single voltage for a tap, so it works as a stock button. The
programming step (§8.2) is how you give the *extra* gestures their functions.

### 5.4 What happens on a press

1. Presses are filtered and classified into a gesture.
2. If the gesture is **bound** to something in the app, that runs. A hardware action
   (present a key voltage to the head unit) runs **first and always** — it does not
   depend on a phone being connected.
3. If the gesture is **unbound**, the adapter sends that gesture's **fixed voltage**
   (as in §5.3) — so a head unit you programmed will fire the function you taught
   it. A press matching **no learned button** is reported as "unknown" and beeps
   `KEY_UNKNOWN`; the adapter does not invent a key.
4. The press is reported to the app over USB, if one is connected, so app-side
   actions (launch an app, send an intent) can fire.

**Normal presses are silent by default.** The adapter does not beep on each press;
you can turn a per-press "click" on in the app if you want one (§14).

### 5.5 Output range — matching your head unit

The adapter **auto-detects** the head unit's key voltage at startup and picks the
output range to match:

- a **5 V** head unit is driven at gain 1.82;
- a **3 V** head unit is driven at gain 1.00 (the output tracks the signal).

The safety bias is toward the 5 V range when the measurement is absent or
ambiguous, because over-ranging a 3 V unit is the only dangerous mistake. You do
not normally need to touch this — see `gain_policy` in chapter 14 if you do.

The three gesture voltages (§5.3) are chosen as fractions of this range, so they
stay correct on either a 5 V or a 3 V unit.

### 5.6 Two ways to give a gesture a function — **read this if you used the old board**

There are **two independent ways** to give a button's gesture a function, and they
work together:

| Way | What it does | Where | Needed? |
| --- | --- | --- | --- |
| **Program the gesture** | Holds a **fixed voltage** on the KEY line so your head unit's own "set key function" learns it | **On the device**, hold AUX1 (chapter 8) | Basic use — no app |
| **Bind the gesture** | Tells the adapter to run a specific **action** (a chosen `key_mv`, launch an app, send an intent) | **In the Android app** (chapter 9) | Optional |

**This is the 2022 Pico flow, and it works here.** Hold AUX1, do the gesture on a
wheel button, teach your head unit that voltage, release AUX1. The adapter holds a
distinct voltage per gesture — single, double and long each get their own — so a
head unit you have programmed once gives you all three functions per button with
**no app at all**.

A **binding**, if you set one in the app, **overrides** the default gesture voltage
for that one gesture: the adapter then sends whatever your binding says (or runs an
app action) instead. Remove the binding and the gesture falls back to its default
programmed voltage. So the app *expands* the basic behaviour; it does not replace
it.

### 5.7 How the head unit's "function" gets chosen

The adapter **does not store a named head-unit function** ("Volume Up", "Source",
…). It sends a **voltage** on the KEY line. Which function that voltage means is
the *head unit's* interpretation, and it is different in every car — the same
"volume up" is a different voltage on a different radio — so the firmware cannot
name it and does not try.

That is why you "program" a gesture by teaching the **head unit** the voltage,
rather than picking a function from a list. It is also why an `OUT_VOLTAGE` binding
(chapter 9) asks you for a **`key_mv`** — a number of millivolts — rather than a
function from a list. There is **no built-in table of head-unit functions to pick
from**.

See chapter 9.3 for how to find those numbers, and chapter 8.2 for the no-app way.

## 6. First power-on

On a healthy boot the adapter:

1. puts the KEY output into its **safe idle state** (the line released) *before*
   it brings up anything else,
2. loads its saved configuration (falling back to defaults if it is damaged),
3. measures the ladder and the head unit,
4. then starts serving presses.

You will hear **one short beep** and see the STATUS LED **breathing slowly**. If
instead you hear a repeated pattern, see chapter 7 for what it means.

### Before it has been configured

A brand-new adapter has no learned buttons and no bindings. It runs in
**pass-through**: it mirrors what it senses on the wheel onto the head unit. Your
steering wheel works like stock, and the adapter is invisible. Everything else in
this manual is about *upgrading* that behaviour.

### First-ever power-on with no saved configuration

The adapter offers its setup window on the very first power-on with no
configuration (chapter 12), which is how you get it onto WiFi without any other
trigger. The STATUS LED double-flashes while the window is open.

## 7. Feedback reference — the LEDs and the buzzer

The LEDs are **continuous state** you can read at a glance; the buzzer is
**transient events**. Between them they carry everything the adapter needs to tell
you without a phone.

### 7.1 STATUS LED (`D6`)

| Pattern | Meaning |
| --- | --- |
| Off | No power / not running |
| **Solid** | Running, output safe, USB connected, configuration valid — all good |
| **Slow breathe (1 Hz)** | Running normally, **no phone connected** |
| **Fast blink (5 Hz)** | **Fault** — read the buzzer pattern for which one |
| Double-flash burst | **Maintenance mode** active (WiFi/Bluetooth setup) |
| Alternating with LED2 | **Learn mode** active, waiting for a press |

A **fault latches** the fast blink and outranks everything else: a device that is
faulted is not OK, whatever the link is doing. A hardware fault clears only on a
restart, because a wiring fault or a collapsed supply does not fix itself. The one
exception is a **configuration fault**, which clears as soon as a good
configuration is written.

### 7.2 LED2 (`D12`)

| Pattern | Meaning |
| --- | --- |
| Off | Idle, no recent activity |
| Flick | A gesture was recognised |
| **Solid** | A key value is currently being presented (**the line is driven**) |
| Long pulse (0.5 s) | Output range changed, or the head unit was (re)detected |

LED2's "solid while driving" is a genuine diagnostic: it shows the adapter is
holding a key, which tells you whether "the adapter is doing something wrong" or
"the head unit is ignoring it".

### 7.3 Buzzer patterns

The tone is fixed; the **rhythm** is the message. Each pattern is written as
*pulse length / gap length × repeats*.

| Pattern | Rhythm (on/off ms) | Repeats | Meaning |
| --- | --- | --- | --- |
| `BOOT_OK` | 60/60 | 1 | Power-on self-test passed |
| `BOOT_DEGRADED` | 60/60 | 3 | Booted, but with a fault (see `FAULT_*`) |
| `BOOT_ERROR` | 500/200 | 2 | Cannot serve output; needs attention |
| `KEY_ACCEPTED` | 25/0 | 1 | A gesture was recognised (deliberately short and quiet — you hear it constantly) |
| `KEY_UNKNOWN` | 120/80 | 1 | A press was seen but matches no learned button |
| `PROGRAM_ENTER` | 40/40 | 2 | Entering learn mode (or maintenance mode, from a 3 s AUX1 hold) |
| `PROGRAM_STEP` | 40/40 | 1 | *Not played* — see the note below |
| `PROGRAM_SAVED` | 40/20 | 4 | *Not played* — see the note below |
| `PROGRAM_EXIT` | 200/0 | 1 | Learn finished |
| `PROGRAM_CANCEL` | 300/100 | 1 | *Not played* — see the note below |
| `LEARN_PROMPT` | 100/100 | 1 | Waiting for you to press a button |
| `LEARN_OK` | 40/30 | 2 | That button was learned and accepted |
| `LEARN_REJECT` | 300/80 | 2 | The sample was rejected; try again |
| `FAULT_DAC` | 500/300 | 3 | Output/DAC fault |
| `FAULT_CONFIG` | 500/300 | 4 | Configuration corrupt; defaults loaded |
| `FAULT_INPUT` | 500/300 | 3 | Ladder/rail wiring fault — an open input or a short to a supply |
| `FACTORY_RESET` | 800/200 | 3 | Everything erased |
| `OTA_START` | 400/0 | 1 | Firmware update started |
| `OTA_OK` | 150/100 | 2 | Firmware update written successfully |
| `OTA_FAIL` | 80/40 | 3 | Firmware update failed; old version still running |

> The **fatal** patterns (`BOOT_ERROR`, `FAULT_*`, `FACTORY_RESET`) are longer than
> the rest on purpose — they must be unmistakable, and they are rare enough that
> nothing is delayed behind them. They also play even when the buzzer is set to
> `OFF`, because a device that cannot serve output must still be able to say so.
>
> **`PROGRAM_STEP`, `PROGRAM_SAVED` and `PROGRAM_CANCEL` are not played by this
> adapter.** They existed in the 2022 Pico design's gesture-programming flow ("*n*
> beeps announces the *n*-th gesture", escalating to a saved confirmation). This
> adapter uses the learn/programming flow in chapter 8 instead, which signals with
> `PROGRAM_ENTER` on the hold and `LEARN_OK`/`LEARN_REJECT` on the release rather
> than an escalating beep count. They are listed only so this table matches the
> firmware's full vocabulary.

## 8. Learning and programming (no app needed)

There are **two things you do on the device itself**, both driven by the **AUX1**
input, and both work with **no phone**:

| You want to… | Hold AUX1 and… | Records |
| --- | --- | --- |
| **Learn** a button | press the button once, then release AUX1 | **where** the button is (its voltage) |
| **Program** a gesture | do the gesture (tap / two taps / hold), then release AUX1 | **holds a voltage** for your head unit to learn |

You learn each button **once**; you then program its gestures to give them functions
on the head unit. Wire a momentary push-button to `AUX1` on `J5` (and its ground)
if the board does not already have one.

> **AUX1 is the programming button.** It is also the only input that is *never* a
> learn target.

### 8.1 Learning the buttons

Do this first, for every wheel button.

1. **Hold AUX1** for about **1.5 seconds**. You hear `PROGRAM_ENTER` and the STATUS
   LED starts alternating.
2. **While still holding AUX1, press and briefly hold the wheel button you want to
   teach** — on whichever input you are programming (SWC1, SWC2, AUX2 or AUX3).
   **You do not pick a channel or a menu** — the adapter watches all the inputs and
   takes the one that leaves its idle as the one being programmed. You hear
   `LEARN_PROMPT` while it measures.
3. **Release the wheel button, then release AUX1.** The button is stored and the
   adapter beeps `LEARN_OK`. If a measurement is rejected (too noisy, or the button
   barely left idle), you hear `LEARN_REJECT` — try again with a firmer press.

- **One hold = one button, learned once.**
- **Re-learning a button corrects it in place** — press the *same* physical button
  again and the adapter matches it by voltage rather than adding a duplicate.
- **Learning adds buttons; it never wipes the ones you already have.**
- **After learning, the button already works like stock** — a tap sends that button's
  fixed single voltage to the head unit.
- **Its double and long send their own fixed voltages too**, once you have taught the
  head unit what they mean (**programming**, §8.2).

### 8.2 Programming a gesture (no app needed)

This is how you give the *double* and *long* gestures their own functions on the
head unit, and it is the 2022 Pico flow. **Do this after learning the button.** You
need the head unit's own "**set key function**" / "**learn steering key**" screen
open for each step — that is where the function is stored.

1. **Hold AUX1** ~1.5 s (`PROGRAM_ENTER`, STATUS LED alternating).
2. **Do the gesture you want to program** on a button you have already learned:

   | To program | Do this on the button |
   | --- | --- |
   | the **single** function | tap it once |
   | the **double** function | tap it twice |
   | the **long** function | hold it |

   The adapter now **holds that gesture's voltage on the KEY line**, and keeps
   holding it until you release AUX1. Give it about half a second after the gesture
   so it has settled on the right voltage before you capture it (the *double*
   voltage only appears once the second tap has finished).
3. **While the voltage is being held, set the function on your head unit** — use
   its "set key function" screen and assign it to that voltage (usually you press
   "set" on the head unit and it captures whatever level it is reading).
4. **Release AUX1.** The adapter releases the KEY line.

Repeat from step 1 for each gesture. When you later do that gesture normally, the
adapter sends the **same voltage**, so the head unit fires the function you set.

- **Single, double and long get three different, fixed voltages** — that is what
  makes three functions possible on one button. They do not drift with supply
  voltage or temperature.
- **You can leave a gesture unprogrammed.** A button you have only learned still
  sends its single voltage for a tap.
- **A gesture you have *bound* in the app overrides its default voltage** — see
  §5.6 and chapter 9.
- **A press during programming is being *held* for the head unit, not driven as an
  action** — your app bindings do not fire for it.

### 8.3 AUX inputs are switches, not ladders

AUX2 and AUX3 are for extra buttons wired directly to the adapter (a switch to
ground). Learning one records the switch's own window — it is a simple pressed /
not-pressed input. You can then bind gestures on it like any other button.

### 8.4 If AUX1 is held too long

AUX1 is also the way in to **maintenance mode**, on a **longer hold**:

| Hold AUX1 for | What happens |
| --- | --- |
| ~1.5 s | **Learn / programming** |
| ~3 s | **Maintenance mode** (WiFi + Bluetooth setup) |

The two are nested deliberately so over-holding escalates cleanly: if you hold past
3 seconds, the adapter abandons the learn (nothing half-measured is saved) and opens
the maintenance window instead.

## 9. Binding gestures to actions (and using the app)

**The adapter gives you a working wheel with no app** (chapter 8). The Android app
is **optional** and **expands** on that: it lets you point a gesture at a specific
key voltage you choose, launch a phone app, or send an Android intent — things the
head unit's own input cannot do.

> **Bindings are an override, not a requirement.** A gesture with no binding uses
> its default programmed voltage (§5.3), so a device used headlessly keeps working.
> A binding on one gesture only changes that gesture.

A binding is one row of the app's **Bindings** grid: **channel + button + gesture →
action**.

- `SWC1 · Volume Up · Single` → present the key voltage for Volume Up
- `SWC1 · Volume Up · Long`   → launch your music app
- `ANY · Next · Double`       → send an intent with a data payload

**The core action is `OUT_VOLTAGE`** — present the head unit a key voltage it reads
as a key. That is what makes a *single* tap react on the stock head unit even when
no phone is running. `APP_*` actions are the phone's half.

### 9.1 Step by step — give a button a function

Worked example: make a **single tap** on **Volume Up** present the Volume Up key to
the head unit.

1. Connect the phone to the adapter (chapter 10.1).
2. Open the **Bindings** screen.
3. Find the row for **`SWC1 · Volume Up · SINGLE`**. (If Volume Up has not been
   learned yet, it will not appear — go back to chapter 8 and learn it first.)
4. **Tap the row.** The action picker opens.
5. Set **Kind** to **`OUT_VOLTAGE`**.
6. Type the **`key_mv`** value — the millivolt level your head unit reads as
   Volume Up (see 9.3).
7. Tap **Apply**. The row now shows `OUT_VOLTAGE`.
8. Repeat for the other gestures and buttons — e.g. set `SWC1 · Volume Up · LONG`
   to `APP_LAUNCH` with `package` = your music app.
9. Tap **Save to device.** Nothing is stored on the adapter until you save.

To make a gesture **stop** doing something, open the row and tap **Clear binding** —
it then falls back to its default programmed voltage (chapter 5.3).

### 9.2 The binding grid, and where buttons come from

The grid lists a row for every gesture on every button the adapter knows about:

- your **learned wheel buttons**, from chapter 8;
- your **configured AUX switches** (AUX2/AUX3).

Each row is channel-specific, because the same button name usually exists on both
ladders. **`ANY` is a real channel**: a binding on `ANY` matches the button on
*either* SWC channel.

### 9.3 Choosing `key_mv` — how a head-unit function is named

**There is no list of head-unit functions to pick from.** The adapter stores the
**voltage** to put on the KEY line, because "which key" is the head unit's
interpretation of that voltage and differs per head unit. So an `OUT_VOLTAGE`
binding asks for a **`key_mv`** — a number of millivolts.

**`key_mv` is bounded by the output stage.** The adapter can only pull the KEY line
*down*, so:

- the **lowest** level it can present is **1800 mV** (its output floor);
- the **highest** useful level is the head unit's **own idle minus 200 mV** — above
  that the line is simply released, not driven.

So for a typical head unit idling near 3.3 V, usable values are roughly
**1800–3100 mV**; for one idling near 5 V, roughly **1800–5000 mV**. A value outside
the band is clamped (with a warning on the app's Link screen) rather than silently
doing nothing.

**How to find the value for a function:**

1. The most reliable source is the head unit's own wiring sheet or a
   steering-wheel key-resistance table for your vehicle and radio — a specific
   button maps to a specific level.
2. You can narrow it experimentally: the **Ladder screen** shows what the adapter
   currently reads and classifies, so you can see the effect of a level as you
   try it. Bind it, save, press the button, and watch the radio.
3. When you don't yet know, a **safe starting point is a mid-band value** (e.g. a
   few hundred millivolts *below* the head unit's idle) and then adjust.

If a level produces **no reaction on the radio**, it is usually sitting above the
head unit's idle (so nothing was driven) or on a level that radio treats as a
different key. Move it lower within the band and try again.

### 9.4 The action library

| Action | Parameter | Runs on | What it does |
| --- | --- | --- | --- |
| `NONE` | — | — | Explicit no-op (placeholder) |
| `OUT_VOLTAGE` | `key_mv` | **Adapter** | Drive the KEY line to a voltage the head unit reads as a key — the core action |
| `OUT_RELEASE` | — | **Adapter** | Force the KEY line to idle |
| `APP_LAUNCH` | `package` | Phone | Launch an app by package name |
| `APP_INTENT` | `action`, `data` | Phone | Send an arbitrary intent, including a data payload |
| `KEYCODE` | `keycode` | Phone | Inject a key event (media/volume keys work unprivileged) |
| `MEDIA` | `command` | Phone | Media transport: `play`/`pause`/`next`/`prev`/`stop` |
| `VOLUME` | `target` | Phone | Step the volume of a named stream (`media`/`call`/`ring`/`alarm`) |
| `SYSTEM` | `command` | Phone | Housekeeping: `screen_off`/`night_mode`/`screenshot`/`open_settings` |
| `BUZZ` | `pattern` | **Adapter** | Play a named buzzer pattern instead of the default `KEY_ACCEPTED` |
| `APP_RAW` | `command` | Phone | Escape hatch for an app-defined command |

Notes:

- **Two actions per binding** at most, and **32 bindings in total** — these are
  storage limits of the adapter.
- **A hardware action runs even with no phone.** An `OUT_VOLTAGE` binding fires
  whether or not the app is running; the app-side kinds need the phone.
- **`BUZZ` replaces the default confirmation beep** — there is one buzzer, and a
  new pattern replaces rather than queues.
- **An empty cell means the default gesture behaviour.** A gesture with no binding
  presents its own programmed voltage for that gesture (§5.3), so it still reaches
  a head unit you programmed — it just does not do anything app-side.
- **Bindings are matched in order**, and a binding that appears first wins. If you
  bind a button to `ANY` and later want a *channel-specific* difference, make sure
  the specific binding is ahead of the `ANY` one.

## 10. The Android app

The app (`com.oetsolutions.swc`) is an **enhancement**: it configures the adapter,
shows you what it is doing, and can update its firmware. **The adapter works fully
without it.**

### 10.1 Installing the app

The app needs **Android 8.0 (API 26) or later** and a phone with **USB host
(OTG)** support — it talks to the adapter only over USB. Build artifacts are
published from the project's CI as a debug APK (see the repository's Actions page,
artifact `swc-app-debug`); install it with `adb install` or your phone's package
installer.

Connect the phone to the adapter's USB-C port with an OTG cable/adapter. The app
asks for USB permission the first time a device is attached — **allow it**. If no
prompt appears, replug the adapter; the prompt only shows on a fresh attach.

### 10.2 Link screen

The home screen. It shows:

- **Connection state** — Connected / Not connected / Not responding / Version
  mismatch / Failed.
- **Firmware version** the adapter is running.
- **Board temperature** and **free heap** — diagnostics; a non-zero temperature is
  not a problem.
- **Configuration problems** — e.g. the adapter lost its configuration and is
  running defaults, or it restored a backup.
- **Lost frames / adapter link loss** — a flaky USB cable. Re-seat the cable.
- **Device warnings** — e.g. a stored key level the adapter refused to drive as
  written and clamped instead.
- **A button's action did not run** — the *phone's* half of an app-side action
  failed (a permission, a missing app). The adapter still sent the key press.
- **Maintenance mode** — the button to open/close the setup window (chapter 12).

If the link fails, the screen gives you the reason and the fix, not just a red
light.

### 10.3 Ladder screen

A **live diagnostic** for the ladder channel. It shows the rail (idle) voltage,
each learned button's window as a band on a scale, and the current reading as a
marker — **with the band the adapter is currently classifying marked**. This is
what lets you tell an adapter fault from a head-unit fault:

- The adapter classifies the button you pressed, but the stereo does nothing → the
  problem is the head unit.
- The adapter classifies a *different* button than the one you pressed → the
  problem is the adapter (a window needs re-learning).

It also flags a **rail fault** — if the +3V3 supply has sagged, the screen says so
rather than misreading it as a button press.

### 10.4 Bindings screen

The bindings grid, one cell per **channel · button · gesture**. Each cell shows the
bound action, or **the default gesture behaviour** when nothing is bound. Tap a cell
to open the action picker:

- choose the **Kind** from the dropdown,
- fill in the parameter field if the kind needs one (e.g. `package` for
  `APP_LAUNCH`, or `key_mv` for `OUT_VOLTAGE`),
- **Apply** to set it, or **Clear binding** to fall back to the default gesture
  voltage.

**Save to device** writes the whole configuration to the adapter. Save is disabled
while there is a problem to fix (the screen lists them); a *save failure* (a nack,
a timeout) is shown separately and leaves Save enabled so you can retry.

> The grid lists your **learned** buttons and your configured AUX switches. **Learn
> your buttons on the device first** (chapter 8) — they appear here once learned.

### 10.5 Update screen

Shows the running firmware version and offers the update paths in chapter 11.

## 11. Updating the firmware

There are two routes, and the adapter is designed so **a failed update never
bricks it**. New firmware is always written to the *inactive* slot and only
becomes active once the adapter proves it can still drive the output; if it cannot,
the adapter restarts back into the version you were running. **A failed or
interrupted update does not leave you with a dead adapter.**

### 11.1 Over USB, from the app (in the car)

1. Open the **Update** screen.
2. **Check for updates** — the app compares the adapter against the published
   release (over the phone's own connection, which is why this works with no WiFi
   on the adapter).
3. If a newer release exists and it is built for this board, the app offers
   **Download and install** — it downloads the image, verifies it, and pushes it
   to the adapter over USB while showing a progress bar.
4. Or **Push a file over USB** to install an image you already have (a `.bin` from
   anywhere — Downloads, a USB stick).
5. When the push completes, **reboot the adapter** to run the new version.

The adapter **keeps serving button presses during the transfer** — the update
writes the inactive slot while normal operation continues on the active one.

Updates are refused, not forced, when they should not apply:

- a **downgrade** is never offered;
- an image built for **different hardware** is refused;
- a release that requires an intermediate version first (its `min_from_version`)
  is not offered, and the screen says why.

A corrupt image is refused and the running version is left untouched — nothing is
changed.

### 11.2 Over WiFi, from the maintenance page

With the adapter in **maintenance mode** and on a network (chapter 12), open its
setup page and use **firmware update** to either upload a file or **check for a
newer release** and pull it down over the network. This uses the same verification
as the USB path.

### 11.3 What keeps it safe

| Guarantee | How |
| --- | --- |
| Never install an unverifiable image | SHA-256 is checked as the image is written; a mismatch discards it and keeps the old image |
| Never brick on a bad image | Two app slots; a new image must confirm health before it becomes the default — otherwise the adapter rolls back |
| Never lose your configuration | Your configuration lives in separate storage, untouched by any update |
| Never drive a stuck key during an update | The running image keeps the safe idle state throughout |
| Never offer an incompatible jump | The release manifest carries a minimum source version |

## 12. Maintenance mode — WiFi and Bluetooth setup

**The adapter's radio is OFF in normal operation.** WiFi and Bluetooth are started
only inside a **maintenance window**, and fully shut down when the window closes.
This is deliberate: the adapter sits on a car's electrical system and its only job
is to pass button presses. A radio that is idle-but-initialised still costs power
and memory and still has a radio on — so it stays off.

A maintenance window is how you **get the adapter onto WiFi** (for firmware
updates) and **reach its setup page**.

### 12.1 Opening the window

| How | Notes |
| --- | --- |
| **Hold AUX1 for ~3 s** on the device | Works with **no app** — the fallback when you are at the car |
| **"Enter maintenance mode"** in the app's Link screen | The primary way when a phone is connected |
| **A configuration flag** | `maintenance_on_boot` — opens the window on the next boot only |
| **First-ever power-on with no config** | The adapter offers setup the first time it boots with nothing saved |

The window closes on its own after a period of inactivity (default **5 minutes**,
configurable), or immediately on exit. On close the radio shuts down and the adapter
returns to normal. The STATUS LED stops its double-flash.

### 12.2 Setting up WiFi and Bluetooth

The adapter uses **Bluetooth provisioning** (Espressif's scheme), so you can get it
onto a network with the **Espressif provisioning app**:

1. Open a maintenance window.
2. In the app's Link screen, read the **Bluetooth name** and the **BLE passcode**
   (both shown only while the window is open and the radio is up). The board has no
   screen, so the app is where these per-device secrets are shown — over the
   already-trusted USB link.
3. In the Espressif app, find the `SWC-…` device and enter the passcode when asked.
4. Enter your WiFi SSID and password.

### 12.3 The setup page

The adapter also serves a small **setup page** while its radio is up. The app's
Link screen shows the page's URL (with its access token). The page shows device
status and offers **WiFi setup** and **firmware update**. It is **not** a
configuration UI — the app owns configuration; the page exists to get you online
and updated.

The page requires a **token** (carried in the URL the app shows). There is no
default password.

> The window and the radio are separate states. If the adapter is in maintenance
> mode but its radio could not start, the app says so plainly rather than handing
> you a setup page that is not there.

## 13. Troubleshooting

| Symptom | Likely cause and fix |
| --- | --- |
| **Held AUX1, did a single/double/long on a button I had *not* learned, released — nothing happened** | Programming holds a voltage for a button the adapter already knows. If the button has no learned level there is no band to place the gesture's voltage in, so nothing is driven. **Learn the button first** ([§8.1](#81-learning-the-buttons)), then program it ([§8.2](#82-programming-a-gesture-no-app-needed)). |
| **Held AUX1 to program a gesture and expected a "saved" beep** | `PROGRAM_SAVED` is never played by this adapter (chapter 7.3). The confirmation here is the head unit capturing the held voltage, then `LEARN_OK` on release — there is no escalating beep count. |
| **A learned button is recognised but the stereo does nothing new** | A button you have only *learned* passes its single level through; its double/long need **programming** (§8.2) or an app **binding** (chapter 9). |
| **Pressed a button to program it and the head unit reacted instead** | Your press was treated as a real press, not a learn — the AUX1 hold had not armed first. Hold AUX1 for the full ~1.5 s (until `PROGRAM_ENTER` beeps) *before* pressing the wheel button. |
| **`OUT_VOLTAGE` had no effect on the radio** | The `key_mv` is probably above the head unit's own idle (so the line was released, not driven) or on a level the radio reads as a different key. Move it lower within the band (chapter 9.3). |
| **App says "No device found"** | Cable not connected, adapter unpowered, or no OTG support. Check the USB-C cable and that the adapter has power. The adapter still works without the app. |
| **App says "USB permission needed"** | Allow the Android USB prompt; replug the adapter to re-trigger it. |
| **App says "That is not the adapter"** | Another USB device is sharing the hub. Unplug it and retry. |
| **App says "Firmware and app versions differ"** | The app and adapter protocols disagree, so nothing is sent (a partial match corrupts configs). Update the app or the firmware so both match. |
| **App says "The adapter stopped responding"** | It answered then went quiet for 10 s. Check the cable and power — **a button press still works even while the app cannot see it.** Reopen the app or tap Try again. |
| **STATUS LED fast-blinking** | A fault. Listen to the buzzer: `FAULT_DAC` (output/DAC), `FAULT_CONFIG` (configuration corrupt — defaults loaded), `FAULT_INPUT` (ladder/rail wiring fault). A configuration fault clears once a good config is written; a hardware fault needs a restart and a wiring check. |
| **Buzzer plays `FAULT_INPUT`** | An open input or a short to a supply on the ladder wiring. Check `J2`/`J5` wiring. |
| **`KEY_UNKNOWN` on a button you pressed** | That button has not been learned (or the press did not match its window). Learn it (chapter 8). |
| **A button does nothing** | It is learned but its gesture is not **programmed** (§8.2) and not **bound** (chapter 9), so the adapter is presenting the gesture's default voltage and the *head unit* decides the effect — and a head unit that was never taught that voltage does nothing. Program the gesture, or bind it to a `key_mv`. |
| **App warns "lost frames"** | A flaky USB cable. Re-seat it and retry the operation; a dropped frame can make a command fail with no error otherwise. |
| **A button's action did not run** | The **phone's** half of an app-side action failed (a permission, a missing app). The adapter still sent the key press; the button itself is configured correctly. |
| **Learn keeps rejecting (`LEARN_REJECT`)** | The press was too light (barely left idle) or too noisy. Press the button firmly and hold it steady while AUX1 is held. |
| **The adapter behaves as if a button is held** | Check the ladder wiring — an open input can float high. Restart the adapter; it re-establishes the safe idle state on every boot. |
| **Nothing responds at all** | Press **RESET**. The adapter comes back in its safe idle state and resumes. |

## 14. Settings reference

Settings live in the adapter's configuration. They are written as part of a config
save; the app's screens cover bindings and the maintenance window, while the
timing/gain/feedback settings below are carried in the configuration and honoured
by the adapter.

| Setting | Default | Meaning |
| --- | --- | --- |
| `debounce_ms` | 25 | Contact debounce for a press |
| `double_press_off_ms` | 500 | Window in which a second tap counts as a **double** |
| `long_press_ms` | 750 | Hold time that counts as a **long** press |
| `send_duration_ms` | 200 | How long a presented key is held (bounded) |
| `gain_policy` | `AUTO` | Output range: `AUTO` detects the head unit; force **tracking** (3 V, gain 1.00) or **amplified** (5 V, gain 1.82) to override |
| `buzzer_level` | 2 | `OFF` / `QUIET` / `NORMAL` / `LOUD`. `OFF` silences everything except `BOOT_ERROR` and `FAULT_*` |
| `led_level` | 2 | `OFF` / `QUIET` / `NORMAL` / `LOUD` |
| `temp_comp_enabled` | — | Temperature compensation of the ladder (a stated v1 gap — see [CHANGELOG.md](CHANGELOG.md)) |
| `key_click_enabled` | **off** | Beep on every normal press, as a key-click confirmation. **Off by default** so ordinary switch operation stays silent; switch it on in the app if you want it |
| `maintenance_timeout_ms` | 300000 (5 min) | Maintenance window length; measured from the last activity (bounded to one hour) |
| `maintenance_on_boot` | off | Open the maintenance window on the next boot only |

Per-channel configuration includes the channel **name**, whether it is **enabled**,
its **learned ladder** (the buttons you taught), and its **output** profile (gain
mode and the idle DAC code).

---

*Connector pin details, the analog design, and the reasoning behind each choice are
in [DESIGN.md](DESIGN.md). Ordering and part information are in
[MANUFACTURING.md](MANUFACTURING.md). Revision history is in
[CHANGELOG.md](CHANGELOG.md).*
