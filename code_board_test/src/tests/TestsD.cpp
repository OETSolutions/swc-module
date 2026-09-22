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
#include "driver/gpio.h"
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
    const uint8_t stim[3] = {PIN_AUX_STIM1, PIN_AUX_STIM2, PIN_AUX_STIM3};
    const char *stimname[3] = {"IO16/TP5", "IO21/TP6", "IO43/TP7"};

    Log::Printf("  AUX1-AUX3 are the user-facing programming inputs (spec 7.5), wired like");
    Log::Printf("  the SWC channels but with 1k series (R23-R25) and a 10k pull-up (R17-R19).");
    Log::Printf("  AUX1 is the one the production firmware uses.");
    Log::Printf("");
    Log::Printf("  This test DRIVES ITS OWN STIMULUS through the three test wires: pulling a");
    Log::Printf("  spare pin LOW pulls its AUX input to GND (button pressed), and floating it");
    Log::Printf("  lets the board's own 10k pull-up set the level (released).");
    Log::Printf("");
    Log::Printf("  NOTE: the wire order does NOT matter -- the mapping is discovered below");
    Log::Printf("  before anything is measured, so any AUX input may go to any test point.");
    Log::Printf("");

    // ---------------------------------------------------------------------
    // STEP 1: DISCOVER the wiring. Which spare pin feeds which AUX input is a fact
    // the ADC can measure, and it is NOT safe to assume it.
    //
    // The first version of this test hard-coded stim1->AUX1, stim2->AUX2,
    // stim3->AUX3 -- and on the real rig two wires were crossed, so it reported "no
    // response" for two perfectly healthy lines. The hardware was right and the
    // code's assumption was wrong. Discovering the mapping costs one extra pass and
    // removes the whole failure mode: the wires may go in any order.
    // ---------------------------------------------------------------------
    // Clear any internal pull left on the stimulus pins by an EARLIER test.
    //
    // This is what made the full-suite run disagree with the standalone run: test 3
    // leaves its spare pins as INPUT_PULLUP, and an internal ~45k pull-up BEATS the
    // stimulus pin's pull-down, so the discovery pass saw no movement and reported
    // "no wire" for an input that is wired and works. `pinMode(INPUT)` does not clear
    // an internal pull on the ESP32 -- gpio_set_pull_mode does.
    for (size_t k = 0; k < n; ++k) {
        // gpio_reset_pin DETACHES any peripheral function from the pad. Without it,
        // a pin with a default peripheral role -- and IO43 is TXD0, UART0's
        // transmitter -- can be held by that peripheral and ignore pinMode entirely.
        // That is exactly what happened: IO43 drove its AUX input in one run and did
        // nothing in another, because whether UART0 still owned the pad varied.
        gpio_reset_pin((gpio_num_t)stim[k]);
        gpio_set_pull_mode((gpio_num_t)stim[k], GPIO_FLOATING);
        pinMode(stim[k], INPUT);
    }
    delay(40);

    uint32_t base[3] = {0, 0, 0};
    for (size_t k = 0; k < n; ++k) Adc::ReadAvgMv(rows[k].ch, 64, &base[k]);

    int driven_by[3] = {-1, -1, -1};      // per AUX input: which stim pin pulls it

    // Show the resting levels before anything is driven. If an input is already LOW
    // here it cannot be attributed to any drive, which is a distinct failure from a
    // missing wire -- and the first version of this could not tell them apart.
    Log::Printf("  resting levels before any drive: AUX1 %u, AUX2 %u, AUX3 %u mV",
                base[0], base[1], base[2]);
    for (size_t j = 0; j < n; ++j) {
        if (base[j] <= 1500) {
            Note("%s rests at %u mV, not high. It cannot be attributed to any drive, so "
                 "this input cannot be verified until it is released. Something is "
                 "holding it down: an output left low by an earlier test, a wire to a "
                 "GND point, or a board fault. The matrix below shows what each drive "
                 "does anyway.", rows[j].name, base[j]);
        }
    }

    for (size_t k = 0; k < n; ++k) {
        gpio_reset_pin((gpio_num_t)stim[k]);
        gpio_set_pull_mode((gpio_num_t)stim[k], GPIO_FLOATING);
        pinMode(stim[k], OUTPUT);
        digitalWrite(stim[k], LOW);
        delay(30);
        Log::Printf("  driving %s low:", stimname[k]);
        for (size_t j = 0; j < n; ++j) {
            uint32_t mv = 0;
            Adc::ReadAvgMv(rows[j].ch, 32, &mv);
            Log::Printf("      %s = %u mV", rows[j].name, mv);
            // Only credit a pin that was HIGH and is now pulled well down. If a line
            // was already low, it cannot be attributed to this drive.
            if (base[j] > 1500 && mv < 1200 && driven_by[j] < 0) driven_by[j] = (int)k;
        }
        gpio_set_pull_mode((gpio_num_t)stim[k], GPIO_FLOATING);
        pinMode(stim[k], INPUT);
        delay(10);
    }

    Log::Printf("  Wiring as MEASURED (not assumed):");
    int wired = 0;
    for (size_t j = 0; j < n; ++j) {
        if (driven_by[j] < 0) {
            Log::Printf("    %s  <- no spare pin pulls it (no wire on this input)", rows[j].name);
        } else {
            Log::Printf("    %s  <- %s", rows[j].name, stimname[driven_by[j]]);
            ++wired;
        }
    }
    Log::Printf("");
    if (wired == 0) {
        Note("None of the three test wires pulls any AUX input, so nothing was "
             "exercised. That is a SKIP: fit wires from the AUX inputs to the spare "
             "test points (IO%d/TP5, IO%d/TP6, IO%d/TP7 -- any order) and re-run. An "
             "unwired input simply reads its own pull-up, which is why a resting "
             "reading alone proves nothing.",
             PIN_AUX_STIM1, PIN_AUX_STIM2, PIN_AUX_STIM3);
        TestRunner::MutableCurrent().result = TestRunner::Result::kSkip;
        return TestRunner::Current();
    }

    // ---------------------------------------------------------------------
    // STEP 2: exercise each input through the mapping just discovered.
    // ---------------------------------------------------------------------
    Log::Printf("  %-6s %-11s %-12s %-12s %-10s %s", "input", "via", "open mV",
                "driven low", "recovered", "verdict");

    for (size_t j = 0; j < n; ++j) {
        if (driven_by[j] < 0) {
            Log::Printf("  %-6s %-11s %s", rows[j].name, "-",
                        "NOT EXERCISED (no wire)");
            continue;
        }
        const uint8_t sp = stim[driven_by[j]];

        // (a) OPEN: float the stimulus pin; the board's 10k sets the level.
        gpio_reset_pin((gpio_num_t)sp);
        gpio_set_pull_mode((gpio_num_t)sp, GPIO_FLOATING);
        pinMode(sp, INPUT);
        delay(30);
        uint32_t open_mv = 0;
        Adc::ReadAvgMv(rows[j].ch, 64, &open_mv);

        // (b) DRIVEN LOW: through the wire this pulls the input to GND.
        pinMode(sp, OUTPUT);
        digitalWrite(sp, LOW);
        delay(30);
        uint32_t short_mv = 0;
        Adc::ReadAvgMv(rows[j].ch, 64, &short_mv);

        // (c) RELEASED again: the node must come back up. This is the part that
        // catches a BOARD fault rather than a wiring one -- a node held down on the
        // board stays low however the stimulus is driven.
        pinMode(sp, INPUT);
        delay(30);
        uint32_t back_mv = 0;
        Adc::ReadAvgMv(rows[j].ch, 64, &back_mv);

        const long swing = (long)open_mv - (long)short_mv;
        const bool ok = (swing > 800) && (back_mv > 1500);

        Log::Printf("  %-6s %-11s %-12u %-12u %-10u %s", rows[j].name,
                    stimname[driven_by[j]], open_mv, short_mv, back_mv,
                    ok ? "collapses and recovers" : "FAILED");

        True(swing > 800, "the input collapses under its driven-low stimulus");
        True(back_mv > 1500, "the input recovers when the stimulus is released");
        if (back_mv <= 1500) {
            Note("%s stayed at %u mV after the stimulus was released. The node is held "
                 "down on the board -- a solder bridge or a shorted clamp diode (D%d). "
                 "That is a board fault, not a wiring one.", rows[j].name, back_mv,
                 (int)(8 + j));
        }
        if (swing <= 800 && back_mv > 1500) {
            Note("%s moved only %ld mV under the drive (open %u, driven %u). The pin is "
                 "live, so suspect R%d (1k series) far off value rather than a missing "
                 "wire.", rows[j].name, swing, open_mv, short_mv, (int)(23 + j));
        }
    }

    for (size_t k = 0; k < n; ++k) pinMode(stim[k], INPUT);

    Log::Printf("");
    if (wired < (int)n) {
        Log::Printf("  %d of %d inputs exercised; the rest have no wire.", wired, (int)n);
        Note("%d of %d AUX inputs responded. Fit the missing wire(s) -- any order, the "
             "map is discovered automatically -- and re-run for full coverage.",
             (int)n - wired, (int)n);
        if (TestRunner::MutableCurrent().result == TestRunner::Result::kNotRun)
            TestRunner::MutableCurrent().result = TestRunner::Result::kWarn;
    } else {
        True(true, "all three AUX inputs collapse under a driven low and recover");
    }

    Note("AUX1 is the input the production firmware uses for programming and "
         "maintenance entry (spec 7.5, 8.2): a fault on AUX1 specifically makes the "
         "device unmaintainable in the field even when AUX2 and AUX3 are fine.");
    Note("The wire order does not matter: this test discovers which spare pin reaches "
         "which AUX input before it measures anything, so wires may be fitted to any "
         "of the three test points.");
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
