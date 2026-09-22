#pragma once

// The rig driver, device side: it PRESENTS a ladder voltage to the DUT's input
// and a switch state to the DUT's AUX1, on command.
//
// The wiring this assumes (both boards are the same hardware):
//
//   driver J3.3 (KEY1)  ──  DUT J2.3 (SWC1)      the ladder stimulus
//   driver J3.2 (KEY2)  ──  DUT J2.2 (SWC2)
//   driver J5.4 (AUX1)  ──  DUT J5.4 (AUX1)      the programming switch
//   GND ──────────────────  GND                  common ground is REQUIRED
//
// Driving a KEY voltage LOW sinks the DUT's ladder node -- that is a button
// press. Releasing the KEY line lets the DUT's own pull-up set the node high --
// that is the released state. There is no separate "button" signal: the analog
// level IS the button, exactly as it is on a car.
//
// The AUX1 pin is driven as a plain GPIO: OUTPUT LOW shorts it (switch closed),
// INPUT/floating leaves it high-Z so the DUT's pull-up holds it open. It must
// NOT be read by the driver's own ADC while it is being driven -- it is
// repurposed from an ADC pad to a GPIO, so no driver code may call
// Adc::ReadMv(kAux1).

#include <stdint.h>

#include "DriverHarness.h"
#include "swc_logic/Output.h"

namespace Driver {

// Bring up the DAC and the AUX stimulus pin, park the KEY output released, and
// print the wiring this expects. Returns false if the DAC did not answer.
bool Begin();

// Which DUT channel (1 or 2) the KEY stimulus is presented on. Defaults to 1.
void SelectChannel(int ch);
int  Channel();

// The gain mode used to reach a commanded level. Auto measures the released
// level on the selected channel and applies spec 6.2's selection to it.
void SetMode(Output::Mode m);
void AutoSelectMode();
Output::Mode Mode();

// Present `key_mv` on the selected channel's KEY line, in the current mode.
// Returns the DAC code written, or -1 if the target is unreachable (above the
// rail, or above the line's own resting level, where a command is a release).
int DriveKeyMv(int key_mv);

// Let the selected channel's KEY line float: the released state.
void Release();

// Read the level actually present on the selected channel's KEY line, x2 from
// the sense divider. Returns false on an ADC error. This is what lets the
// operator confirm the wiring before trusting a gesture.
bool ReadKeyMv(int *out_mv);

// The programming switch: closed drives the AUX1 pin to GND through the DUT's
// 1k series resistor (R23, shorted per the rig), open leaves it floating.
void SetAuxClosed(bool closed);
bool AuxClosed();

// Run the DUT's full AUX1 learn wizard for one slot (spec 7.4 / FR-31), with the
// rig standing in for the user:
//   1. hold AUX1 past the enter threshold, release   -> the DUT enters select
//   2. `slot` short AUX1 presses                     -> the DUT counts the slot
//   3. a pause past the select gap                    -> the DUT starts sampling
//   4. hold the commanded key through the sampling    -> the DUT measures it
// The timing mirrors the DUT's LearnWizard constants; see the .cpp for each.
void Learn(int slot);

// Hold AUX1 long enough to open the DUT's maintenance window (FR-38's entry).
void OpenMaintenanceWindow();

// The rig's timing config, as currently set.
const Harness::RigConfig &Rig();
void SetRig(const Harness::RigConfig &c);

// Play a timeline: run every step in order, waiting each step's `after_ms` and
// printing what it did. Blocks for the timeline's total duration.
void Play(const Harness::Timeline &t, const char *label);

}  // namespace Driver
