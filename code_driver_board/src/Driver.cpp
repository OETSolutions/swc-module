#include "Driver.h"

#include <Arduino.h>

#include "Adc.h"
#include "BoardPins.h"
#include "Dac.h"
#include "KeyLine.h"
#include "Log.h"
#include "driver/gpio.h"

// Included DIRECTLY here, not only through Driver.h. PlatformIO's library
// dependency finder scans the includes of files in `src/`; an include that is
// reached only through a header in `include/` is not followed, so the
// swc_logic library would never be linked and the build would fail with
// undefined references to Output:: at link time. The bring-up tool carries the
// same include in its own src/ for the same reason.
#include "swc_logic/Output.h"

// The AUX1 pad is an ADC channel on the DUT, but on the DRIVER it is only ever an
// output: it sinks the DUT's AUX1 node when the programming switch is closed.
//
// It is driven with the IDF gpio API rather than pinMode() because the pin is
// attached to the ADC peripheral (Adc::Begin configured it), and while that
// attachment holds, `pinMode`/`digitalWrite` do not reliably own the pad -- the
// same trap the bring-up tool's AUX probe documents at length. gpio_reset_pin
// detaches it from the ADC mux and hands it back to plain GPIO.
static const uint8_t kAuxStimPin = PIN_AUX1;   // J5.4

namespace Driver {

namespace {
int            s_channel = 1;
Output::Mode   s_mode    = Output::Mode::kAmplified;
bool           s_aux     = false;
Harness::RigConfig s_rig  = Harness::kDefaultRig;

// The mode auto-selection landed on, for the log line.
const char *ModeStr(Output::Mode m) { return Output::ModeName(m); }

void AuxApply(bool closed)
{
    gpio_reset_pin((gpio_num_t)kAuxStimPin);
    gpio_set_pull_mode((gpio_num_t)kAuxStimPin, GPIO_FLOATING);
    if (closed) {
        pinMode(kAuxStimPin, OUTPUT);
        digitalWrite(kAuxStimPin, LOW);      // sink the DUT's AUX1 to GND
    } else {
        pinMode(kAuxStimPin, INPUT);          // high-Z: the DUT's pull-up holds it open
    }
}
}  // namespace

bool Begin()
{
    Log::Section("RIG DRIVER -- SWC stimulus for the device under test");

    const bool dac_ok = Dac::Begin();
    Log::Printf("DAC at 0x%02X: %s", Dac::Address(), dac_ok ? "present" : "NOT FOUND");

    // The output MUST start released: a stray command on boot is a phantom key
    // press on the car the DUT is emulating.
    Dac::Release(1);
    Dac::Release(2);

    AuxApply(false);
    Log::Printf("AUX1 programming switch: OPEN (pin IO%d high-Z)", kAuxStimPin);

    Log::Rule('-');
    Log::Printf("  Wiring this rig expects (both boards are the same hardware):");
    Log::Printf("    driver J3.3 KEY1  ->  DUT J2.3 SWC1   (ladder stimulus)");
    Log::Printf("    driver J3.2 KEY2  ->  DUT J2.2 SWC2");
    Log::Printf("    driver J5.4 AUX1  ->  DUT J5.4 AUX1   (programming switch)");
    Log::Printf("    GND               ->  GND             (common ground is required)");
    Log::Rule('-');

    // Read the released level on each channel so the operator sees the rig is
    // connected before running anything that depends on it.
    for (int ch = 1; ch <= 2; ++ch) {
        const int idle = KeyLine::FloatMv(ch);
        Log::Printf("  channel %d released level: %d mV  (a command must sit BELOW this)",
                    ch, idle);
    }
    Log::Printf("");
    return dac_ok;
}

void SelectChannel(int ch) { s_channel = (ch == 2) ? 2 : 1; }
int  Channel()             { return s_channel; }

void SetMode(Output::Mode m) { s_mode = m; }
Output::Mode Mode()          { return s_mode; }

void AutoSelectMode()
{
    const int idle = KeyLine::FloatMv(s_channel);
    Output::Mode m = s_mode;
    const Output::Decision d = Output::SelectFromIdleKeyMv(idle, &m);
    s_mode = m;
    Log::Printf("auto mode: idle %d mV -> %s -> %s (gain %d/%d)",
                idle, Output::DecisionName(d), ModeStr(m),
                Output::GainNum(m), Output::GainDen(m));
}

int DriveKeyMv(int key_mv)
{
    // Reachability: on this rig the DUT's ladder node can only be pulled DOWN
    // from its resting level, so a target at or above the float level is a
    // release, not a command. Report it rather than writing a code that cannot
    // take effect.
    const int idle = KeyLine::FloatMv(s_channel);
    if (key_mv >= idle) {
        Log::Printf("drive %d mV refused: at or above the released level (%d mV) -- "
                    "this is a release, not a command", key_mv, idle);
        return -1;
    }

    int code = Output::CodeForTargetKeyMv(s_mode, key_mv);
    if (code < 0) {
        Log::Printf("drive %d mV refused: unreachable in %s mode", key_mv, ModeStr(s_mode));
        return -1;
    }
    if (code > Output::kDacMaxCode) code = Output::kDacMaxCode;

    const int wrote = Dac::SetSignal(s_channel, s_mode, (uint16_t)code);
    if (wrote < 0) {
        Log::Printf("drive %d mV: DAC write FAILED (bus error)", key_mv);
        return -1;
    }
    return code;
}

void Release()
{
    Dac::Release(s_channel);
    Dac::Release(1);
    Dac::Release(2);
}

bool ReadKeyMv(int *out_mv)
{
    int mv = 0;
    const Adc::Ch ch = (s_channel == 2) ? Adc::kSense2 : Adc::kSense1;
    uint32_t sense = 0;
    if (!Adc::ReadAvgMv(ch, 16, &sense)) return false;
    mv = Output::KeyMvFromSenseMv((int)sense);
    if (out_mv) *out_mv = mv;
    return true;
}

void SetAuxClosed(bool closed)
{
    s_aux = closed;
    AuxApply(closed);
}

bool AuxClosed() { return s_aux; }

// ---------------------------------------------------------------------------
// The AUX1 learn sequence, and the DUT constants it has to straddle.
//
// Mirrored from code/lib/Learning/LearnWizard.h. They are constants with names so
// the relationship is legible: a short press must be SHORTER than the enter hold
// (or the DUT reads it as an enter, not a selection), the gap must EXCEED the
// select gap (or the DUT does not yet consider selection finished), and the key
// must be held for the whole sampling prompt.
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t kDutEnterHoldMs   = 1500;  // LearnWizard::kEnterHoldMs
constexpr uint32_t kDutSelectGapMs   = 1200;  // LearnWizard::kSelectGapMs
constexpr uint32_t kShortPressMs     = 150;   // must be < kDutEnterHoldMs
constexpr uint32_t kShortReleaseMs   = 200;   // must be < kDutSelectGapMs
constexpr uint32_t kSelectSettleMs   = 1500;  // > kDutSelectGapMs: ends selection
constexpr uint32_t kSampleHoldMs     = 600;   // >= LearnSession's 100 ms span, sampled
}  // namespace

void Learn(int slot)
{
    if (slot < 1) slot = 1;
    if (slot > 8) slot = 8;
    Log::Section("LEARN -- the DUT's AUX1 wizard, driven by the rig");
    Log::Printf("  slot %d, key %d mV, channel %d", slot, s_rig.key_mv, s_channel);

    // Step 1: enter. The hold must clear the enter threshold; the release after
    // it is what lets the DUT re-arm so the first SELECTION press is not swallowed.
    Log::Printf("  1. holding AUX1 %u ms to enter...", (unsigned)(kDutEnterHoldMs + 300));
    SetAuxClosed(true);
    delay(kDutEnterHoldMs + 300);
    SetAuxClosed(false);
    delay(kShortReleaseMs);

    // Step 2: select the slot with `slot` short presses.
    for (int i = 0; i < slot; ++i) {
        SetAuxClosed(true);
        delay(kShortPressMs);
        SetAuxClosed(false);
        delay(kShortReleaseMs);
    }
    Log::Printf("  2. sent %d selection press%s", slot, slot == 1 ? "" : "es");

    // Step 3: the gap that ends selection and starts the sampling prompt.
    Log::Printf("  3. waiting %u ms for selection to end (LEARN_PROMPT)...",
                (unsigned)kSelectSettleMs);
    delay(kSelectSettleMs);

    // Step 4: hold the key while the DUT samples it. The level must be genuinely
    // present (a real press), or the DUT rejects it as "button not pressed".
    const int code = DriveKeyMv(s_rig.key_mv);
    if (code < 0) {
        Log::Printf("  4. ABORT: key %d mV is not reachable -- check the wiring", s_rig.key_mv);
        Release();
        SetAuxClosed(false);
        return;
    }
    Log::Printf("  4. holding the key at %d mV (code %d) for %u ms while the DUT samples...",
                s_rig.key_mv, code, (unsigned)kSampleHoldMs);
    delay(kSampleHoldMs);
    Release();
    Log::Printf("  5. released. Watch the DUT: LEARN_OK = stored, LEARN_REJECT = refused");
    Log::Printf("     (out of range / too close to an existing button / not actually pressed).");
}

void OpenMaintenanceWindow()
{
    Log::Section("MAINTENANCE -- hold AUX1 alone past the window entry");
    SetAuxClosed(true);
    Log::Printf("  AUX1 closed; holding 3200 ms (past the ~3 s maintenance hold)...");
    delay(3200);
    SetAuxClosed(false);
    Log::Printf("  released. The DUT should have opened its ~5 minute maintenance window.");
    Log::Printf("  (FR-32: the radio behind that window is not implemented yet -- the window");
    Log::Printf("   itself should still open; see the DUT's HANDOFF.)");
}

const Harness::RigConfig &Rig() { return s_rig; }
void SetRig(const Harness::RigConfig &c) { s_rig = c; }

void Play(const Harness::Timeline &t, const char *label)
{
    Log::Printf("");
    Log::Section(label);
    Log::Printf("  %d steps, %u ms total, channel %d, key %d mV, mode %s",
                t.count, (unsigned)t.total_ms, s_channel, s_rig.key_mv, ModeStr(s_mode));

    for (int i = 0; i < t.count; ++i) {
        const Harness::Step &s = t.steps[i];
        if (s.after_ms) delay(s.after_ms);
        switch (s.action) {
            case Harness::Action::kDrive: {
                const int code = DriveKeyMv(s.mv);
                Log::Printf("  step %d: DRIVE %d mV (code %d)", i, s.mv, code);
                break;
            }
            case Harness::Action::kRelease:
                Release();
                Log::Printf("  step %d: RELEASE (idle)", i);
                break;
            case Harness::Action::kMark:
                Log::Printf("  step %d: wait %u ms (the DUT judges the gesture)",
                            i, (unsigned)s.after_ms);
                break;
        }
    }
    Release();
    Log::Printf("  -> done; the KEY line is released. Watch the DUT's buzzer/LED "
                "and its USB output for the gesture it resolved.");
}

}  // namespace Driver
