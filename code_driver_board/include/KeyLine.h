#pragma once

// Helpers for the KEY line: reading the sense node, and finding the level the line
// rests at when it is floating.
//
// WHY THIS EXISTS. Several tests originally assumed the KEY line sits near 0 V when
// the output is released and J3 is open. It does not. The line has NO pull-up of its
// own (the 3V3 pull-ups are on the LADDER pins, not here), and Q4 can only SINK --
// so with nothing attached the node is left at whatever R36's 1M path and the
// op-amp's bias leave it, measured at ~3.26 V on this board.
//
// That matters because it bounds what the servo can do WITHOUT a head unit:
//
//   * Commands BELOW the float level are reachable -- the FET sinks the line down.
//   * Commands ABOVE it are not, and are not supposed to be: the servo turns the
//     FET off and the line floats back up. That IS the release behaviour, and
//     DESIGN.md 4.6 calls it a property of the circuit rather than a mode.
//
// So a test must ask "is this target below the line's resting level?" before
// calling a reading wrong. Asserting against a fixed expected value instead is how
// a correct board gets reported as broken -- which is exactly what happened here,
// and why the float level is now MEASURED rather than assumed.

#include <stdint.h>

namespace KeyLine {

// The KEY voltage for a channel, ×2 from the sense divider. Returns false if the
// ADC read failed.
bool SenseMv(int ch, int *out_key_mv);

// The level the channel's line rests at with the output RELEASED, averaged over
// several reads. This is the ceiling a command can reach on an open line.
int FloatMv(int ch);

// True if a command at `target_key_mv` is reachable on this channel right now,
// i.e. below the resting level. A target at or above it is a release, not a command
// (spec 6.2's headroom rule, in the form the bench can actually check).
bool CommandReachable(int ch, int target_key_mv);

// The highest KEY voltage the servo can actually command on this channel, which is
// the float level less spec 6.2's 200 mV of headroom. Returns 0 when that leaves no
// usable band.
int CommandCeilingMv(int ch);

}  // namespace KeyLine
