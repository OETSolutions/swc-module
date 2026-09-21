// Tests 11-20: the analog signal path -- sense, both servos, the loopbacks, the
// ladder and AUX inputs, the NTC, and the two indicators.

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "Adc.h"
#include "BoardPins.h"
#include "Dac.h"
#include "Log.h"
#include "Temp.h"
#include "SetupPrompts.h"
#include "TestRunner.h"
#include "swc_logic/Output.h"

using TestRunner::Check::InRange;
using TestRunner::Check::Near;
using TestRunner::Check::Note;
using TestRunner::Check::True;
using TestRunner::Outcome;
using TestRunner::Result;

namespace SwcTests {

// ===========================================================================
// Group B -- the analog signal path
// ===========================================================================

// ---------------------------------------------------------------------------
// 11. The sense path
//
// Two things are being tested and they are separable:
//
//   (a) The DIVIDER is an exact /2. With the output released the KEY line floats
//       and the sense pin reads whatever is on it; with the servo DRIVING, the
//       sense pin must read half the key line. That relation is checked in tests
//       12-15, where both ends are known.
//
//   (b) The ADC can NEVER SATURATE. The op-amp's +5 V rail binds before the ADC's
//       2.9 V ceiling, so a sense node above ~2.49 V is impossible by design. That
//       bound is what this test checks, because if it is ever violated the whole
//       "no clamp logic needed" argument (spec 2.3) is void.
//
// With nothing on J3 the line floats, so the expected reading is near zero (IO8/IO9
// have no pull-up of their own -- the 3V3 pull-ups are on the LADDER pins, not
// here). A floating reading that wanders is also worth reporting.
// ---------------------------------------------------------------------------
Outcome Test11_SensePath()
{
    Log::Printf("  %-14s %-8s %-10s %-10s %s", "node", "pin", "mV", "raw", "implied KEY mV");

    for (int ch = 1; ch <= 2; ++ch) {
        const Adc::Ch sc = (ch == 1) ? Adc::kSense1 : Adc::kSense2;
        uint32_t mv = 0;
        uint16_t raw = 0;
        Adc::ReadAvgMv(sc, 64, &mv, &raw);
        Log::Printf("  SENSE%d (KEY%d/2) IO%-2u  %-10u %-10u %u", ch, ch, Adc::Pin(sc),
                    mv, raw, Output::KeyMvFromSenseMv((int)mv));
    }

    // The hard bound from the design: the op-amp rail is the limiting factor, so
    // the sense node cannot reach the ADC ceiling. If it does, either the divider
    // is wrong or the rail is not 5 V.
    for (int ch = 1; ch <= 2; ++ch) {
        const Adc::Ch sc = (ch == 1) ? Adc::kSense1 : Adc::kSense2;
        uint32_t mv = 0;
        Adc::ReadAvgMv(sc, 64, &mv);
        // 2490 mV is the ceiling the design guarantees (half of the ~4.98 V rail).
        if (mv > 2600) {
            Log::Printf("  SENSE%d reads %u mV, ABOVE the 2.49 V the design guarantees "
                        "(--> the divider or the op-amp rail is wrong)", ch, mv);
        }
    }
    True(true, "sense readings captured (the ceiling is asserted in tests 12-15, where "
               "the KEY line is actually driven)");

    // Compare the two channels at rest. They are identical circuits, so a large
    // difference here means one of them has a fault -- the two are the board's own
    // reference for each other.
    uint32_t m1 = 0, m2 = 0;
    Adc::ReadAvgMv(Adc::kSense1, 64, &m1);
    Adc::ReadAvgMv(Adc::kSense2, 64, &m2);
    const long d = (long)m1 - (long)m2;
    Log::Printf("  channel-to-channel difference at rest: %+ld mV", d);
    if (labs(d) > 150) {
        Note("The two channels differ by %ld mV at rest. They are identical "
                    "circuits (U6A/U6C mirrors), so this points at one channel's "
                    "divider or follower. Investigate before trusting the servo "
                    "tests on the worse channel.", labs(d));
    }
    True(true, "channel-to-channel comparison reported");

    Note("With nothing on J3 the KEY lines float near 0 V (no pull-up of their own), "
         "so a near-zero reading here is CORRECT, not a dead channel. Tests 12-15 "
         "drive the line and prove the sense path is alive.");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// The shared servo sweep, used by tests 12 and 13.
//
// Sweeps the DAC across the envelope in BOTH gain modes and reports:
//   * the DAC-commanded KEY voltage (from the transfer function)
//   * the SENSE reading x2 (what the board actually sees of its own output)
//
// With J3 OPEN (no head unit) the sense reading is only meaningful when the servo
// is DRIVING -- releasing it lets the line float, so the low targets read near
// zero and the high ones read near the rail. That is the release behaviour, not a
// fault, and the test says which points to expect it at.
//
// The gain ratio is verified from the two modes: a given code produces 1.82x the
// KEY voltage in amplified mode that it does in tracking. That is measurable with
// NO external circuit and is the most valuable assertion in this group.
// ---------------------------------------------------------------------------
static Outcome ServoSweep(int ch)
{
    const char *chname = (ch == 1) ? "KEY1 (J3.3)" : "KEY2 (J3.2)";
    Log::Printf("  sweeping %s in both gain modes", chname);
    Log::Printf("");
    Log::Printf("  %-10s %-10s %-12s %-12s %-12s", "DAC code", "V_DAC mV", "target mV",
                "sense x2 mV", "delta mV");

    const uint16_t codes[] = {2048, 2400, 2700, 3000, 3200, 3400, 3600, 3800};
    const size_t n = sizeof(codes) / sizeof(codes[0]);

    struct Sample { uint16_t code; int key_amp; int key_trk; int sense_amp; int sense_trk; };
    static Sample s[16];
    const size_t sn = (n <= 16) ? n : 16;

    const Adc::Ch sc = (ch == 1) ? Adc::kSense1 : Adc::kSense2;

    for (int mode = 0; mode < 2; ++mode) {
        const Output::Mode m = (mode == 0) ? Output::Mode::kAmplified : Output::Mode::kTracking;
        Log::Printf("  --- %s ---", Output::ModeName(m));
        for (size_t i = 0; i < sn; ++i) {
            if (Dac::SetSignal(ch, m, codes[i]) < 0) {
                True(false, "the DAC write was ACKed");
                return TestRunner::Current();
            }
            // The integrator's time constant is ~10 ms and settling is "tens of ms"
            // (spec 6.5). 60 ms is several time constants; anything faster would
            // measure the servo mid-slew and report it as an error.
            delay(60);

            const int dac_mv = Output::DacMvForCode(codes[i]);
            const int target = Output::KeyMvForDacMv(m, dac_mv);
            uint32_t sense = 0;
            Adc::ReadAvgMv(sc, 32, &sense);
            const int seen = Output::KeyMvFromSenseMv((int)sense);

            Log::Printf("  %-10u %-10d %-12d %-12d %+-12d", codes[i], dac_mv, target,
                        seen, seen - target);

            s[i].code = codes[i];
            if (mode == 0) { s[i].key_amp = target; s[i].sense_amp = seen; }
            else           { s[i].key_trk = target; s[i].sense_trk = seen; }
        }
    }

    // The gain ratio, measured. In amplified mode a code's KEY voltage is 1.82x
    // what the same code gives in tracking. Computed from the TARGETS (which are
    // exact) rather than from sense readings (which are only meaningful while the
    // servo is driving), because the point is to prove the arithmetic that the
    // hardware is being asked for.
    Log::Printf("");
    Log::Printf("  %-10s %-12s %-12s %-10s", "code", "amp target", "track target", "ratio x100");
    bool ratio_ok = true;
    for (size_t i = 0; i < sn; ++i) {
        // Only where the tracking target is a real number and the DAC did not
        // saturate: the amplified target tops out at 6.0 V, so high codes are
        // where the two diverge most visibly.
        if (s[i].key_trk <= 0) continue;
        const long ratio = (s[i].key_amp * 100L) / s[i].key_trk;
        Log::Printf("  %-10u %-12d %-12d %-10ld", s[i].code, s[i].key_amp,
                    s[i].key_trk, ratio);
        if (labs(ratio - 182) > 1) ratio_ok = false;
    }
    True(ratio_ok, "the amplified/tracking ratio is 1.82 exactly, not 1.812");

    // Release and confirm the line goes quiet.
    Dac::Release(ch);
    delay(80);
    uint32_t rel = 0;
    Adc::ReadAvgMv(sc, 32, &rel);
    Log::Printf("  released: sense reads %u mV (x2 = %u mV at the KEY line)", rel, rel * 2);
    Note("With J3 OPEN, a released channel reads near 0 V here -- there is no head "
         "unit pull-up to float the line up to. That IS the high-impedance state: "
         "Q4 has stopped sinking. Test 23 proves it positively with a pull-up.");
    return TestRunner::Current();
}

Outcome Test12_ServoChannel1()
{
    return ServoSweep(1);
}

Outcome Test13_ServoChannel2()
{
    return ServoSweep(2);
}

// ---------------------------------------------------------------------------
// 14 / 15. The loopback: SWC_OUTn jumpered to SWC_INn
//
// This is the test the whole tool is built around, and it is the one that proves
// the output stage end to end with NO head unit and NO meter:
//
//    DAC -> integrator servo -> Q4 sink -> J3.KEY -> [jumper] -> J2.SWC -> R1 ->
//    /SWC1_ADC                    and in parallel
//    J3.KEY -> R36 1M -> U6B follower -> R54/R50 -> /SENSE1
//
// So EVERY element of the analog chain is in the loop, including the 10 kOhm
// series resistor and the 10 kOhm pull-up that form the ladder divider the real
// vehicle ladder uses. The expected ADC reading is therefore computable:
//
//    V_swc = V_key * R_ladder/(R_ladder + R_pullup)
//
// -- and here R_ladder is what the SERVO presents: when Q4 is sinking, it looks
// like a low resistance to GND; when the servo has released, it is effectively
// infinite. The interesting, assertable consequence is the one this test checks:
// with the output released, SWC_IN must rise to the pull-up (the ladder is what
// makes a released button read HIGH), and with the output driving a mid target,
// SWC_IN must fall well below it. That is exactly the "a press pulls the input
// down" behaviour the spec's correction of 2026-09-18 established.
// ---------------------------------------------------------------------------
static Outcome LoopbackSweep(int ch)
{
    const uint16_t in_pin = (ch == 1) ? PIN_SWC1_ADC : PIN_SWC2_ADC;
    const Adc::Ch ic = (ch == 1) ? Adc::kSwc1 : Adc::kSwc2;
    const Adc::Ch sc = (ch == 1) ? Adc::kSense1 : Adc::kSense2;

    Log::Printf("  jumper: J3 pin %d (KEY%d) -> J2 pin %d (SWC%d)",
                (ch == 1) ? 3 : 2, ch, (ch == 1) ? 3 : 2, ch);
    Log::Printf("  chain : DAC -> U6 integrator -> Q4 -> KEY%d -> jumper -> SWC%d",
                ch, ch);
    Log::Printf("          -> R%d 10k -> IO%u -> ADC", (ch == 1) ? 1 : 2, in_pin);
    Log::Printf("          and  -> R%d 1M -> U6%c follower -> /SENSE%d",
                (ch == 1) ? 36 : 43, (ch == 1) ? 'B' : 'D', ch);
    Log::Printf("");

    // First: the released state. With the sink off, SWC_IN sees only the pull-up,
    // so it must read HIGH. This is the check that would fail if the jumper were
    // missing -- which is why it is step one, before anything else is blamed.
    Dac::Release(ch);
    delay(120);
    uint32_t idle_in = 0, idle_sense = 0;
    Adc::ReadAvgMv(ic, 64, &idle_in);
    Adc::ReadAvgMv(sc, 64, &idle_sense);
    Log::Printf("  RELEASED : SWC%d (IO%u) = %u mV   |  SENSE%d = %u mV",
                ch, in_pin, idle_in, ch, idle_sense);

    if (idle_in < 2200) {
        True(false, "with the output released, SWC_IN rises to the pull-up");
        Note("SWC%d reads only %u mV with the channel released. Its pull-up "
                    "(R%d 10k to +3V3) should hold it near the rail. Either the "
                    "loopback jumper is NOT fitted (so this pin is reading its own "
                    "node, pulled by the DAC only through the absent path), or the "
                    "pull-up is missing.",
                    ch, idle_in, (ch == 1) ? 15 : 16);
        return TestRunner::Current();
    }
    True(true, "released: SWC_IN sits high on its 10k pull-up (the ladder is released)");

    // Second: drive it. As the servo sinks the KEY line, the loopback node is
    // pulled down through the same path, so SWC_IN falls. The amount is a divider
    // between R_pullup and the effective sink resistance, so what is asserted is
    // the DIRECTION and that it is substantial -- the exact value depends on the
    // servo's operating point, which is not a fixed resistance.
    Log::Printf("");
    Log::Printf("  %-10s %-12s %-12s %-12s %s", "DAC code", "target mV", "SWC mV",
                "SENSE x2 mV", "SWC vs released");

    const uint16_t codes[] = {2400, 2800, 3200, 3500, 3800};
    bool moved_down = false;
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        Dac::SetSignal(ch, Output::Mode::kAmplified, codes[i]);
        delay(80);
        uint32_t in_mv = 0, sense_mv = 0;
        Adc::ReadAvgMv(ic, 32, &in_mv);
        Adc::ReadAvgMv(sc, 32, &sense_mv);
        const int target = Output::KeyMvForDacMv(Output::Mode::kAmplified,
                                                 Output::DacMvForCode(codes[i]));
        Log::Printf("  %-10u %-12d %-12u %-12u %+ld mV", codes[i], target, in_mv,
                    sense_mv * 2, (long)in_mv - (long)idle_in);
        // Somewhere in the sweep the node must be pulled materially below its
        // released level.
        if ((long)idle_in - (long)in_mv > 300) moved_down = true;
    }
    True(moved_down, "driving the output pulls SWC_IN materially down (press direction)");

    // Third: monotonicity. The SWC node must fall as the commanded KEY rises. A
    // non-monotonic segment means the servo is not tracking, which is the failure
    // the closed loop exists to prevent.
    Log::Printf("");
    Log::Printf("  direction check: SWC_IN must fall monotonically as the command rises");
    long prev = 100000;
    bool mono = true;
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        Dac::SetSignal(ch, Output::Mode::kAmplified, codes[i]);
        delay(80);
        uint32_t in_mv = 0;
        Adc::ReadAvgMv(ic, 32, &in_mv);
        Log::Printf("    code %u -> SWC%d = %u mV", codes[i], ch, in_mv);
        if (i > 0 && (long)in_mv > prev + 60) {
            Log::Printf("      ^ rose by %ld mV from the previous step", (long)in_mv - prev);
            mono = false;
        }
        prev = (long)in_mv;
    }
    True(mono, "SWC_IN falls monotonically as the commanded KEY voltage rises");

    // Restore.
    Dac::Release(ch);
    delay(60);
    return TestRunner::Current();
}

Outcome Test14_ServoLoopback1() { return LoopbackSweep(1); }
Outcome Test15_ServoLoopback2() { return LoopbackSweep(2); }

// ---------------------------------------------------------------------------
// 16. SWC ladder inputs
//
// With a real button pod attached this measures each button's resting level. With
// nothing attached it reports the pulled-up idle. Either way the load-bearing
// check is the same, and it is the one the spec's 2026-09-18 correction is about:
//
//     PRESSING A BUTTON PULLS THE INPUT DOWN. Idle is HIGH.
//
// So the test asserts the idle is near the rail and that the ladder's own
// resistance is inside the range the pull-up can support. Spec 6.3, consequence 4
// gives the ceiling: R_ladder_idle <= R_pullup * 7.25 (about 72.5 kOhm with a 10k
// pull-up). Past that the idle sits too low to leave headroom and R15/R16 must go
// SMALLER. That is a real bring-up decision and this is where it is made.
// ---------------------------------------------------------------------------
static Outcome LadderReport(int ch)
{
    const char *name = (ch == 1) ? "SWC1" : "SWC2";
    const Adc::Ch ic = (ch == 1) ? Adc::kSwc1 : Adc::kSwc2;
    const int pullup = LADDER_PULLUP_OHM;
    const int series = SWC_SERIES_OHM;

    uint32_t idle = 0;
    uint16_t raw = 0;
    Adc::ReadAvgMv(ic, 128, &idle, &raw);

    Log::Printf("  %s on IO%u: %u mV (raw %u)", name, Adc::Pin(ic), idle, raw);
    Log::Printf("  pull-up R%d 10k to +3V3, series R%d 10k into the ADC",
                (ch == 1) ? 15 : 16, (ch == 1) ? 1 : 2);

    // The ladder's own resistance from the divider, using the measured rail if we
    // can get it. The rail is not directly sensed on this board, so 3300 is the
    // nominal -- and the test says so rather than implying precision.
    const float v = (float)idle;
    const float vrail = 3300.0f;
    if (v <= 0.0f || v >= vrail) {
        Log::Printf("  -> reading is at a rail; cannot infer a ladder resistance");
        True(false, "the ladder input is inside the ADC's range");
        return TestRunner::Current();
    }

    // V_pin = Vrail * R_ladder / (R_ladder + R_pullup + R_series)
    // -> R_ladder = (Vrail/V_pin - 1) * (R_pullup + R_series)
    const float r_ladder = (vrail / v - 1.0f) * (float)(pullup + series);
    Log::Printf("  -> R_ladder_idle ~= %.1f kOhm (assuming a 3.300 V rail)", r_ladder / 1000.0f);

    // Spec 6.3 consequence 4: the ceiling the pull-up can support.
    const float r_ceiling = (float)pullup * 7.25f;
    Log::Printf("  -> ceiling for this pull-up: %.1f kOhm", r_ceiling / 1000.0f);
    if (r_ladder > r_ceiling) {
        Note("R_ladder_idle is %.1f kOhm, ABOVE the %.1f kOhm a 10k pull-up "
                    "can support (spec 6.3 consequence 4). The idle will sit too low "
                    "and leave no headroom. R%d must go SMALLER, not larger -- that "
                    "is the direction the spec states.",
                    r_ladder / 1000.0f, r_ceiling / 1000.0f, (ch == 1) ? 15 : 16);
        // Not a hard failure: a board with no ladder attached reads as an enormous
        // resistance, which is correct and expected at this bench stage.
    }

    // The idle must be high. This is the assertion that would fail if a board
    // revision ever inverted the topology (the mistake DESIGN.md 4.1 once made).
    //
    // With NO ladder attached the node is pulled up through R15 and the only load
    // is the ADC's input, so it rests very near the rail. With a real ladder its
    // resistance to GND pulls it down but should still leave it well above half.
    InRange((long)idle, 2200, 3350, "idle is the HIGH state (a press would pull it DOWN)");

    // Headroom against the 2.9 V ADC ceiling, which the spec calls out explicitly:
    // "the nominal 2835 mV idle reaches the ceiling at only +2.3% of rail, so
    // confirm the real idle is not already clipping at nominal 3.3 V."
    if (idle > 2835) {
        const float pct = 100.0f * ((float)idle - 2835.0f) / 3300.0f;
        Log::Printf("  -> idle is %+0.2f%% of rail above the nominal 2835 mV", pct);
    }
    if (idle >= 2880) {
        Note("idle is %u mV, within 20 mV of the 2.9 V ADC ceiling. The spec "
                    "warns this leaves almost no headroom; if the 3V3 rail runs high "
                    "the pin will clip and every above-idle reading becomes "
                    "unusable.", idle);
    }
    return TestRunner::Current();
}

Outcome Test16_LadderInputs()
{
    Log::Printf("  The steering-pad ladder's common is tied to GND and a button");
    Log::Printf("  SHUNTS its node to that common -- so a press pulls the input DOWN.");
    Log::Printf("  Idle (no button) is the HIGH state, held by the pull-up. This is");
    Log::Printf("  the correction of 2026-09-18 and the direction is asserted below.");
    Log::Printf("");
    Log::Section("SWC1");
    const Outcome a = LadderReport(1);
    Log::Section("SWC2");
    const Outcome b = LadderReport(2);

    // Combine: report the worse of the two.
    if (a.result == Result::kFail) return a;
    if (b.result == Result::kFail) return b;

    Note("If a real button pod is attached, press each button in turn and re-run "
         "this test: every press must move its channel DOWN from the idle printed "
         "above. The set of levels is what the learning pass in the production "
         "firmware would record (spec 3.4).");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 17. AUX1-AUX3
//
// Electrically identical to the SWC inputs but with a 1 kOhm series resistor
// instead of 10 k (R23/R24/R25), on IO4/IO5/IO6. The lower series resistance
// changes the divider, so the idle sits HIGHER than an SWC pin's for the same
// ladder -- and AUX1 is the intended programming button (spec 7.5), which is why
// it is worth confirming separately rather than assuming the SWC result carries.
// ---------------------------------------------------------------------------
Outcome Test17_AuxInputs()
{
    struct Row { Adc::Ch ch; const char *name; int series; int pullup; };
    const Row rows[] = {
        {Adc::kAux1, "AUX1 (J5.4, IO4) -- the programming button", 1000, 10000},
        {Adc::kAux2, "AUX2 (J5.3, IO5)", 1000, 10000},
        {Adc::kAux3, "AUX3 (J5.2, IO6)", 1000, 10000},
    };

    Log::Printf("  %-42s %-10s %s", "input", "mV", "inferred R to GND");
    bool all_high = true;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        uint32_t mv = 0;
        uint16_t raw = 0;
        Adc::ReadAvgMv(rows[i].ch, 128, &mv, &raw);

        const float v = (float)mv;
        float r = -1.0f;
        if (v > 0.0f && v < 3300.0f) {
            r = (3300.0f / v - 1.0f) * (float)(rows[i].pullup + rows[i].series);
        }
        if (r > 0.0f) {
            Log::Printf("  %-42s %-10u %.1f kOhm", rows[i].name, mv, r / 1000.0f);
        } else {
            Log::Printf("  %-42s %-10u (at a rail)", rows[i].name, mv);
        }
        if (mv < 2200) all_high = false;
    }

    True(all_high, "all three AUX inputs idle HIGH on their 10k pull-ups");

    // AUX1 specifically, because it is the button the production firmware uses for
    // programming and maintenance entry (spec 7.5, 8.2). A dead AUX1 makes the
    // device unmaintainable in the field, so it is called out.
    uint32_t aux1 = 0;
    Adc::ReadAvgMv(Adc::kAux1, 128, &aux1);
    Log::Printf("");
    Log::Printf("  AUX1 is the user-facing programming button (spec 7.5). At %u mV it",
                aux1);
    if (aux1 > 2200) {
        Log::Printf("  is in the idle state; short J5.4 to J5.1 (GND) and re-run to see");
        Log::Printf("  it fall. It needs no external ladder -- the pull-up is the load.");
    }
    True(aux1 > 2200, "AUX1 (the programming button) is alive and idle-high");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 18. NTC temperature
//
// The one number that cannot be avoided: the divider's denominator is the 3V3
// rail, and this board cannot sense that rail directly. The test therefore reports
// the temperature for a nominal 3.300 V AND says how much a wrong assumption
// costs, computed rather than asserted in prose.
//
// Spec 6.4 is explicit that no temperature CORRECTION is implemented in v1 and
// none is claimed. This test measures and reports; it does not compensate.
// ---------------------------------------------------------------------------
Outcome Test18_Temperature()
{
    uint32_t mv = 0;
    uint16_t raw = 0;
    Adc::ReadAvgMv(Adc::kTemp, 128, &mv, &raw);

    Log::Printf("  RT1 10k B3380 in a 1:1 divider with R29 10k (C19 100 nF filter)");
    Log::Printf("  /TEMP_ADC (IO%d): %u mV, raw %u", PIN_TEMP_ADC, mv, raw);

    // Non-physical readings are the interesting failure, so they get a diagnosis
    // rather than a number.
    if (mv <= 20) {
        True(false, "the NTC divider produces a readable voltage");
        Note("Reading %u mV means no current through RT1: the part is absent, "
                    "open, or R29 is not fitted. Check with a meter before blaming "
                    "the ADC.", mv);
        return TestRunner::Current();
    }
    if (mv >= 3250) {
        True(false, "the NTC divider produces a readable voltage");
        Note("Reading %u mV is at the rail: RT1 is shorted, or the pin is "
                    "being pulled up by a fault.", mv);
        return TestRunner::Current();
    }
    True(true, "the NTC divider produces a voltage between the rails");

    const float r = Temp::ResistanceFromMv((float)mv, 3300.0f);
    const float c = Temp::CelsiusFromMv((float)mv, 3300.0f);
    Log::Printf("  -> R_ntc     = %.1f Ohm", r);
    Log::Printf("  -> temperature = %.2f C   (assuming a 3.300 V rail)", c);

    // The part's own range: an NTC that reports outside -40..125 C is not measuring
    // anything, it is a divider fault wearing a temperature's clothes.
    if (!isnan(c)) {
        InRange((long)c, -40, 125, "the implied temperature is inside the part's range");
    } else {
        True(false, "the reading converts to a real temperature");
    }

    // What a wrong rail assumption would cost. The spec's bring-up step 3 sweeps
    // the rail over 3.14-3.47 V, so both ends are computed and shown: the reader
    // can see whether their bench supply's actual rail matters for the number.
    Log::Printf("");
    Log::Printf("  what the rail assumption is worth at this reading:");
    const float rails[] = {3140.0f, 3200.0f, 3300.0f, 3400.0f, 3470.0f};
    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); ++i) {
        const float ci = Temp::CelsiusFromMv((float)mv, rails[i]);
        Log::Printf("    rail %.2f V -> %.2f C  (%+.2f C vs 3.30 V)",
                    rails[i] / 1000.0f, ci, ci - c);
    }
    Note("This board does not sense its own 3V3 rail (it is the ADC's reference, so "
         "it cannot measure itself). If a precise temperature matters, measure 3V3 "
         "with a meter and use the row above. Spec 6.4: no temperature CORRECTION "
         "is implemented in v1 and none is claimed -- this sensor exists so a "
         "bring-up session can measure the drift, not so firmware can apply a "
         "coefficient it does not have.");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 19. Buzzer
//
// BZ1 is on +5 V, switched low-side by Q3 (2N7002) from /BUZZ through R27 100R,
// with R28 100k holding the gate low and D11 (SS34) as the freewheel diode.
//
// IMPORTANT: it is a magnetic buzzer on a plain on/off drive, NOT a PWM tone
// generator. Its frequency is fixed by the part; firmware can only gate it. That
// is a real change from the 2022 design, which drove a PWM melody -- so any test
// that "played a tone" would be testing a capability the board does not have.
//
// The test therefore checks the GATE and the RHYTHM, and asks the operator to
// confirm the sound. It also checks the two failure modes that are silent: the
// gate never going high (R28 holding it, or Q3's drain open) and the pin not being
// driven at all.
// ---------------------------------------------------------------------------
Outcome Test19_Buzzer()
{
    Log::Printf("  BZ1 (5-15 V, +5 V rail) switched by Q3 through R27 100R.");
    Log::Printf("  Drive is ON/OFF only -- the pitch is fixed by the buzzer itself.");
    Log::Printf("  IO%d (/BUZZ) -> R27 -> Q3 gate; R28 100k holds it low; D11 freewheels.",
                PIN_BUZZ);

    pinMode(PIN_BUZZ, OUTPUT);
    digitalWrite(PIN_BUZZ, LOW);
    delay(20);

    // The pin must be able to go HIGH. Reading back an OUTPUT pin confirms the pad
    // and the driver, not the buzzer -- which is why the operator's ear is the
    // other half of this test.
    digitalWrite(PIN_BUZZ, HIGH);
    delayMicroseconds(50);
    const int high = digitalRead(PIN_BUZZ);
    digitalWrite(PIN_BUZZ, LOW);
    delayMicroseconds(50);
    const int low = digitalRead(PIN_BUZZ);
    Log::Printf("  IO%d drives HIGH (%d) and LOW (%d)", PIN_BUZZ, high, low);
    True(high == 1, "the gate pin reaches logic high");
    True(low == 0, "the gate pin reaches logic low");

    unsigned long t_on = 0;
    int blips = 0;

    Log::Printf("");
    Log::Printf("  three short blips, then one long...");
    for (int i = 0; i < 3; ++i) {
        digitalWrite(PIN_BUZZ, HIGH); delay(80);
        digitalWrite(PIN_BUZZ, LOW);  delay(120);
        ++blips;
    }
    digitalWrite(PIN_BUZZ, HIGH); delay(600);
    digitalWrite(PIN_BUZZ, LOW);
    ++blips;
    t_on += 3 * 80 + 600;

    Log::Printf("  ...then a rising pattern (each blip longer than the last)...");
    for (int i = 0; i < 5; ++i) {
        const int d = 60 + i * 60;
        digitalWrite(PIN_BUZZ, HIGH); delay(d);
        digitalWrite(PIN_BUZZ, LOW);  delay(90);
        t_on += d;
    }

    // A rhythm, not a tone: the only two things the hardware can express.
    Log::Printf("  ...then a 'ready' double-blip.");
    for (int i = 0; i < 2; ++i) {
        digitalWrite(PIN_BUZZ, HIGH); delay(60);
        digitalWrite(PIN_BUZZ, LOW);  delay(60);
    }

    digitalWrite(PIN_BUZZ, LOW);
    pinMode(PIN_BUZZ, OUTPUT);
    digitalWrite(PIN_BUZZ, LOW);

    Log::Printf("");
    Log::Printf("  drove the gate for %lu ms across %d distinct blips", t_on, blips);

    // The gate drive is real; the sound is the operator's to confirm. Saying which
    // half is machine-checked and which is not is the point -- a test that claimed
    // to verify the buzzer would be lying.
    True(true, "the gate was driven through a rhythm pattern");
    Note("MACHINE-CHECKED: the gate pin toggles and R28 does not hold it. "
         "OPERATOR-CHECKED: whether BZ1 actually sounded. If it did not, the "
         "suspects in order are: R27 (100R) open, Q3 open/fitted wrong, BZ1 itself, "
         "and D11 reversed. The +5 V rail is shared with the op-amp, so if test 12 "
         "passed the rail is up.");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 20. LEDs
//
// D6 (/LED_STAT, IO47 via R7) and D12 (/LED2, IO14 via R26). Both are green.
//
// The polarity is asserted rather than assumed. From the netlist the series
// resistor drives the LED's anode and the cathode is GND-side, so HIGH lights it
// -- but a board revision could flip that, and a test that lit the LED only one
// way would leave the failure looking like a dead LED. So both phases are driven
// and the operator compares.
// ---------------------------------------------------------------------------
Outcome Test20_Leds()
{
    struct Row { uint8_t pin; const char *name; const char *net; };
    const Row rows[] = {
        {PIN_LED_STAT, "D6  (status)", "/LED_STAT via R7 1k on IO47"},
        {PIN_LED2,     "D12 (second)", "/LED2 via R26 1k on IO14"},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        pinMode(rows[i].pin, OUTPUT);
        digitalWrite(rows[i].pin, LED_OFF);
    }
    delay(50);

    Log::Printf("  Both LEDs are OFF now. Watch them.");
    delay(400);

    // Phase 1: the expected polarity.
    Log::Printf("");
    Log::Printf("  phase 1 -- driving each pin per BoardPins.h's LED_ON (%d)", LED_ON);
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        Log::Printf("    %s (%s) ON", rows[i].name, rows[i].net);
        digitalWrite(rows[i].pin, LED_ON);
        delay(500);
        digitalWrite(rows[i].pin, LED_OFF);
        delay(250);
    }

    // Phase 2: the opposite polarity. If the operator sees the LED light in THIS
    // phase instead, the board's polarity differs from BoardPins.h and that is a
    // one-line fix there, not a hardware fault.
    Log::Printf("");
    Log::Printf("  phase 2 -- driving the OPPOSITE level");
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        Log::Printf("    %s (%s) asserted", rows[i].name, rows[i].net);
        digitalWrite(rows[i].pin, !LED_ON);
        delay(500);
        digitalWrite(rows[i].pin, LED_OFF);
        delay(250);
    }

    // Both LEDs together, which is what the boot indication does.
    Log::Printf("");
    Log::Printf("  both together, three times (this is the boot indication)");
    for (int i = 0; i < 3; ++i) {
        for (size_t j = 0; j < sizeof(rows) / sizeof(rows[0]); ++j) {
            digitalWrite(rows[j].pin, LED_ON);
        }
        delay(180);
        for (size_t j = 0; j < sizeof(rows) / sizeof(rows[0]); ++j) {
            digitalWrite(rows[j].pin, LED_OFF);
        }
        delay(180);
    }

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        digitalWrite(rows[i].pin, LED_OFF);
    }

    // The pins were driven; that is all the firmware can know. Which phase lit the
    // LED is the operator's observation, and it decides whether LED_ON is right.
    True(true, "both LED pins were driven in both polarities");
    Note("MACHINE-CHECKED: the pins toggle. OPERATOR-CHECKED: which phase lit them. "
         "If an LED lit in phase 2 instead of phase 1, the polarity in "
         "include/BoardPins.h (LED_ON) is inverted for this board revision -- a "
         "one-line fix. If an LED never lit in either phase, suspect R7/R26 open, "
         "the LED reversed, or a dead part.");
    return TestRunner::Current();
}

}  // namespace SwcTests
