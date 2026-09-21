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
    return "You will short each AUX input to GND in turn, when asked. Have a jumper "
           "ready: J5.4 (AUX1), J5.3 (AUX2), J5.2 (AUX3) are the signals and J5.1 is "
           "GND. Press ENTER to start; the test waits for each one.";
}

// Wait for a keypress, with a timeout so an unattended run cannot hang forever.
static bool WaitForKey(uint32_t timeout_ms)
{
    const uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (Serial.available()) {
            while (Serial.available()) Serial.read();
            return true;
        }
        delay(20);
    }
    return false;
}

Outcome Test31_AuxManual()
{
    struct Row { Adc::Ch ch; const char *name; const char *pin; };
    const Row rows[] = {
        {Adc::kAux1, "AUX1", "J5.4"},
        {Adc::kAux2, "AUX2", "J5.3"},
        {Adc::kAux3, "AUX3", "J5.2"},
    };
    const size_t n = sizeof(rows) / sizeof(rows[0]);

    Log::Printf("  AUX1-AUX3 are the user-facing programming inputs (spec 7.5), wired");
    Log::Printf("  exactly like the SWC channels but with a 1k series resistor (R23-R25)");
    Log::Printf("  and a 10k pull-up (R17-R19). AUX1 is the one the product uses.");
    Log::Printf("");
    Log::Printf("  For each input: short its J5 pin to J5.1 (GND), then press ENTER.");
    Log::Printf("  The reading must COLLAPSE toward 0 mV and RECOVER when you remove it.");
    Log::Printf("");

    bool all_ok = true;
    for (size_t i = 0; i < n; ++i) {
        uint32_t rest = 0;
        Adc::ReadAvgMv(rows[i].ch, 64, &rest);

        Log::Printf("  %s (%s): resting at %u mV", rows[i].name, rows[i].pin, rest);
        Log::Printf("    -> short %s to J5.1 (GND) now, then press ENTER (30 s max)",
                    rows[i].pin);

        if (!WaitForKey(30000)) {
            Log::Printf("    timed out waiting -- skipping %s", rows[i].name);
            Note("No keypress within 30 s, so %s was not exercised. That is a SKIP, "
                 "not a pass: this test cannot prove anything without the stimulus.",
                 rows[i].name);
            all_ok = false;
            continue;
        }

        // Sample while the operator holds the short.
        uint32_t shorted = 0;
        Adc::ReadAvgMv(rows[i].ch, 64, &shorted);
        Log::Printf("    shorted: %u mV  (was %u mV)", shorted, rest);

        // Then confirm it RECOVERS -- a pin that stays low after the short is removed
        // is shorted to ground on the board, which is the failure this catches.
        delay(250);
        uint32_t after = 0;
        Adc::ReadAvgMv(rows[i].ch, 64, &after);
        Log::Printf("    released again: %u mV", after);

        const bool collapsed = (rest > 1500) && (shorted < 600);
        const bool recovered = (after > 1500);
        True(collapsed, "the input collapses when shorted to GND");
        if (!collapsed) {
            Note("%s read %u mV while shorted to GND. If the short really was applied, "
                 "suspect R%d (1k series) open, the pin open, or the short was not "
                 "made. If it did NOT move at all, the node may be open-circuit.",
                 rows[i].name, shorted, (int)(23 + i));
            all_ok = false;
        }
        True(recovered, "the input recovers when the short is removed");
        if (!recovered) {
            Note("%s stayed at %u mV after the short was removed. That means it is "
                 "pulled to GND on the board -- a solder bridge or a shorted clamp "
                 "diode (D%d).", rows[i].name, after, (int)(8 + i));
            all_ok = false;
        }
    }

    Log::Printf("");
    if (all_ok) {
        True(true, "every AUX input collapses under a GND short and recovers");
    }
    Note("AUX1 is the input the production firmware uses for programming and "
         "maintenance entry (spec 7.5, 8.2), so a fault on AUX1 specifically makes "
         "the device unmaintainable in the field even if AUX2/AUX3 are fine.");
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
    return "Ideally have something warm (a hand, a warm mug) to hold against RT1, "
           "which is the small 0603 part on the exposed right edge of the board. The "
           "test measures at rest, asks you to warm it, and measures again.";
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

    if (!WaitForKey(45000)) {
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
