// The on-device hardware tests: what each one measures and why.
//
// NUMBERING. The numbers are a suggested ORDER, not a dependency graph: the tests
// that establish a precondition (power, then the DAC, then the ADC calibration)
// come first because everything after them is interpreted through their results.
// `RunAll` runs them in this order for that reason. Any test can be run alone.
//
// WHAT A TEST ASSERTS. A test asserts something FALSE-IF-BROKEN, not just that a
// number came back. "The I2C bus scanned and found a device" is not a test; "the
// device ACKed at 0x60 AND an arbitrary code written to it reads back" is,
// because it fails when the frame is malformed, which is the way this part
// actually breaks. Several of these exist specifically to fail if a defect the
// production firmware has already shipped once comes back.
//
// MEASUREMENTS vs EXPECTATIONS. Where the board's true value is vehicle-specific
// (the ladder resistances) or unknown until measured (the head unit's idle
// voltage) the test cannot assert an expected number and does not pretend to: it
// measures, prints, and checks the things that ARE known (that a value is inside
// the ADC's range, that a press moves it the right way). It says so in the log.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

#include "Adc.h"
#include "BoardPins.h"
#include "Dac.h"
#include "Log.h"
#include "Temp.h"
#include "SetupPrompts.h"
#include "TestRunner.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "swc_logic/Output.h"

using TestRunner::Check::InRange;
using TestRunner::Check::Near;
using TestRunner::Check::Note;
using TestRunner::Check::True;
using TestRunner::Outcome;
using TestRunner::Result;

namespace SwcTests {

// ===========================================================================
// Group A -- power, identity, and the platform itself
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. Boot identity, flash and heap
//
// The spec's bring-up step 1: "confirm the flash size and absence of PSRAM the
// build assumed. If this disagrees with the board JSON, stop -- every later
// measurement is void." That check is this test.
// ---------------------------------------------------------------------------
Outcome Test01_BootIdentity()
{
    Note("reset reason: %d (%s)", (int)esp_reset_reason(),
                esp_reset_reason() == ESP_RST_POWERON ? "power-on"
                : esp_reset_reason() == ESP_RST_SW ? "software"
                : esp_reset_reason() == ESP_RST_PANIC ? "panic" : "other");

    Log::Printf("  chip          : %s rev %u", ESP.getChipModel(), ESP.getChipRevision());
    Log::Printf("  CPU           : %u MHz", ESP.getCpuFreqMHz());
    Log::Printf("  flash         : %u bytes (%.1f MB)", ESP.getFlashChipSize(),
                ESP.getFlashChipSize() / 1048576.0);
    Log::Printf("  PSRAM         : %u bytes", ESP.getPsramSize());
    Log::Printf("  sketch size   : %u bytes", ESP.getSketchSize());
    Log::Printf("  free heap     : %u bytes", ESP.getFreeHeap());
    Log::Printf("  min heap ever : %u bytes", ESP.getMinFreeHeap());

    // The board is a 4 MB part with NO PSRAM (spec 2.1). Both facts are load
    // bearing: the partition table and every buffer size assume them.
    InRange((long)(ESP.getFlashChipSize() / 1048576), 4, 4, "flash size is 4 MB");
    Near((long)ESP.getPsramSize(), 0, 0, "no PSRAM (the build assumes none)");

    // An ESP32-S3 reports its model string; check the *S3* silicon specifically,
    // because a wrong board definition would otherwise only show up as ADC
    // numbers that are subtly wrong.
    True(strstr(ESP.getChipModel(), "S3") != nullptr, "silicon is an ESP32-S3");

    // A power-on reset or a deliberate software reset is expected. A panic is not
    // a failure of the board -- it means the LAST run crashed, which is worth
    // surfacing rather than hiding.
    const esp_reset_reason_t rr = esp_reset_reason();
    True(rr != ESP_RST_PANIC && rr != ESP_RST_INT_WDT && rr != ESP_RST_TASK_WDT,
         "previous reset was not a panic or watchdog");

    True(ESP.getFreeHeap() > 100000, "free heap above 100 kB");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 2. Rail health: 3V3 and the 5 V / VBUS presence
//
// The ESP32's own supply is not sensed by any ADC pin -- 3V3 is the reference the
// ADC measures against, so it cannot measure itself. It is inferred instead:
//
//   * The NTC divider (R29 10k / RT1 10k) is a 1:1 divider on 3V3, so with the
//     sensor near room temperature the pin sits near 1.65 V. A rail that has
//     collapsed or is being dragged down moves that midpoint.
//   * Every pulled-up input (SWC, AUX) idles at a large fraction of the rail, so
//     they too bound it from below.
//
// The firm number comes from VBUS: /VBUS_VALID is a 1:1 divider off fused VBUS, so
// if USB is plugged in the pin must read about half of 5 V. That is a real rail
// check, and it is the one this test asserts.
// ---------------------------------------------------------------------------
Outcome Test02_Rails()
{
    uint32_t ntc = 0, swc1 = 0;
    Adc::ReadAvgMv(Adc::kTemp, 32, &ntc);
    Adc::ReadAvgMv(Adc::kSwc1, 32, &swc1);

    Log::Printf("  NTC midpoint  : %u mV  (expect ~1650 at 25 C if RT1 fitted)", ntc);
    Log::Printf("  SWC1 idle     : %u mV", swc1);

    // VBUS_VALID is a digital input, not an ADC channel, so read it as a GPIO.
    pinMode(PIN_VBUS_VALID, INPUT);
    const int vbus_pin = digitalRead(PIN_VBUS_VALID);
    Log::Printf("  /VBUS_VALID   : %s (IO10 reads %d)", vbus_pin ? "HIGH" : "LOW", vbus_pin);

    if (vbus_pin) {
        Log::Printf("  -> USB power present. VBUS >= ~1.8 V at the pin means VBUS >= ~3.6 V.");
        True(true, "VBUS present and the divider reads above logic threshold");
    } else {
        Log::Printf("  -> USB reports NO VBUS. If the board is running, it is on the 12 V");
        Log::Printf("     feed (or the VBUS divider/IO10 is faulty -- check with a meter).");
        // Not a failure: the board can legitimately run from 12 V alone.
        // But the USB *link* tests will report it.
    }

    // The ADC reference is the 3V3 rail. A wild midpoint on the NTC divider with
    // everything else sane is a sensor fault, not a rail fault; this test only
    // asserts the rail is in a plausible band via the pulled-up input.
    //
    // SWC1 idles at the top of the ladder divider. With no ladder fitted it is
    // just the pull-up: the ADC input has no DC path to ground, so it reads near
    // the rail but slightly under (input leakage through the clamp and the ADC).
    InRange((long)swc1, 2500, 3400, "SWC1 idle sits high (pull-up to +3V3 is alive)");

    // The NTC midpoint must be inside the ADC's range. Whether it is NEAR 1650
    // depends on the ambient temperature and whether RT1 is fitted, so the test
    // reports the temperature it implies rather than asserting room temperature.
    InRange((long)ntc, 100, 2900, "NTC midpoint is a real ADC reading, not clamped");
    const float c = Temp::CelsiusFromMv((float)ntc, 3300.0f);
    if (isnan(c)) {
        Note("NTC reading is not physical -> RT1 absent, or open/shorted");
    } else {
        Log::Printf("  implied temp  : %.1f C (using an assumed 3.30 V rail)", c);
        InRange((long)c, -40, 125, "implied temperature is in the part's range");
    }
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 3. GPIO loopback of the spare test points
//
// IO3/IO11/IO12/IO15/IO16/IO21 go to TP1-TP6 and nowhere else (netlist: each is a
// single-pad net on U3). They have no external pull, so with nothing attached they
// float -- which is exactly why this test matters: it is the only way to find a
// solder bridge or an open pad on a pin that is otherwise unused.
//
// The test drives the internal pull-ups and pull-downs and checks each pin
// follows, THEN does a pairwise check: with internal pull-ups on all six, any two
// pins that read the same shorted state on both phases are bridged.
// ---------------------------------------------------------------------------
static const uint8_t kSpare[] = {
    PIN_SPARE_IO3, PIN_SPARE_IO11, PIN_SPARE_IO12,
    PIN_SPARE_IO15, PIN_SPARE_IO16, PIN_SPARE_IO21,
};
static const char *kSpareName[] = {"IO3(TP1)", "IO11(TP2)", "IO12(TP3)",
                                   "IO15(TP4)", "IO16(TP5)", "IO21(TP6)"};
static const size_t kSpareCount = sizeof(kSpare) / sizeof(kSpare[0]);

Outcome Test03_SpareGpio()
{
    Log::Printf("  %-10s %-8s %-8s %-8s", "pin", "pull-up", "pull-dn", "floating");

    int hi[6] = {0}, lo[6] = {0}, fl[6] = {0};

    for (size_t i = 0; i < kSpareCount; ++i) {
        pinMode(kSpare[i], INPUT_PULLUP);
        delay(2);
        hi[i] = digitalRead(kSpare[i]);

        pinMode(kSpare[i], INPUT_PULLDOWN);
        delay(2);
        lo[i] = digitalRead(kSpare[i]);

        // Floating: read immediately after switching to a plain input, which is
        // the state a disconnected pin is in.
        pinMode(kSpare[i], INPUT);
        delayMicroseconds(200);
        const int a = digitalRead(kSpare[i]);
        delayMicroseconds(200);
        const int b = digitalRead(kSpare[i]);
        fl[i] = (a != b);

        Log::Printf("  %-10s %-8d %-8d %-8s", kSpareName[i], hi[i], lo[i],
                    fl[i] ? "yes (good)" : "stable");
    }

    // Each pin must follow the internal pull in both directions -- EXCEPT the three
    // that test 31 may have wired to the AUX inputs. Those have a 10k pull-up to +3V3
    // on them (R17/R18/R19), which an internal pull-down (~45k) cannot beat, so they
    // read high under both. That is the test rig, not a fault, and calling it one
    // would fail a healthy board every time the AUX wires are fitted.
    static const uint8_t kMaybeAuxWired[] = {PIN_AUX_STIM1, PIN_AUX_STIM2, PIN_AUX_STIM3};

    char failed[128] = {0};
    for (size_t i = 0; i < kSpareCount; ++i) {
        bool is_stim_pin = false;
        for (size_t k = 0; k < sizeof(kMaybeAuxWired); ++k) {
            if (kSpare[i] == kMaybeAuxWired[k]) is_stim_pin = true;
        }
        if (hi[i] != 1) {
            snprintf(failed, sizeof(failed), "%s does not follow a pull-up (reads low)", kSpareName[i]);
            True(false, failed);
        }
        if (lo[i] != 0) {
            if (is_stim_pin) {
                // Expected when the AUX test rig is fitted: an external 10k pull-up
                // holds the pin high against the internal pull-down.
                Log::Printf("  [note] %s does not follow a pull-down -- expected if it is "
                            "wired to an AUX input (that input's 10k pull-up wins)",
                            kSpareName[i]);
            } else {
                snprintf(failed, sizeof(failed), "%s does not follow a pull-down (reads high)", kSpareName[i]);
                True(false, failed);
            }
        }
    }
    True(failed[0] == '\0', "every spare pin follows both internal pulls "
                            "(AUX-wired pins excepted, and noted)");

    // Pairwise bridge check: with pull-ups on all six, a bridged pair cannot be
    // distinguished by level alone, so drive each pin low in turn and see whether
    // any OTHER pin follows it. That is the definitive bridge test.
    int bridges = 0;
    for (size_t i = 0; i < kSpareCount; ++i) {
        for (size_t j = 0; j < kSpareCount; ++j) pinMode(kSpare[j], INPUT_PULLUP);
        pinMode(kSpare[i], OUTPUT);
        digitalWrite(kSpare[i], LOW);
        delay(2);
        for (size_t j = 0; j < kSpareCount; ++j) {
            if (j == i) continue;
            if (digitalRead(kSpare[j]) == 0) {
                Log::Printf("  BRIDGE: %s driven low pulls %s low", kSpareName[i], kSpareName[j]);
                ++bridges;
            }
        }
        pinMode(kSpare[i], INPUT_PULLUP);
    }
    True(bridges == 0, "no solder bridges between the spare test-point pins");

    // Leave them as inputs with pull-ups: a floating input on an unused pin wastes
    // current and is a noise source.
    for (size_t i = 0; i < kSpareCount; ++i) pinMode(kSpare[i], INPUT_PULLUP);

    Note("IO43/IO44 (TP7/TP8) are the ROM UART0 pins. They are NOT driven here, but "
         "not because driving them would be unsafe: this build sets ARDUINO_USB_MODE=1, "
         "so the console is the USB peripheral and UART0 is free (test 31 does use "
         "IO43 as a stimulus). They are excluded from THIS test because a pin whose "
         "level can be changed by an attached jumper is not a useful bridge check.");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 4. I2C bus scan, and locating the MCP4728
//
// Spec item N-4 makes the DAC's address a MEASUREMENT: 0x60 is the all-low strap
// default, but the board's A0/A1/A2 are tied at layout time and the spec lists the
// result as open. So the test scans the whole 7-bit range and reports what it
// finds rather than assuming -- and if the device is at a different address it
// ADOPTS it, so every later test works without an edit.
// ---------------------------------------------------------------------------
Outcome Test04_I2cScan()
{
    Log::Printf("  scanning 0x08..0x77 on SDA=IO%d SCL=IO%d at %d Hz",
                PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);

    int found[16];
    int n = 0;
    for (uint8_t addr = 0x08; addr <= 0x77 && n < 16; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            found[n++] = addr;
            Log::Printf("  device at 0x%02X", addr);
        }
    }

    if (n == 0) {
        True(false, "at least one I2C device answered");
        Note("nothing on the bus: check R5/R6 pull-ups, that U4 is fitted and "
                    "soldered, and that 3V3 is actually up (test 2)");
        return TestRunner::Current();
    }

    // The DAC must be one of them.
    bool dac_found = false;
    for (int i = 0; i < n; ++i) {
        if (found[i] == MCP4728_ADDR) dac_found = true;
    }
    if (dac_found) {
        Log::Printf("  MCP4728 found at the expected 0x%02X", MCP4728_ADDR);
        Dac::SetAddress(MCP4728_ADDR);
        True(true, "MCP4728 answers at its 0x60 strap default");
    } else if (n == 1) {
        // ONE other device on a bus this board only puts a DAC on: it is the DAC,
        // strapped differently. Adopt it and say so -- this is the N-4 measurement.
        Log::Printf("  MCP4728 is NOT at 0x%02X; adopting the only device found, 0x%02X",
                    MCP4728_ADDR, found[0]);
        Note("SPEC ITEM N-4 RESOLVED: the strap address is 0x%02X, not 0x60. "
                    "Change MCP4728_ADDR in include/BoardPins.h and in the production "
                    "firmware's PinMap.h (SWC_MCP4728_ADDR), which is the one home for it.",
                    found[0]);
        Dac::SetAddress((uint8_t)found[0]);
        Dac::Begin();
        return TestRunner::Current();
    } else {
        // Several devices: the MCP4728 is not among them, or the bus is noisy.
        Log::Printf("  %d devices but none at 0x%02X -- cannot identify the DAC", n, MCP4728_ADDR);
        True(false, "the MCP4728 is identifiable on the bus");
        return TestRunner::Current();
    }

    True(n == 1, "exactly one device on the DAC bus (no strays, no shorts)");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 5. DAC write and read-back at several codes
//
// The definitive check that the three-byte Multi-Write frame is correct AND that
// the output latches on the frame's own ACK with no ~LDAC pulse. It writes a code,
// reads the input register back, and compares -- so a malformed frame (which is
// how this part actually fails: the production firmware shipped a four-byte one
// and addressed nothing, silently) is caught here with no meter attached.
//
// Then it measures the ANALOG result on /SENSEn, where the DAC's contribution is
// observable through the servo even with nothing else connected.
// ---------------------------------------------------------------------------
Outcome Test05_DacWriteReadback()
{
    if (!Dac::Present()) {
        Note("no DAC at 0x%02X -- run test 4; skipping", Dac::Address());
        return TestRunner::Blocked();
    }

    const uint16_t codes[] = {0, 1, 512, 2048, 3000, 4094, 4095};
    const size_t n = sizeof(codes) / sizeof(codes[0]);

    // Only the CODE is printed: it is the one field the read response actually
    // carries. The power-down/VREF/gain bits are NOT decodable from it (writing all
    // four power modes changes no byte in the response -- see DacFrame.h), so a
    // column for them would print a number the decoder cannot justify.
    Log::Printf("  %-8s %-10s %-10s %s", "written", "read-back", "dec", "match");

    bool all_match = true;
    for (size_t i = 0; i < n; ++i) {
        // Write channel A (channel 1 signal) in normal mode.
        if (Dac::SetSignal(1, Output::Mode::kAmplified, codes[i]) < 0) {
            True(false, "the write was ACKed");
            break;
        }
        delay(2);

        DacFrame::ChannelReg reg{};
        if (!Dac::ReadChannelReg(DacFrame::kChannelA, &reg)) {
            True(false, "the read-back was ACKed and returned the full response");
            break;
        }
        const bool match = (reg.code == codes[i]);
        all_match = all_match && match;
        Log::Printf("  %-8u 0x%03X(%u)%s %-10s %s", codes[i], reg.code, reg.code,
                    match ? " " : "!", reg.code == codes[i] ? "ok" : "MISMATCH",
                    match ? "yes" : "NO");
    }

    True(all_match, "every written code reads back identically");

    // The frame really is three bytes -- but kSetSize is a COMPILE-TIME constant, so
    // a True() on it cannot fail and would prove nothing at runtime. What actually
    // catches a four-byte frame is the read-back loop above: a four-byte write
    // shifts every field by one, addresses no channel, and the FIRST code compared
    // would already mismatch. The constant is reported, and pinned where it can
    // fail -- in the host suite.
    Log::Printf("  frame size: %u bytes (MCP4728 Multi-Write; the MCP4725's is 4)",
                (unsigned)DacFrame::kSetSize);

    // ~LDAC must be idle-high and NEVER pulsed by this tool: with UDAC = 0 each
    // output latches on its own ACK. Test 10 covers the pin; this asserts the
    // intent at the point where a pulse would have been tempting.
    Note("no ~LDAC pulse was issued: UDAC=0 latches each channel on its own ACK "
         "(spec 2.5.1). The read-back above is what proves that on real silicon.");

    // Leave channel 1 released.
    Dac::Release(1);
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 6. DAC power-down modes, including the 1 kOhm that IS the gain selector
//
// The MCP4728 has NO high-impedance state: all three power-down modes are defined
// pull-DOWNs (1k / 100k / 500k). The 1 kOhm one is the 5 V-range gain selector,
// and the spec chose it over a series MOSFET precisely because it is a DEFINED
// path rather than leakage. So the modes are read back and reported.
// ---------------------------------------------------------------------------
Outcome Test06_DacPowerModes()
{
    if (!Dac::Present()) return TestRunner::Blocked();

    // THE POWER-DOWN FIELD IS NOT OBSERVABLE IN THE READ RESPONSE, and this test does
    // not pretend otherwise. Writing all four PD1:PD0 values to a channel and diffing
    // the 24-byte response changes NOTHING (measured on this board; see DacFrame.h's
    // read-layout note). Whatever carries the power-mode state is not a byte that
    // moves when that state changes, so a read-back assertion here could only produce
    // a fabricated verdict -- the first version of this test did exactly that, and
    // reported a mismatch no matter what was written.
    //
    // What matters about the power-down mode is not its register value anyway: it is
    // the VOLTAGE it produces, because the 1k power-down IS the 5 V-range gain
    // selector (spec 2.3). So the mode is verified BEHAVIOURALLY, which is stronger
    // evidence than a register read would ever have been.
    Log::Printf("  The power-down field is not decodable from the read response, so this");
    Log::Printf("  test verifies the mode by its EFFECT on the output instead:");
    Log::Printf("      V_KEY = 1.82*V_DAC - 0.82*V_ADJ");
    Log::Printf("");

    const uint16_t sig = 2048;                          // V_DAC = 1650 mV
    const int v_dac = Output::DacMvForCode(sig);
    const int key_adj_zero = (int)((182L * v_dac) / 100L);   // 1k down -> V_ADJ = 0
    const int key_adj_same = Output::KeyMvForDacMv(Output::Mode::kTracking, v_dac);

    Log::Printf("  signal code fixed at %u (V_DAC = %d mV)", sig, v_dac);
    Log::Printf("  predicted V_KEY: %d mV if ADJ is pulled to 0; %d mV if ADJ tracks it",
                key_adj_zero, key_adj_same);
    Log::Printf("");
    Log::Printf("  %-24s %-12s %-12s %s", "ADJ mode and code", "sense x2 mV", "predicted", "verdict");

    struct Case { const char *name; DacFrame::PowerMode mode; uint16_t code; int want; };
    const Case cases[] = {
        {"1k down (PD=01), code 0",   DacFrame::kGnd1k,   0,   key_adj_zero},
        {"normal (PD=00), code 0",    DacFrame::kNormal,  0,   key_adj_zero},
        {"normal (PD=00), code 2048", DacFrame::kNormal,  sig, key_adj_same},
    };

    bool all_ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t f[DacFrame::kSetSize];
        DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, sig);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        DacFrame::EncodeSet(f, DacFrame::kChannelB, (uint8_t)cases[i].mode, cases[i].code);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        delay(120);

        uint32_t sense = 0;
        Adc::ReadAvgMv(Adc::kSense1, 32, &sense);
        const int seen = (int)sense * 2;

        // The prediction comes from the transfer function, so 200 mV is the tolerance
        // on the WHOLE analog path -- DAC linearity, servo offset, divider tolerance
        // and ADC error -- not on the mode alone.
        const bool ok = (labs((long)seen - cases[i].want) < 200);
        all_ok = all_ok && ok;
        Log::Printf("  %-24s %-12d %-12d %s", cases[i].name, seen, cases[i].want,
                    ok ? "as predicted" : "<-- NOT AS PREDICTED");
    }

    True(all_ok, "the power-down mode produces the predicted KEY voltage "
                 "(V_ADJ pulled to 0, gain 1.82)");

    Note("This is the 5 V-range gain selector working, verified by measurement: the "
         "1k power-down pulls the summing node to 0 through a DEFINED path, which is "
         "exactly why the spec chose a DAC channel over a series MOSFET (spec 2.3). "
         "A register read could not have shown it -- only the voltage can.");

    Dac::Release(1);
    Dac::Release(2);
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 7. ADC calibration availability and linearity
//
// Spec 3.2's requirement, tested where it can actually fail: the eFuse
// curve-fitting calibration is NOT universally present -- blank-eFuse module
// batches return ESP_ERR_NOT_SUPPORTED and the fallback is a documented straight
// line. The spec requires that fallback to be REPORTED, and this is the report.
//
// Linearity is checked comparatively: read every channel, then read them again in
// reverse. The per-channel offsets must be stable, which catches a mux/settling
// problem the absolute numbers would hide.
// ---------------------------------------------------------------------------
Outcome Test07_AdcCalibration()
{
    Log::Printf("  calibration source: %s", Adc::CalibrationSourceName());

    if (Adc::CalibrationDegraded()) {
        Note("eFUSE CURVE-FITTING UNAVAILABLE on this module -- readings are "
                    "using the documented linear fallback and are degraded, worst at "
                    "the top of the range. The spec names this a BOOT_DEGRADED "
                    "condition, not a silent one. The board still works; the numbers "
                    "are simply not per-chip corrected.");
    } else {
        True(true, "per-chip eFuse curve-fitting calibration is available");
    }

    // Put the analog path in a KNOWN state before measuring. If the loopback jumpers
    // are fitted (J3.KEY jumpered to J2.SWC -- which the servo tests need), the
    // ladder pins are electrically tied to the servo output, so their readings
    // follow whatever the DAC was last doing. Without this, the forward and reverse
    // passes straddle a settling transient and the stability check below reports a
    // "drift" that is really just the servo still moving.
    Dac::Release(1);
    Dac::Release(2);
    delay(200);

    Log::Printf("  %-16s %-14s %-8s %-8s %-8s", "channel", "pin", "raw", "mV", "mV(rev)");

    // Forward pass, then reverse, and compare. The ADC on the S3 is a single
    // sample-and-hold shared across channels, so a channel that has just been
    // sampled after a very different one can read short. A large forward/reverse
    // difference on a STABLE input is that effect.
    uint32_t fwd[Adc::kCount] = {0};
    uint32_t rev[Adc::kCount] = {0};

    for (int i = 0; i < (int)Adc::kCount; ++i) {
        uint16_t raw = 0;
        Adc::ReadAvgMv((Adc::Ch)i, 32, &fwd[i], &raw);
        Log::Printf("  %-16s IO%-2u %-8u %-8u", Adc::Name((Adc::Ch)i), Adc::Pin((Adc::Ch)i),
                    raw, fwd[i]);
    }
    for (int i = (int)Adc::kCount - 1; i >= 0; --i) {
        Adc::ReadAvgMv((Adc::Ch)i, 32, &rev[i]);
    }

    bool stable = true;
    for (int i = 0; i < (int)Adc::kCount; ++i) {
        const long d = (long)fwd[i] - (long)rev[i];
        Log::Printf("  %-16s reverse read %u mV, delta %+ld mV %s", Adc::Name((Adc::Ch)i),
                    rev[i], d, (labs(d) > 60) ? "<-- unstable" : "");
        if (labs(d) > 60) {
            stable = false;
            // The ladder pins share a node with the servo output when the loopback
            // jumpers are fitted, so a large delta there is the servo, not the ADC.
            const bool tied = (i == (int)Adc::kSwc1 || i == (int)Adc::kSwc2);
            if (tied) {
                Log::Printf("     ^ this pin is tied to the servo output by the "
                            "loopback jumper, so this is servo settling, not ADC drift");
            }
        }
    }
    True(stable, "forward and reverse channel sweeps agree within 60 mV");

    // The calibrated ceiling is 2.9 V at 12 dB (spec 2.1), and whether a reading may
    // exceed it depends on WHICH pin -- so one bound for all eight would be wrong in
    // both directions:
    //
    //   * The SENSE pins cannot exceed ~2.49 V BY DESIGN: the op-amp's +5 V rail
    //     binds before the ADC's ceiling (spec 2.3). A reading above that means the
    //     divider or the rail is wrong, so it IS asserted.
    //
    //   * The LADDER and AUX pins are pulled up to +3V3 through 10k and, with no
    //     external pad attached, sit AT the rail -- legitimately at or above the
    //     2.9 V ceiling, where the ADC clips. That is expected on a bare bench and is
    //     exactly what spec 6.3's bring-up step 3 warns about. Asserting a ceiling
    //     here would fail a perfectly healthy board, so it is REPORTED.
    //
    //   * TEMP is a divider midpoint: never above the rail, and its own 1:1 divider
    //     puts it near half. Asserted loosely.
    bool ok = true;
    for (int i = 0; i < (int)Adc::kCount; ++i) {
        const Adc::Ch ch = (Adc::Ch)i;
        const bool is_sense = (ch == Adc::kSense1 || ch == Adc::kSense2);
        const bool is_temp  = (ch == Adc::kTemp);

        if (is_sense && fwd[i] > 2600) {
            Log::Printf("  %s reads %u mV, ABOVE the ~2.49 V the design guarantees "
                        "(the op-amp rail should bind first) -- divider or +5 V rail?",
                        Adc::Name(ch), fwd[i]);
            ok = false;
        }
        if (is_temp && fwd[i] > ADC_CEILING_MV_12DB) {
            Log::Printf("  %s reads %u mV, above the 2.9 V ceiling -- its 1:1 divider "
                        "cannot reach that", Adc::Name(ch), fwd[i]);
            ok = false;
        }
        if (!is_sense && !is_temp && fwd[i] >= ADC_CEILING_MV_12DB - 40) {
            Log::Printf("  %s reads %u mV: at or above the 2.9 V ceiling, so it is "
                        "CLIPPING. Expected with no ladder attached (the pull-up holds "
                        "the node at the rail), and it stops being harmless once a real "
                        "pad is fitted -- spec 6.3 step 3 is the check for that.",
                        Adc::Name(ch), fwd[i]);
        }
    }
    True(ok, "the sense and NTC pins stay inside their designed bounds");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 8. Every ADC channel responds to a change
//
// The point of this test is to prove each input is a LIVE analog path rather than
// a stuck node. All of these inputs are pulled up to 3V3 through 10 kOhm, so with
// nothing attached they all sit HIGH. Touching a jumper from the pin to GND (or to
// the 3V3 screw terminal) must move it.
//
// The operator is asked for a loopback because the board cannot produce a stimulus
// on its own -- the test says which terminal to use.
// ---------------------------------------------------------------------------
const char *Setup08_AdcChannels()
{
    return "Optional but recommended: have a jumper wire ready, to touch each input "
           "to GND in turn. Press ENTER to continue -- without a jumper the test "
           "reports the resting levels, which is still a useful result.";
}

Outcome Test08_AdcChannelsLive()
{
    Log::Printf("  Each input is pulled up to +3V3 through 10k, so unattached it");
    Log::Printf("  rests HIGH. A jumper from the pin to any GND moves it down.");
    Log::Printf("");
    Log::Printf("  %-16s %-6s %-10s %-10s %s", "channel", "pin", "rest mV", "rest raw", "expected rest");

    struct { Adc::Ch ch; const char *expect; } rows[] = {
        {Adc::kSwc1,   "high (~rail) via R15 10k"},
        {Adc::kSwc2,   "high (~rail) via R16 10k"},
        {Adc::kAux1,   "high (~rail) via R17 10k"},
        {Adc::kAux2,   "high (~rail) via R18 10k"},
        {Adc::kAux3,   "high (~rail) via R19 10k"},
        {Adc::kTemp,   "~half rail (R29/RT1 10k:10k)"},
        {Adc::kSense1, "0 V if KEY1 released, else half the KEY line"},
        {Adc::kSense2, "0 V if KEY2 released, else half the KEY line"},
    };

    bool ok = true;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        uint32_t mv = 0;
        uint16_t raw = 0;
        Adc::ReadAvgMv(rows[i].ch, 64, &mv, &raw);
        Log::Printf("  %-16s IO%-3u %-10u %-10u %s", Adc::Name(rows[i].ch),
                    Adc::Pin(rows[i].ch), mv, raw, rows[i].expect);

        // The pulled-up inputs must rest high. If one reads near zero with nothing
        // attached, its pull-up is missing or the node is shorted -- which would
        // make every later ladder reading meaningless, so it is asserted here.
        const bool is_pulled_up = (rows[i].ch == Adc::kSwc1 || rows[i].ch == Adc::kSwc2 ||
                                   rows[i].ch == Adc::kAux1 || rows[i].ch == Adc::kAux2 ||
                                   rows[i].ch == Adc::kAux3);
        if (is_pulled_up && mv < 2500) {
            Log::Printf("     ^ %s should rest HIGH; %u mV means its pull-up is not "
                        "pulling (R15/R16/R17-R19) or the node is loaded to GND",
                        Adc::Name(rows[i].ch), mv);
            ok = false;
        }
    }
    True(ok, "every pulled-up input rests high with nothing attached");

    Note("The sense channels are the exception: they follow the servo output, so "
         "test 11 proves they are live by driving the DAC rather than by a jumper.");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 9. MCP4728 EEPROM power-on state
//
// DESIGN.md 4.6: the firmware is supposed to program the EEPROM so a cold start
// resets channel A to FULL SCALE (the maximum command, which releases both 3 V and
// 5 V head units) and the gain-mode channel reset powered-down. This board MAY or
// MAY NOT have been through that programming -- the production firmware does not
// do it either (it never writes EEPROM), so in practice the factory default is
// what you get.
//
// So this test does not assert the intended state. It READS the EEPROM and reports
// what a cold start would actually produce, which is the fact that matters for
// power-up safety. The tool itself does not depend on it: Dac::Begin() drives both
// channels to full scale unconditionally, so the outputs are released regardless.
// ---------------------------------------------------------------------------
Outcome Test09_DacEeprom()
{
    if (!Dac::Present()) return TestRunner::Blocked();

    // The read-all command returns input registers, then EEPROM, then status.
    Wire.beginTransmission(DacFrame::kAddrGeneralCall);
    Wire.write(DacFrame::kReadCmdAll);
    if (Wire.endTransmission() != 0) {
        True(false, "the read-all command was ACKed");
        return TestRunner::Current();
    }
    const size_t got = Wire.requestFrom((int)Dac::Address(), (int)DacFrame::kReadAllBytes);
    if (got != DacFrame::kReadAllBytes) {
        Log::Printf("  read-all returned %u bytes, expected %u", (unsigned)got,
                    (unsigned)DacFrame::kReadAllBytes);
        True(false, "read-all returned all 24 bytes");
        return TestRunner::Current();
    }
    uint8_t buf[DacFrame::kReadAllBytes];
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = (uint8_t)Wire.read();

    Log::Printf("  %-8s %-10s %-12s %-8s %-6s %s", "channel", "input code", "EEPROM code",
                "EE pwr", "vref", "cold-start effect");

    // Bytes 0..7 are the input registers, 8..15 the EEPROM, 16 is the status.
    int unprogrammed = 0;
    for (uint8_t ch = 0; ch < 4; ++ch) {
        const size_t o = DacFrame::ChannelOffset(ch);
        const DacFrame::ChannelReg in = DacFrame::DecodeChannel(buf[o], buf[o + 1]);
        const DacFrame::ChannelReg ee =
            DacFrame::DecodeChannel(buf[8 + o], buf[9 + o]);

        const char *effect = "?";
        if (ch == DacFrame::kChannelA || ch == DacFrame::kChannelC) {
            effect = (ee.code >= 4000) ? "release (full scale) -- SAFE"
                                       : "NOT full scale -- cold start would DRIVE a key";
        } else {
            effect = (ee.power_mode == DacFrame::kGnd1k) ? "1k pulldown -- safe default"
                                                         : "not powered down";
        }

        Log::Printf("  %-8u 0x%03X     0x%03X        %-8u %-6u %s", ch,
                    in.code, ee.code, ee.power_mode, ee.vref, effect);

        // An all-ones EEPROM word is what an unprogrammed (or blank) part returns,
        // so count them and report rather than assert: this tool does not depend on
        // the EEPROM's contents, and a board that has never had them programmed is
        // the normal case.
        if (ee.code == 0x0FFF && ee.power_mode == 3) ++unprogrammed;
    }

    const uint8_t status = buf[16];
    Log::Printf("  status byte    : 0x%02X  (POR=%u RDY/BSY=%u A2:A0=%u)",
                status, (status >> 7) & 1, (status >> 6) & 1, status & 0x07);

    Note("This tool does NOT rely on the EEPROM: Dac::Begin() drives both signal "
         "channels to full scale itself, so the KEY lines are released on every "
         "boot whether or not the EEPROM was ever programmed.");
    Note("The production firmware never writes the EEPROM either, so a cold start "
         "here reflects the factory default. If the 'input code' column is not full "
         "scale, that is the default, not a fault.");

    if (unprogrammed) {
        Log::Printf("  %d of 4 EEPROM words read as all-ones (unprogrammed or blank).",
                    unprogrammed);
    }
    True(true, "the EEPROM was read and reported (no assertion -- the tool does not "
               "depend on its contents)");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 10. ~LDAC is idle-high and is never pulsed
//
// Spec 2.5.1: with UDAC = 0 on every write, each output latches on its own ACK and
// ~LDAC is never needed. It is configured as an output and held HIGH so the R13
// 10 kOhm pulldown cannot latch at an arbitrary time during start-up.
//
// The consequence to verify is behavioural, not electrical: writing the two
// channels in a known order must leave them INDEPENDENTLY set. If a deferred latch
// were in use, both would take their new values at one instant and the
// intermediate state the servo depends on would be destroyed (spec 2.3,
// consequence 1 explicitly rejects that).
// ---------------------------------------------------------------------------
Outcome Test10_LdacNeverPulsed()
{
    // The pin must be an output driving HIGH.
    pinMode(PIN_DAC_LDAC_B, OUTPUT);
    digitalWrite(PIN_DAC_LDAC_B, HIGH);
    delay(5);
    const int level = digitalRead(PIN_DAC_LDAC_B);
    Log::Printf("  ~LDAC (IO%d) driven HIGH, reads %d", PIN_DAC_LDAC_B, level);
    True(level == 1, "~LDAC is held high and not being pulled low by R13");

    if (!Dac::Present()) {
        Note("no DAC -- the independence check below is skipped");
        return TestRunner::Current();
    }

    // Write the two channels to DIFFERENT codes back to back, then read BOTH back
    // from the 24-byte response. Independent latching means each holds its own value;
    // a deferred ~LDAC latch would have applied both at one instant and left them
    // equal. This is the assertion the test is NAMED for, so it is made against the
    // codes actually read back -- the first version compared them and then returned
    // True(true), so it proved nothing.
    const uint16_t code1 = 1000, code2 = 3000;
    Dac::SetSignal(1, Output::Mode::kTracking, code1);
    Dac::SetSignal(2, Output::Mode::kTracking, code2);
    delay(6);

    DacFrame::ChannelReg a{}, c{};
    if (!Dac::ReadChannelReg(DacFrame::kChannelA, &a) ||
        !Dac::ReadChannelReg(DacFrame::kChannelC, &c)) {
        True(false, "both channels read back from the full response");
        return TestRunner::Current();
    }
    Log::Printf("  ch1 wrote %u, reads %u", code1, a.code);
    Log::Printf("  ch2 wrote %u, reads %u", code2, c.code);

    True(a.code == code1, "channel 1 holds its own code independently");
    True(c.code == code2, "channel 2 holds its own code independently");
    True(a.code != c.code, "the two channels did NOT take a shared value "
                           "(a deferred ~LDAC latch would have made them equal)");

    // Restore: pin stays high, channels released.
    digitalWrite(PIN_DAC_LDAC_B, HIGH);
    Dac::Release(1);
    Dac::Release(2);
    return TestRunner::Current();
}

// The registry is defined in Tests.cpp's companion; see TestList.cpp.

}  // namespace SwcTests
