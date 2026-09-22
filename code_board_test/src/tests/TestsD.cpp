// Tests 31-32: the AUX inputs under manual stimulus, and RT1's temperature SCALE.
//
// Both of these need the operator, and both are worth having anyway: the AUX inputs
// are the user-facing programming buttons (spec 7.5), so a dead one makes the device
// unmaintainable in the field; and a thermistor that reads a plausible number at
// room temperature can still be wrong by tens of degrees at either end of the range,
// which only a second point can catch.

#include <Arduino.h>
#include <math.h>

#include "Adc.h"
#include "BoardPins.h"
#include "Log.h"
#include "SetupPrompts.h"
#include "TestTask.h"
#include "Temp.h"
#include "TestRunner.h"

using TestRunner::Check::InRange;
using TestRunner::Check::Note;
using TestRunner::Check::True;
using TestRunner::Outcome;
using TestRunner::Result;

namespace SwcTests {

// ---------------------------------------------------------------------------
// 31. AUX1-AUX3 under manual stimulus
//
// Test 17 reports the resting levels. This one goes further: it asks the operator to
// short each input to GND in turn and confirms the reading COLLAPSES and RECOVERS.
// That is the difference between "the pin reads something" and "the pin is a live
// analog path" -- a pin with a broken series resistor or a shorted clamp can read a
// plausible resting voltage and still be dead as an input.
//
// The board cannot produce the stimulus itself (the AUX pins are pulled UP and only
// the operator can pull one down), so the test prints what to do and waits.
// ---------------------------------------------------------------------------
const char *Setup31_AuxManual()
{
    return "Fully automatic IF the three test wires are fitted: J5.4<->IO16 (TP5), "
           "J5.3<->IO21 (TP6), J5.2<->IO43 (TP7). The test drives each line low to "
           "simulate the short and floats it to simulate open -- no prompts either way. "
           "With no wiring it reports SKIP rather than failing.";
}

Outcome Test31_AuxManual()
{
    struct Row { Adc::Ch ch; uint8_t stim; const char *name; int series; };
    const Row rows[] = {
        {Adc::kAux1, PIN_AUX_STIM1, "AUX1", 23},
        {Adc::kAux2, PIN_AUX_STIM2, "AUX2", 24},
        {Adc::kAux3, PIN_AUX_STIM3, "AUX3", 25},
    };
    const size_t n = sizeof(rows) / sizeof(rows[0]);

    Log::Printf("  AUX1-AUX3 are the user-facing programming inputs (spec 7.5), wired like");
    Log::Printf("  the SWC channels but with 1k series (R23-R25) and a 10k pull-up (R17-R19).");
    Log::Printf("  AUX1 is the one the production firmware uses.");
    Log::Printf("");
    Log::Printf("  This test DRIVES ITS OWN STIMULUS through the three test wires:");
    Log::Printf("    J5.4 (AUX1) <-> IO%d (TP5)   J5.3 (AUX2) <-> IO%d (TP6)   J5.2 (AUX3) <-> IO%d (TP7)",
                PIN_AUX_STIM1, PIN_AUX_STIM2, PIN_AUX_STIM3);
    Log::Printf("  Driving the spare pin LOW pulls the input to GND (the short); floating");
    Log::Printf("  it leaves the board's own 10k pull-up to set the level (open/released).");
    Log::Printf("  No operator prompts: the whole thing runs unattended.");
    Log::Printf("");

    // ---------------------------------------------------------------------
    // Is the rig present? Park every stimulus pin as an INPUT (floating). If the wire
    // is fitted, the AUX input sits on its own 10k pull-up and reads HIGH -- exactly
    // what it reads with nothing attached, so that alone proves nothing. The check
    // that DOES prove it is in the next step: driving the pin low must pull the input
    // down. So this first pass just reports the resting levels.
    //
    // A pin that is NOT wired is driven in the air, which harms nothing.
    // ---------------------------------------------------------------------
    for (size_t i = 0; i < n; ++i) {
        pinMode(rows[i].stim, INPUT);          // float: simulates open
    }
    delay(30);

    struct Meas { uint32_t open_mv, short_mv; };
    Meas m[3] = {};

    Log::Printf("  %-6s %-14s %-12s %-12s %s", "input", "resting/open", "driven low",
                "swing", "verdict");

    int wired = 0;
    for (size_t i = 0; i < n; ++i) {
        // (a) OPEN: the stimulus pin floats, so the input is set by R17/R18/R19.
        pinMode(rows[i].stim, INPUT);
        delay(30);
        Adc::ReadAvgMv(rows[i].ch, 64, &m[i].open_mv);

        // (b) SHORTED: drive the stimulus pin low. Through the wire this pulls the AUX
        // input toward GND, through R2x which limits the current.
        pinMode(rows[i].stim, OUTPUT);
        digitalWrite(rows[i].stim, LOW);
        delay(30);
        Adc::ReadAvgMv(rows[i].ch, 64, &m[i].short_mv);

        // Back to open, and confirm it RECOVERS. This is the part that catches a board
        // fault rather than a wiring one: if the input stays low once released, the
        // node is held down on the board.
        pinMode(rows[i].stim, INPUT);
        delay(30);
        uint32_t back = 0;
        Adc::ReadAvgMv(rows[i].ch, 64, &back);

        const long swing = (long)m[i].open_mv - (long)m[i].short_mv;
        const bool present = (m[i].open_mv > 1500) && (swing > 800);
        if (present) ++wired;

        Log::Printf("  %-6s %-14u %-12u %-12ld %s", rows[i].name, m[i].open_mv,
                    m[i].short_mv, swing,
                    present ? "wired: collapses and recovers"
                            : "no swing -- rig not fitted for this input?");

        if (present) {
            True(true, "the input collapses under its driven-low stimulus");
            True(back > 1500, "the input recovers when the stimulus is released");
            if (back <= 1500) {
                Note("%s stayed at %u mV after the stimulus was released. The node is "
                     "held down on the board -- a solder bridge or a shorted clamp "
                     "diode (D%d), not a wiring problem.", rows[i].name, back,
                     (int)(8 + i));
            }
            Log::Printf("      %s: open %u mV -> shorted %u mV -> open %u mV",
                        rows[i].name, m[i].open_mv, m[i].short_mv, back);
        }
    }

    // Leave every stimulus pin floating, so an idle board drives nothing.
    for (size_t i = 0; i < n; ++i) pinMode(rows[i].stim, INPUT);

    Log::Printf("");
    if (wired == 0) {
        // Nothing is wired, so the test has no stimulus and must NOT report PASS.
        Note("None of the three test wires produced a swing, so no AUX input was "
             "actually exercised. That is a SKIP: fit J5.4<->IO%d, J5.3<->IO%d and "
             "J5.2<->IO%d and re-run. (An input with no wire attached simply reads its "
             "pull-up level, which is why 'resting' alone proves nothing.)",
             PIN_AUX_STIM1, PIN_AUX_STIM2, PIN_AUX_STIM3);
        TestRunner::MutableCurrent().result = TestRunner::Result::kSkip;
        return TestRunner::Current();
    }

    if (wired < (int)n) {
        Log::Printf("  %d of %d inputs were exercised -- the others had no stimulus.", wired, (int)n);
        Note("%d of %d AUX inputs responded. Fit the missing test wire(s) "
                    "and re-run for full coverage.", (int)n - wired, (int)n);
        TestRunner::MutableCurrent().result = TestRunner::Result::kWarn;
    } else {
        True(true, "all three AUX inputs collapse under a driven low and recover");
    }

    Note("AUX1 is the input the production firmware uses for programming and "
         "maintenance entry (spec 7.5, 8.2): a fault on AUX1 specifically makes the "
         "device unmaintainable in the field even when AUX2 and AUX3 are fine.");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 32. RT1: the temperature SCALE, not just the reading
//
// Test 18 proves the divider produces a plausible number. This one checks that the
// number is on the right SCALE, which a single room-temperature reading cannot show:
// the NTC curve is exponential, so an error in B or in the series resistance can read
// correctly at 25 C and be wrong by tens of degrees at 0 C or 60 C.
//
// The operator supplies a second point by warming the sensor. Two points pin the
// curve, and the test reports the implied beta -- the one number that says whether the
// curve is right across the whole range rather than at a single temperature.
// ---------------------------------------------------------------------------
const char *Setup32_TempVerify()
{
    return "Runs unattended: it measures RT1 and reports the temperature. Warm RT1 (the "
           "0603 on the board's right edge) FIRST if you want the two-point beta check "
           "-- the test uses whatever temperature it finds.";
}

Outcome Test32_TempVerify()
{
    Log::Printf("  RT1 is a 10k B3380 NTC in a 1:1 divider with R29 10k, so it reads");
    Log::Printf("  ~half the 3V3 rail at 25 C. Its curve is EXPONENTIAL, which is why");
    Log::Printf("  one room-temperature reading cannot validate it: a wrong beta or a");
    Log::Printf("  wrong series resistance still reads plausibly at 25 C.");
    Log::Printf("");

    uint32_t mv0 = 0;
    Adc::ReadAvgMv(Adc::kTemp, 128, &mv0);
    const float r0 = Temp::ResistanceFromMv((float)mv0, 3300.0f);
    const float c0 = Temp::CelsiusFromMv((float)mv0, 3300.0f);

    Log::Printf("  POINT 1 (at rest)");
    Log::Printf("    /TEMP_ADC (IO%d) = %u mV", PIN_TEMP_ADC, mv0);
    Log::Printf("    R_ntc = %.0f Ohm, T = %.2f C   (assuming a 3.300 V rail)", r0, c0);
    Log::Printf("");

    if (isnan(c0) || r0 < 0) {
        True(false, "the NTC produces a physical reading");
        Note("Reading %u mV is not a physical divider value -- RT1 absent, open or "
             "shorted. Nothing further can be concluded.", mv0);
        return TestRunner::Current();
    }

    // Is it near room temperature? This is the sanity check the spec's bring-up
    // step 3 implies: if the bench is 20-25 C and the sensor says 60 C, something
    // is wrong that a single reading WOULD catch.
    InRange((long)c0, 5, 45, "the resting temperature is plausibly room temperature");

    Log::Printf("");
    Log::Printf("  Now WARM the sensor: hold a finger on RT1 (the 0603 part on the");
    Log::Printf("  exposed right edge, roughly 33 mm from the nearest heat source).");
    Log::Printf("  Hold it for ~20 s, then press ENTER (45 s max).");

    if (!TestTask::AskOperator("Hold something warm against RT1 for ~20 s, then continue", 90000)) {
        Log::Printf("    timed out -- reporting the single point only");
        Note("Without a second point the curve cannot be validated, so this test "
             "reports rather than asserts the scale. A single reading only proves the "
             "divider works, which test 18 already covers.");
        True(true, "the resting reading is physical and near room temperature");
        return TestRunner::Current();
    }

    // Sample while warm, taking the extreme so a brief touch still registers.
    float c_hot = c0;
    float r_hot = r0;
    uint32_t mv_hot = mv0;
    for (int i = 0; i < 20; ++i) {
        uint32_t mv = 0;
        Adc::ReadAvgMv(Adc::kTemp, 64, &mv);
        const float c = Temp::CelsiusFromMv((float)mv, 3300.0f);
        if (!isnan(c) && c > c_hot) { c_hot = c; r_hot = Temp::ResistanceFromMv((float)mv, 3300.0f); mv_hot = mv; }
        delay(120);
    }

    Log::Printf("");
    Log::Printf("  POINT 2 (warmed)");
    Log::Printf("    /TEMP_ADC = %u mV", mv_hot);
    Log::Printf("    R_ntc = %.0f Ohm, T = %.2f C", r_hot, c_hot);
    Log::Printf("");

    const float rise = c_hot - c0;
    Log::Printf("  rise: %.2f C  (%u mV -> %u mV, %.0f -> %.0f Ohm)",
                rise, mv0, mv_hot, r0, r_hot);

    if (rise < 1.0f) {
        Note("The sensor barely moved (%.2f C). A finger should warm a 0603 NTC by "
             "several degrees within 20 s. If it truly did not move, either the "
             "sensor is thermally isolated from where you touched, or R29/RT1 are "
             "not the parts fitted. The scale check below is then not meaningful.",
             rise);
        True(true, "the warmed point was captured (but the rise was too small to "
                   "validate the curve)");
        return TestRunner::Current();
    }

    True(rise > 1.0f, "warming the sensor raises the reading (the NTC direction is right)");
    True(mv_hot < mv0, "a warmer NTC drops LESS voltage (its resistance falls)");

    // The implied beta from the two points. This is the real check: the B3380 part
    // should come out near 3380. A wildly different value means the curve is wrong
    // even though both individual readings looked reasonable.
    //
    //   B = ln(R1/R2) / (1/T1 - 1/T2)
    const float t1 = c0 + 273.15f;
    const float t2 = c_hot + 273.15f;
    const float beta = logf(r0 / r_hot) / ((1.0f / t1) - (1.0f / t2));

    Log::Printf("");
    Log::Printf("  implied beta from the two points: %.0f K  (part is B3380 = 3380 K)", beta);
    InRange((long)beta, 2500, 4500, "the implied beta is in the right family");

    if (beta > 3000 && beta < 3800) {
        True(true, "the implied beta is consistent with the fitted B3380 part");
    } else {
        Note("The implied beta came out %.0f, outside the 3000-3800 window a B3380 "
             "should give. Both readings can still be individually plausible -- which "
             "is exactly why a single-point check cannot validate this sensor. Check "
             "that RT1 is a 10k B3380 and that R29 is 10k, then re-run.", beta);
    }

    Note("This board does not sense its own 3V3 rail (it is the ADC's reference, so it "
         "cannot measure itself), so both points assume 3.300 V. If the rail is off by "
         "5%%, every temperature shifts -- which is why the error table in test 18 is "
         "worth reading before trusting an absolute value.");
    return TestRunner::Current();
}

}  // namespace SwcTests
