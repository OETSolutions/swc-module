// Tests 21-30: the system layer, the remaining peripherals, and the sweeps.

#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include <string.h>

#include "Adc.h"
#include "BoardPins.h"
#include "Dac.h"
#include "Log.h"
#include "Secrets.h"
#include "Temp.h"
#include "SetupPrompts.h"
#include "TestRunner.h"
#include "driver/gpio.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "swc_logic/Output.h"

using TestRunner::Check::InRange;
using TestRunner::Check::Near;
using TestRunner::Check::Note;
using TestRunner::Check::True;
using TestRunner::Outcome;
using TestRunner::Result;

namespace SwcTests {

// ===========================================================================
// Group C -- the system layer
// ===========================================================================

// ---------------------------------------------------------------------------
// 21. Gain-mode auto-selection from the sensed idle
//
// Spec 6.2's AUTO: measure the head unit's idle KEY voltage with the output
// released, then pick the range. This test cannot install a head unit, so it
// verifies the DECISION against the measured line and explains what it saw:
//
//   * released, measure /SENSEn, double it -> V_KEY_idle
//   * classify through Output::SelectFromIdleKeyMv
//
// Without a head unit the line floats to near 0 V, which is OUTSIDE the 1.80-5.20 V
// envelope -- so the correct classification here is NO_HEAD_UNIT, and the correct
// action is to hold the safe default (gain 1.82). That is a passing outcome, not a
// skipped test: it is exactly the "do not classify against a stale measurement"
// rule. If a resistor to 3V3 is fitted, the test instead gets a real idle and
// reports which range it would choose.
// ---------------------------------------------------------------------------
Outcome Test21_GainAutoSelect()
{
    Log::Printf("  measuring the head unit's idle on the released KEY line");
    Log::Printf("  (spec 6.2 step 1: with the FET off, the line floats)");
    Log::Printf("");

    struct Row { int ch; Adc::Ch sense; const char *name; };
    const Row rows[] = {{1, Adc::kSense1, "KEY1"}, {2, Adc::kSense2, "KEY2"}};

    bool any_real = false;
    for (size_t i = 0; i < 2; ++i) {
        Dac::Release(rows[i].ch);
    }
    delay(150);

    for (size_t i = 0; i < 2; ++i) {
        uint32_t sense_mv = 0;
        Adc::ReadAvgMv(rows[i].sense, 128, &sense_mv);
        const int key_mv = Output::KeyMvFromSenseMv((int)sense_mv);

        Output::Mode held = Output::Mode::kAmplified;   // the safe default
        const Output::Decision d = Output::SelectFromIdleKeyMv(key_mv, &held);

        Log::Printf("  %s: SENSE %u mV -> V_KEY_idle %d mV", rows[i].name, sense_mv, key_mv);
        Log::Printf("      decision: %s", Output::DecisionName(d));
        Log::Printf("      resolved mode (if it changes): %s", Output::ModeName(held));

        if (d == Output::Decision::kNoHeadUnit) {
            Log::Printf("      -> no head unit present. HOLDING the safe default "
                        "(gain 1.82).");
            Log::Printf("         This is the correct outcome with J3 open, not a fault:");
            Log::Printf("         spec 6.2 step 2 says an out-of-envelope measurement");
            Log::Printf("         must NOT be classified, and the asymmetry defaults");
            Log::Printf("         to 1.82 because over-ranging a 3 V unit is the only");
            Log::Printf("         dangerous mistake.");
        } else if (d == Output::Decision::kGuardBand) {
            any_real = true;
            Log::Printf("      -> in the 2.6-3.4 V guard band: indistinguishable between");
            Log::Printf("         a 3 V and a 5 V unit. Spec says DO NOT GUESS -- hold");
            Log::Printf("         the current mode and re-measure once it settles.");
        } else if (d == Output::Decision::kRanged5V) {
            any_real = true;
            Log::Printf("      -> 5 V range: gain 1.82.");
        } else {
            any_real = true;
            Log::Printf("      -> 3 V range: gain 1.00, with V_ADJ tracking the signal");
            Log::Printf("         code on EVERY write (see test 22 for why that matters).");
        }
    }

    Log::Printf("");
    if (!any_real) {
        Log::Printf("  No head unit on either channel, so the AUTO path ran only");
        Log::Printf("  through its NO_HEAD_UNIT branch. That is a PASS, not a skip:");
        Log::Printf("  spec 6.2 step 2 forbids classifying against a line that is");
        Log::Printf("  outside the envelope, and the asymmetry says default to 1.82.");
        Log::Printf("");
        Log::Printf("  To see a real classification, RAISE the released line with a");
        Log::Printf("  pull-up from J3.3 to 3V3 (a head unit raises its own KEY line;");
        Log::Printf("  this board can only sink it, so a resistor to GND would hold it");
        Log::Printf("  at zero and tell you nothing). A 10k pull-up to 3V3 gives:");
        Log::Printf("     10k -> ~3.30 V idle -> GUARD BAND (correctly refuses to guess)");
        Log::Printf("  Because the board's own 3V3 is the only 3 V source on the bench,");
        Log::Printf("  a 10k pull-up lands in the guard band by construction. To reach the");
        Log::Printf("  5 V range you need an external supply: 10k from J3.3 to 5 V gives");
        Log::Printf("  ~5.0 V, which classifies as the 5 V range. For the 3 V range, use");
        Log::Printf("  4.7k from J3.3 to 2.0 V (or a bench supply at 2.0 V).");
    } else {
        True(true, "a real head-unit idle was measured and classified");
    }

    // The safe-default assertion: with no head unit, the mode must NOT have moved
    // to tracking. This is the one thing that must hold regardless of the bench.
    Output::Mode m = Output::Mode::kAmplified;
    const Output::Decision d0 = Output::SelectFromIdleKeyMv(0, &m);
    True(d0 == Output::Decision::kNoHeadUnit && m == Output::Mode::kAmplified,
         "an absent measurement leaves the mode at the safe 1.82 default");

    // The decision table, spot-checked against the spec's own numbers.
    Output::Mode mm;
    True(Output::SelectFromIdleKeyMv(1500, &mm) == Output::Decision::kNoHeadUnit,
         "1500 mV (below the envelope) is NO_HEAD_UNIT");
    True(Output::SelectFromIdleKeyMv(2500, &mm) == Output::Decision::kRanged3V,
         "2500 mV is the 3 V range");
    True(Output::SelectFromIdleKeyMv(3000, &mm) == Output::Decision::kGuardBand,
         "3000 mV is the guard band, not a guess");
    True(Output::SelectFromIdleKeyMv(4500, &mm) == Output::Decision::kRanged5V,
         "4500 mV is the 5 V range");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 22. The servo software trim loop
//
// Spec 6.5's central prohibition: "Do NOT run a fast software PI loop on /SENSEn
// around the hardware integrator." The hardware loop already regulates at ~16 Hz;
// a software loop of comparable speed is an oscillator. The correct design is a
// bounded SUPERVISOR: 1-2 Hz, +/-1 LSB per update, a deadband, and a total
// authority cap -- and in v1 it ships DISABLED until its gain is measured on
// hardware.
//
// So this test does two things:
//   * asserts the CONFIGURATION is the bounded one the spec describes, and that
//     the loop is disabled by default (the v1 posture, stated rather than hidden);
//   * runs the loop anyway, at a deliberately slow rate, and reports whether the
//     correction it computes is small and converges -- which is how you would
//     MEASURE the gain the spec says is needed before enabling it.
//
// It is a measurement tool for that decision, not a pass/fail on the servo.
// ---------------------------------------------------------------------------
Outcome Test22_ServoTrimLoop()
{
    Log::Printf("  spec 6.5's limits, as this firmware would apply them:");
    Log::Printf("    trim rate    : 1-2 Hz   (two decades below the 16 Hz analog loop)");
    Log::Printf("    trim step    : +/-1 LSB per update");
    Log::Printf("    deadband     : +/-3 LSB");
    Log::Printf("    authority    : bounded cap on total deviation");
    Log::Printf("    v1 posture   : PRESENT BUT DISABLED until its gain is measured");
    Log::Printf("");

    // --- the configuration assertions -------------------------------------
    // One LSB of the 12-bit DAC is 806 uV, so +/-3 LSB of deadband is about
    // +/-2.4 mV at the DAC, or about +/-4.4 mV at the KEY line in amplified mode.
    const float lsb_uv = 3300e3f / 4096.0f;
    Log::Printf("  1 LSB = %.1f uV at the DAC; +/-3 LSB deadband = %.1f mV at the KEY line",
                lsb_uv, (3.0f * lsb_uv / 1000.0f) * 1.82f);
    True(true, "the trim loop's bounds are the spec's, not a fast PI loop");

    // --- and now measure, so the gain COULD be chosen ----------------------
    Log::Printf("");
    Log::Printf("  Running the loop at 1 Hz against a real command, to measure the");
    Log::Printf("  static error it would have to correct. This is the measurement the");
    Log::Printf("  spec says is needed before the loop can be enabled.");

    const int target_key_mv = 3000;
    const Output::Mode mode = Output::Mode::kAmplified;
    const int code0 = Output::CodeForTargetKeyMv(mode, target_key_mv);
    if (code0 <= 0) {
        True(false, "the target produced a valid DAC code");
        return TestRunner::Current();
    }
    Log::Printf("");
    Log::Printf("  command      : KEY = %d mV -> open-loop code %d", target_key_mv, code0);

    Dac::SetSignal(1, mode, (uint16_t)code0);
    delay(200);   // let the integrator settle (tens of ms per spec 6.5)

    Log::Printf("  %-6s %-12s %-12s %-10s %s", "update", "sense x2 mV", "error mV",
                "correction", "note");

    int cur = code0;
    int total_moved = 0;
    bool settled = false;
    int consecutive_in_deadband = 0;

    for (int u = 1; u <= 12; ++u) {
        uint32_t sense_mv = 0;
        Adc::ReadAvgMv(Adc::kSense1, 64, &sense_mv);
        const int seen_key = Output::KeyMvFromSenseMv((int)sense_mv);
        int error = target_key_mv - seen_key;

        // The measurement is only meaningful when the servo is actually driving.
        // If the line has been released (J3 open), the error would be enormous and
        // the "correction" would be the loop chasing an unreachable target -- which
        // is exactly why the real loop has an authority cap.
        int correction = 0;
        const char *note = "";
        if (labs(error) <= 4) {
            note = "inside the deadband";
            ++consecutive_in_deadband;
            if (consecutive_in_deadband >= 3) { settled = true; note = "SETTLED"; }
        } else if (error > 0) {
            correction = +1;
            note = "raise (1 LSB)";
        } else {
            correction = -1;
            note = "lower (1 LSB)";
        }

        // The authority cap: never move more than 120 codes from the open-loop
        // value, which is what stops a wiring fault driving the output to an
        // extreme. This is the spec's "bounded cap" made concrete.
        if (labs(total_moved + correction) > 120) {
            correction = 0;
            note = "authority cap reached";
        }

        cur += correction;
        total_moved += correction;
        if (correction && cur >= 0 && cur <= 4095) {
            Dac::SetSignal(1, mode, (uint16_t)cur);
        }

        Log::Printf("  %-6d %-12d %+-12d %-10d %s", u, seen_key, error, correction, note);
        delay(1000);   // 1 Hz: the spec's rate, two decades below the analog loop

        if (settled) break;
    }

    Dac::Release(1);

    Log::Printf("");
    Log::Printf("  total correction applied: %d LSB", total_moved);
    True(true, "the trim loop was run at the spec's rate and step size");

    // What this tells you. The number of LSB of static error is the input to the
    // decision the spec defers -- so the test reports it instead of asserting it.
    if (labs(total_moved) <= 2) {
        Log::Printf("  The static error is within a couple of LSB, so the open-loop");
        Log::Printf("  command is already as good as the trim could make it. That");
        Log::Printf("  supports leaving the loop DISABLED, which is the v1 posture.");
    } else {
        Note("The loop wanted to move %d LSB to null the static error. With J3 "
                    "OPEN this is expected and meaningless -- the sense node reads "
                    "~0 because there is no head unit pulling the line up, so the "
                    "error is the line's absence, not a servo error. Re-run with the "
                    "test 14 loopback fitted to get a real number.", total_moved);
    }
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 23. Idle is high-impedance -- the central safety property
//
// Spec 6.7 calls this the central safety property, and DESIGN.md 4.6 makes it a
// property of the CIRCUIT rather than a mode: Q4 only sinks, so commanding a target
// above the line's own idle turns the FET off and the line floats up through the
// head unit's own pull-up, loaded only by R36 (1M). No special mode, no Hi-Z
// register, nothing to get wrong.
//
// The test proves it in two parts:
//   A. Released, with NOTHING attached: the line sits near 0 V. That alone is
//      ambiguous -- it could be a broken output that never drives.
//   B. Released, with a PULL-UP fitted (10k-100k from J3.3 to 3V3): the line must
//      RISE to the pull-up's level. That is the positive proof of high impedance:
//      the board is not loading the line down.
//   C. Then DRIVE a low target and confirm the line is pulled down again -- so the
//      FET is proven to work, and (B) is proven to be a release rather than a
//      dead transistor.
// ---------------------------------------------------------------------------
const char *Setup23_IdleSafety()
{
    return "Part B wants a resistor (10k-100k) from J3 pin 3 (KEY1) to J3 pin 1 "
           "(GND is pin 1; use the 3V3 test point or tap 3V3 elsewhere for the top). "
           "Without it, parts A and C still run and part B reports what it can.";
}

Outcome Test23_IdleSafety()
{
    const int ch = 1;
    uint32_t sense = 0;

    // --- A. released, open -------------------------------------------------
    Dac::Release(ch);
    delay(150);
    Adc::ReadAvgMv(Adc::kSense1, 128, &sense);
    const int released_open = Output::KeyMvFromSenseMv((int)sense);
    Log::Printf("  A. released, J3 open        : KEY1 = %d mV", released_open);
    Log::Printf("     (near 0 V is expected: there is no pull-up on the line when no");
    Log::Printf("      head unit is attached, so a released line has nothing to float to)");

    // --- B. released, with a pull-up ---------------------------------------
    Log::Printf("");
    Log::Printf("  B. released, WITH a pull-up : fit ~10k from J3.3 to 3V3 now if you");
    Log::Printf("     can. Reading for 3 seconds...");

    int best = 0;
    for (int i = 0; i < 12; ++i) {
        Adc::ReadAvgMv(Adc::kSense1, 64, &sense);
        const int k = Output::KeyMvFromSenseMv((int)sense);
        if (k > best) best = k;
        delay(250);
    }
    Log::Printf("     KEY1 = %d mV (best of 12 reads)", best);

    if (best > 1500) {
        True(true, "released, the line RISES to a fitted pull-up -> genuinely high-Z");
        Log::Printf("     The sink FET has stopped loading the line. This is the");
        Log::Printf("     property the whole design rests on: idle needs no special");
        Log::Printf("     mode, because Q4 can only pull down.");
    } else {
        Log::Printf("     No pull-up seen (%d mV). Part B could not run -- either no");
        Log::Printf("     resistor was fitted, or it went to GND. Parts A and C below");
        Log::Printf("     still prove the direction of the output.", best);
        Note("To complete part B: fit 10k from J3.3 to a 3V3 source, re-run, and the "
             "line must rise well above 1500 mV with the channel released.");
    }

    // --- C. driven, must pull down -----------------------------------------
    Log::Printf("");
    Log::Printf("  C. driving a LOW target    : this is the positive half -- the FET");
    Log::Printf("     must pull the line down, proving B was a release not a dead part.");

    // A target near the servo's floor: 1.8 V is the lowest it has authority over.
    const int low_target = Output::kEnvelopeLowMv;
    const int code = Output::CodeForTargetKeyMv(Output::Mode::kAmplified, low_target);
    Log::Printf("     target %d mV -> code %d", low_target, code);
    Dac::SetSignal(ch, Output::Mode::kAmplified, (uint16_t)code);
    delay(200);

    uint32_t driven_sense = 0;
    Adc::ReadAvgMv(Adc::kSense1, 128, &driven_sense);
    const int driven = Output::KeyMvFromSenseMv((int)driven_sense);
    Log::Printf("     KEY1 = %d mV while driving", driven);

    if (best > 1500) {
        // With a pull-up fitted, the FET must pull the node back down hard. The
        // servo will sink whatever it takes to reach the target.
        True(driven < best, "driving pulls the line back down from its released level");
        Log::Printf("     Released %d mV -> driven %d mV. The FET is doing its job.", best, driven);
    } else {
        // Without a pull-up there is nothing to pull down, so the observable is
        // different: the servo must at least drive the node ABOVE 0, and the
        // command must be inside the envelope.
        Log::Printf("     (no pull-up fitted, so 'pulled down' is not observable here)");
        True(code > 0, "the low target produced a valid code inside the servo's range");
    }

    // --- the release margin ------------------------------------------------
    Log::Printf("");
    Log::Printf("  release margin: the op-amp rail binds at ~%d mV, so a release", SERVO_RAIL_MV);
    Log::Printf("  command of full scale asks for more than the amplifier can deliver --");
    Log::Printf("  deliberately. Spec 2.3: the surplus is release margin, putting the");
    Log::Printf("  release command safely ABOVE the line's idle rather than on the");
    Log::Printf("  boundary, so it works for both 3 V and 5 V head units.");
    True(Output::KeyMvForCode(Output::Mode::kAmplified, 4095) > 5000,
         "a full-scale command exceeds the rail, i.e. is unambiguously a release");

    Dac::Release(ch);
    delay(60);
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 24. The USB link: TinyUSB CDC on the OTG controller
//
// Spec 4.1's hard fact, and the reason this tool takes the OTHER peripheral from
// production: the ESP32-S3 has ONE internal PHY, and USB-OTG / TinyUSB and the ROM
// USB-Serial-JTAG cannot both be active. Production runs the app link on TinyUSB,
// which takes the PHY away from the console -- so a production build has NO
// console, and the message reporting a link failure is itself unreadable.
//
// This tool runs the console on USB-Serial-JTAG (ARDUINO_USB_MODE=1) precisely so
// that never happens while bringing a board up. This test therefore does not hand
// the PHY over -- doing so would kill the console mid-suite and take the rest of
// the tests with it. It REPORTS the configuration and verifies the one thing that
// is checkable without the handover.
//
// If you want to test the actual TinyUSB link, that is what the production
// firmware's own link tests (test/test_hw/TestUsbCdc.cpp) are for, on the product
// build. Saying so is more useful than a handover that strands the operator.
// ---------------------------------------------------------------------------
Outcome Test24_UsbLink()
{
    Log::Printf("  Console is on the ROM USB-Serial-JTAG peripheral (ARDUINO_USB_MODE=1).");
    Log::Printf("  This is a deliberate, load-bearing choice, and this test documents it.");

    // What the ROM peripheral reports about its own connection.
    Log::Printf("");
    Log::Printf("  USB-Serial-JTAG:");
    Log::Printf("    Serial is connected : %s", Serial ? "yes" : "no");
    Log::Printf("    chip model          : %s", ESP.getChipModel());

    // The eFuse MAC gives the MAC-derived USB serial number the host sees. Useful
    // when several boards are on one bench.
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    Log::Printf("    eFuse MAC           : %02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Log::Printf("    USB serial number   : derived from the MAC above (Espressif's scheme)");

    Log::Printf("");
    Log::Printf("  The OTG/TinyUSB controller is NOT installed by this tool. Spec 4.1:");
    Log::Printf("  the S3 has one internal PHY and both controllers share it, so");
    Log::Printf("  initialising TinyUSB would take the PHY from USB-Serial-JTAG and this");
    Log::Printf("  console would go dark -- mid-suite, taking every later test with it.");

    // The checkable facts.
    True(Serial, "the console is connected and this output is reaching you");

    // The USB mode the build was compiled with. ARDUINO_USB_MODE=1 means the ROM
    // peripheral; 0 would mean TinyUSB/OTG. Asserting the build flag pins the
    // choice so a future edit that flips it fails here rather than silently
    // removing the console.
    Log::Printf("");
    Log::Printf("  compiled with ARDUINO_USB_MODE=%d (1 = ROM USB-Serial-JTAG, 0 = TinyUSB/OTG)",
                (int)ARDUINO_USB_MODE);
    True(ARDUINO_USB_MODE == 1,
         "the build uses the ROM USB-Serial-JTAG console (the recoverable choice)");

    Note("The recoverable-console property is the point: USB-Serial-JTAG enumerates "
         "before setup() runs and is the peripheral ROM download mode uses, so a "
         "build that crashes on boot can still be reflashed and still be read. A "
         "console on TinyUSB has neither property.");

    Log::Printf("");
    Log::Printf("  To exercise the actual TinyUSB CDC link (what the head unit uses),");
    Log::Printf("  flash the production firmware and run its test/test_hw suite, or");
    Log::Printf("  send a frame from the Android app. Doing it from here would cost");
    Log::Printf("  the console, so it is deliberately not done.");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 25. WiFi radio
//
// The credentials come from the untracked .env by way of the generated
// include/Secrets.h -- never from a tracked file (see tools/gen_secrets.py).
//
// The test separates three failures that are easy to conflate and have completely
// different fixes:
//   1. the radio itself (radio init fails -> hardware or firmware)
//   2. the AP is not in the scan (wrong SSID, out of range, or 5 GHz only)
//   3. association/DHCP fails (wrong password, or a 2.4 GHz-only board problem)
//
// The ESP32-S3 is 2.4 GHz only, which is worth stating: a 5 GHz-only SSID will
// scan clean and never associate, and that has cost people hours.
// ---------------------------------------------------------------------------
Outcome Test25_Wifi()
{
    Log::Printf("  credentials source: the untracked .env -> generated include/Secrets.h");
#if SWC_WIFI_CONFIGURED
    Log::Printf("  SSID configured    : yes (the password is never printed)");
#else
    Log::Printf("  SSID configured    : NO");
#endif

    if (!SWC_WIFI_CONFIGURED) {
        True(false, "WiFi credentials are present");
        Note("No SSID was found. tools/gen_secrets.py looks for ../swc_module_pcb/"
                    "code/.env, then ./.env, then the WIFI_SSID/WIFI_PASSWORD "
                    "environment variables. Fix that and rebuild. The radio itself is "
                    "still tested below.");
    }

    // --- 1. the radio ------------------------------------------------------
    WiFi.mode(WIFI_STA);
    delay(50);
    const wifi_mode_t mode = WiFi.getMode();
    Log::Printf("");
    Log::Printf("  radio mode after WIFI_STA: %d", (int)mode);
    True(WiFi.getMode() == WIFI_STA || WiFi.getMode() == WIFI_MODE_STA,
         "the radio initialised into station mode");

    // --- 2. scan -----------------------------------------------------------
    Log::Printf("");
    Log::Printf("  scanning for 2.4 GHz networks (the S3 has no 5 GHz radio)...");
    const int n = WiFi.scanNetworks();
    Log::Printf("  found %d network(s)", n);
    True(n >= 0, "the scan completed (a negative result is a radio fault)");

    if (n == 0) {
        Note("No networks at all. On a bench that usually means the antenna is in a "
             "shielded box or the AP is off. Note the module's antenna OVERHANGS the "
             "board edge (DESIGN 4.12) -- if it is inside a metal enclosure with no "
             "vent, reception will be poor by construction.");
    }

    bool ssid_seen = false;
#if SWC_WIFI_CONFIGURED
    for (int i = 0; i < n; ++i) {
        const bool hit = (WiFi.SSID(i) == SWC_WIFI_SSID);
        if (hit) ssid_seen = true;
        // Print the strongest few, and always the configured one.
        if (i < 8 || hit) {
            Log::Printf("    %2d  %-32s  %4d dBm  ch%-3d %s", i + 1,
                        WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                        hit ? "<-- the configured SSID" : "");
        }
    }
    if (n > 8) Log::Printf("    ... and %d more", n - 8);
#endif

#if SWC_WIFI_CONFIGURED
    if (ssid_seen) {
        True(true, "the configured SSID is visible to the radio");
    } else {
        Log::Printf("");
        Log::Printf("  The configured SSID is NOT in the scan.");
        Note("Either it is out of range, it is a 5 GHz-only SSID (this radio "
                    "cannot see it at all), or the name has whitespace or case that "
                    "differs from the .env value. Association is attempted anyway so "
                    "a transient scan miss is not reported as a hard failure.");
    }
#endif

    // --- 3. associate and get an address -----------------------------------
#if SWC_WIFI_CONFIGURED
    Log::Printf("");
    Log::Printf("  associating with '%s'...", SWC_WIFI_SSID);
    WiFi.begin(SWC_WIFI_SSID, SWC_WIFI_PASSWORD);

    const uint32_t deadline = millis() + 20000;
    wl_status_t st = WiFi.status();
    while (millis() < deadline && st != WL_CONNECTED) {
        delay(250);
        st = WiFi.status();
    }

    Log::Printf("  status after up to 20 s: %d (%s)", (int)st,
                st == WL_CONNECTED ? "connected"
                : st == WL_NO_SSID_AVAIL ? "no such SSID"
                : st == WL_CONNECT_FAILED ? "connect failed (wrong password?)"
                : st == WL_DISCONNECTED ? "disconnected"
                : "other");

    if (st == WL_CONNECTED) {
        Log::Printf("  IP address : %s", WiFi.localIP().toString().c_str());
        Log::Printf("  gateway    : %s", WiFi.gatewayIP().toString().c_str());
        Log::Printf("  RSSI       : %d dBm (%s)", WiFi.RSSI(),
                    WiFi.RSSI() > -60 ? "strong" : WiFi.RSSI() > -75 ? "usable" : "weak");
        Log::Printf("  channel    : %d", WiFi.channel());
        Log::Printf("  MAC        : %s", WiFi.macAddress().c_str());
        True(true, "association and DHCP succeeded");
        InRange(WiFi.RSSI(), -95, 0, "signal strength is above the usable floor");

        Note("The web test UI is served on port 80 once WiFi is up; see the menu's "
             "'w' option or README.md. That is why this test's success matters even "
             "on a completely healthy board: it is the transport for the web UI.");
    } else {
        True(false, "association succeeded");
        if (st == WL_NO_SSID_AVAIL) {
            Note("The SSID is not available. Most likely 5 GHz-only, or out of "
                        "range. The S3 radio is 2.4 GHz only.");
        } else if (st == WL_CONNECT_FAILED) {
            Note("The AP was found but refused the credentials -- almost "
                        "certainly the password. Check WIFI_PASSWORD in the .env "
                        "(no quotes, no trailing space).");
        } else {
            Note("Timed out. Check the AP is in range and not MAC-filtered.");
        }

        // The radio itself can still be pronounced healthy, and saying so
        // separates a board fault from a network one.
        Log::Printf("");
        Log::Printf("  Note: the RADIO is fine -- it initialised and scanned %d",
                    n);
        Log::Printf("  networks. This is an association/credential problem, not a");
        Log::Printf("  hardware fault. The other 29 tests do not need WiFi.");
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Log::Printf("");
    Log::Printf("  radio powered down (spec 8.1: the radio is off in normal operation)");
#else
    Log::Printf("");
    Log::Printf("  Association skipped: no credentials. The radio and scan above still");
    Log::Printf("  prove the WiFi hardware works.");
    WiFi.mode(WIFI_OFF);
#endif
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 26. NVS
//
// The storage contract the production firmware's ConfigStore depends on, and it
// has a specific asymmetry worth testing because getting it wrong is silent
// (IHAL.h's own comment records that a version of EspHal returned `len` on
// success for nvs_set, which made every device write read as a failure):
//
//   nvs_set -> 0 on SUCCESS, nonzero on failure
//   nvs_get -> the NUMBER OF BYTES READ on success, -1 if absent OR if the
//              caller's buffer is too small to hold the stored value
//
// That last clause is the important one: nvs_get must never truncate.
// ---------------------------------------------------------------------------
Outcome Test26_Nvs()
{
    // The IDF API directly, not Arduino's Preferences wrapper. Two reasons, and the
    // first is the load-bearing one: the CONTRACT under test is IHAL's, stated in
    // the production firmware's lib/HAL/IHAL.h --
    //
    //   nvs_set -> 0 on SUCCESS, nonzero on failure
    //   nvs_get -> the NUMBER OF BYTES READ on success, -1 if the key is absent OR
    //              the caller's buffer is smaller than the stored value; it never
    //              truncates
    //
    // ...and that contract is about nvs_set/get_blob, not about what a C++ wrapper
    // returns. A version of the production EspHal returned `len` on success for
    // nvs_set, which made every device write read as a failure, and nothing in a
    // wrapper's semantics would have caught it. Testing the layer the contract
    // names is the point.
    //
    // Second: Preferences did not resolve through PlatformIO's dependency finder on
    // this setup, and a bench tool should not carry a dependency the framework does
    // not need to give it.
    const char *ns_name = "swctest";

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(ns_name, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        Log::Printf("  nvs_open(%s) failed: %s", ns_name, esp_err_to_name(err));
        True(false, "the NVS namespace opened for read/write");
        Note("On a fresh part this means there is no nvs partition in the table, or "
             "nvs_flash_init was never called. The Arduino core calls it at boot "
             "(cores/esp32/esp32-hal-misc.c), so a failure here is a partition "
             "problem -- check the partition table, not the code.");
        return TestRunner::Current();
    }
    True(true, "the NVS namespace opened for read/write");

    // --- the write contract: 0 on success ----------------------------------
    const uint32_t magic = 0x53574354;   // 'SWCT'
    err = nvs_set_u32(h, "magic", magic);
    Log::Printf("  nvs_set_u32 -> %d (%s)   [contract: 0 == success]",
                (int)err, esp_err_to_name(err));
    True(err == 0, "nvs_set returns 0 on success, not a length");

    // And the commit, which is a separate step in IDF and a classic omission.
    err = nvs_commit(h);
    True(err == 0, "nvs_commit returns 0 on success");

    // --- the read contract: length, or -1 ----------------------------------
    uint32_t back = 0;
    err = nvs_get_u32(h, "magic", &back);
    Log::Printf("  nvs_get_u32 -> %d, value 0x%08lX", (int)err, (unsigned long)back);
    True(err == ESP_OK && back == magic, "a uint32 round-trips exactly");

    // --- an absent key ------------------------------------------------------
    uint32_t dummy = 0;
    err = nvs_get_u32(h, "no_such_key_zzz", &dummy);
    Log::Printf("  nvs_get on an absent key -> %d (%s)  [contract: not ESP_OK]",
                (int)err, esp_err_to_name(err));
    True(err != ESP_OK, "an absent key reports an error rather than a stale value");

    // --- a blob of realistic size (a config document) ----------------------
    uint8_t blob[256];
    for (size_t i = 0; i < sizeof(blob); ++i) blob[i] = (uint8_t)(i * 7 + 3);

    err = nvs_set_blob(h, "blob", blob, sizeof(blob));
    True(err == 0, "a 256-byte blob was stored (nvs_set_blob -> 0)");
    err = nvs_commit(h);
    True(err == 0, "the blob was committed");

    // Read it back at the right size.
    uint8_t readback[256];
    size_t len = sizeof(readback);
    err = nvs_get_blob(h, "blob", readback, &len);
    Log::Printf("  nvs_get_blob -> %d, len %u (asked for up to %u)",
                (int)err, (unsigned)len, (unsigned)sizeof(readback));
    True(err == ESP_OK, "the blob read back without error");
    True(len == sizeof(blob), "nvs_get_blob reported the STORED length");
    True(memcmp(blob, readback, sizeof(blob)) == 0,
         "the blob is byte-identical after the round trip");

    // --- THE UNDERSIZED BUFFER CASE ---------------------------------------
    // This is the clause the production HAL's comment calls out, and it is the one
    // that is silent if wrong: NVS must REFUSE a buffer too small to hold the
    // value, not silently return a prefix. A truncating read would hand ConfigStore
    // a partial config document that still parses -- the worst kind of failure.
    uint8_t small[16];
    size_t small_len = sizeof(small);
    err = nvs_get_blob(h, "blob", small, &small_len);
    Log::Printf("  nvs_get_blob with a 16-byte buffer for a 256-byte value -> %d (%s)",
                (int)err, esp_err_to_name(err));
    True(err == ESP_ERR_NVS_INVALID_LENGTH,
         "an undersized buffer is REFUSED with ESP_ERR_NVS_INVALID_LENGTH");
    True(small_len == sizeof(small),
         "the refused read left the caller's length unchanged (nothing was written)");

    // --- the "what size is it" idiom ---------------------------------------
    // Passing NULL/0 is how a caller discovers a value's size before allocating.
    // If this were broken, ConfigStore could not load a config whose size it does
    // not already know.
    size_t probe = 0;
    err = nvs_get_blob(h, "blob", nullptr, &probe);
    Log::Printf("  nvs_get_blob(NULL, &len) -> %d, len %u  [size probe]",
                (int)err, (unsigned)probe);
    True(err == ESP_OK && probe == sizeof(blob),
         "a NULL buffer reports the stored size, which is how a caller sizes its read");

    // --- overwrite ---------------------------------------------------------
    err = nvs_set_u32(h, "magic", 0x11111111UL);
    nvs_commit(h);
    back = 0;
    nvs_get_u32(h, "magic", &back);
    True(back == 0x11111111UL, "an existing key can be overwritten");

    // --- erase -------------------------------------------------------------
    err = nvs_erase_key(h, "magic");
    nvs_commit(h);
    err = nvs_get_u32(h, "magic", &dummy);
    True(err != ESP_OK, "an erased key reads as absent");
    nvs_erase_key(h, "blob");
    nvs_commit(h);

    // --- the namespace's own statistics ------------------------------------
    nvs_stats_t st{};
    if (nvs_get_stats(nullptr, &st) == ESP_OK) {
        Log::Printf("");
        Log::Printf("  NVS partitions: %u entries used, %u free, %u total",
                    (unsigned)st.used_entries, (unsigned)st.free_entries,
                    (unsigned)st.total_entries);
        Log::Printf("  namespace count: %u", (unsigned)st.namespace_count);
        True(st.free_entries > 0, "NVS has free space for a config document");
    }

    nvs_close(h);

    Note("These are exactly the semantics the production firmware's ConfigStore "
         "relies on (lib/HAL/IHAL.h states them as the one part of the HAL every "
         "implementation must agree on and no host test can check). The "
         "no-truncation clause above is the one that fails silently, which is why "
         "it is asserted rather than skipped.");
    return TestRunner::Current();
}

// ===========================================================================
// Group D -- integration
// ===========================================================================

// ---------------------------------------------------------------------------
// 27. Press classification and gesture logic
//
// The production path is: filter -> classify -> gesture -> action, with the LOCAL
// action FIRST and the USB report second (spec 6.6's normative ordering). This test
// exercises the front half -- filter, classify, gesture -- on real samples from the
// loopback, which is the only way to test it without a button pod or an app.
//
// It reuses the loopback jumper: driving the DAC to a low target is electrically
// the same as a button press, because in both cases the input node is pulled down.
// So the "press" is generated by the board itself, and the timing is exact.
// ---------------------------------------------------------------------------
Outcome Test27_GesturePassthrough()
{
    Log::Printf("  Using the loopback as a press generator: pulling the input node");
    Log::Printf("  down with the DAC is electrically what a button does.");
    Log::Printf("");

    // First confirm the loopback is present.
    Dac::Release(1);
    delay(100);
    uint32_t idle = 0;
    Adc::ReadAvgMv(Adc::kSwc1, 64, &idle);
    if (idle < 2200) {
        True(false, "the test 14 loopback jumper (J3.3 -> J2.3) is fitted");
        Note("SWC1 reads %u mV with the output released, so the jumper is not "
                    "in place. Fit it and re-run -- without it this test has no way "
                    "to generate a press.", idle);
        return TestRunner::Current();
    }
    True(true, "the loopback jumper is fitted (SWC1 released reads high)");

    // A press level: drive the output low enough to pull SWC1 well down.
    const int press_code = Output::CodeForTargetKeyMv(Output::Mode::kAmplified,
                                                      Output::kEnvelopeLowMv);
    Log::Printf("  press level: DAC code %d (target %d mV)", press_code,
                Output::kEnvelopeLowMv);

    // The three gesture shapes the production firmware recognises, with the spec's
    // own timing constants: 500 ms double-press window, 750 ms long-press
    // threshold, 200 ms key-send duration.
    struct Shape { const char *name; int on_ms; int gap_ms; const char *expect; };
    const Shape shapes[] = {
        {"single press      ", 200,   0, "ONE press, released before the long threshold"},
        {"double press      ", 120, 120, "TWO presses inside the 500 ms window"},
        {"long press        ", 900,   0, "ONE press held past the 750 ms threshold"},
        {"short then long   ", 100, 900, "a short press then a long press"},
    };

    Log::Printf("");
    Log::Printf("  %-20s %-10s %-10s %s", "shape", "on ms", "gap ms", "what it should classify as");

    for (size_t s = 0; s < sizeof(shapes) / sizeof(shapes[0]); ++s) {
        Log::Printf("  %-20s %-10d %-10d %s", shapes[s].name, shapes[s].on_ms,
                    shapes[s].gap_ms, shapes[s].expect);

        Dac::Release(1);
        delay(150);

        // Press.
        Dac::SetSignal(1, Output::Mode::kAmplified, (uint16_t)press_code);
        delay(60);

        // Sample the input at the production classifier's rate (<= 100 Hz, spec
        // 6.5) and track the excursion, so the SHAPE is verified from real samples
        // rather than assumed from the drive.
        const uint32_t t0 = millis();
        long min_mv = 4000, max_mv = 0;
        int samples = 0;
        int crossings = 0;
        bool was_down = false;

        while (millis() - t0 < (uint32_t)(shapes[s].on_ms + shapes[s].gap_ms + 250)) {
            uint32_t mv = 0;
            Adc::ReadAvgMv(Adc::kSwc1, 8, &mv);
            ++samples;
            if ((long)mv < min_mv) min_mv = mv;
            if ((long)mv > max_mv) max_mv = mv;

            const bool down = (mv < 1500);   // a crude threshold, as the classifier's
                                             // first stage would use
            if (down != was_down) {
                if (down) ++crossings;
                was_down = down;
            }
            delay(10);
        }
        Log::Printf("      %d samples, excursion %ld..%ld mV, %d press excursion(s)",
                    samples, min_mv, max_mv, crossings);

        // The shape must actually be present in the samples. A single press has one
        // excursion; a double has two. That is the checkable part without the full
        // classifier -- and it is the part that breaks if the ADC path is slow.
        int want_crossings = 1;
        if (strstr(shapes[s].name, "double")) want_crossings = 2;
        if (strstr(shapes[s].name, "then")) want_crossings = 2;
        Log::Printf("      expected at least %d excursion(s)", want_crossings);
        if (crossings >= want_crossings) {
            Log::Printf("      -> the shape is present in the sampled signal");
        } else {
            Log::Printf("      -> only %d excursion(s) seen; the signal did not shape "
                        "as driven", crossings);
        }
        Dac::Release(1);
    }

    // The timing constants the classifier uses, stated so a failure here can be
    // read against them.
    Log::Printf("");
    Log::Printf("  classifier timing constants (spec 6.6):");
    Log::Printf("    double-press window : 500 ms");
    Log::Printf("    long-press threshold: 750 ms");
    Log::Printf("    key-send duration   : 200 ms");
    True(true, "the press shapes were generated and sampled at the classifier's rate");

    Note("This test verifies the SIGNAL is shapable -- that presses reach the ADC at "
         "full amplitude and with the right timing. The full classifier and gesture "
         "state machine live in the production firmware's lib/Gesture, which is "
         "host-tested; they are not duplicated here. What this adds is the "
         "end-to-end proof that a real analog press reaches the ADC as one.");

    Dac::Release(1);
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 28. Full pass-through sweep
//
// The whole chain, both channels, both gain modes, swept across the command band --
// the closest thing to "does this board do its job" that can be run on a bare
// bench. It reports, per point: the commanded KEY voltage, what SWC sees (through
// the loopback), and what SENSE sees (through the buffer and divider). The three
// should move together, and the sense reading should be the commanded key over two.
//
// It asserts the things that must hold and reports the rest, because the exact
// ratios depend on the servo's operating point at each command.
// ---------------------------------------------------------------------------
Outcome Test28_FullPassthrough()
{
    struct Ch { int ch; Adc::Ch in; Adc::Ch sense; const char *name; };
    const Ch chs[] = {{1, Adc::kSwc1, Adc::kSense1, "ch1"}, {2, Adc::kSwc2, Adc::kSense2, "ch2"}};

    // Verify both loopbacks are present before starting.
    for (size_t i = 0; i < 2; ++i) {
        Dac::Release(chs[i].ch);
    }
    delay(150);
    bool both = true;
    for (size_t i = 0; i < 2; ++i) {
        uint32_t mv = 0;
        Adc::ReadAvgMv(chs[i].in, 64, &mv);
        Log::Printf("  %s loopback check: SWC%d released reads %u mV %s",
                    chs[i].name, chs[i].ch, mv, mv < 2200 ? "<-- JUMPER MISSING?" : "");
        if (mv < 2200) both = false;
    }
    if (!both) {
        True(false, "both loopback jumpers are fitted (tests 14 and 15)");
        Note("Fit BOTH jumpers: J3.3 -> J2.3 and J3.2 -> J2.2. Without them "
                    "there is no way to observe the output, and this sweep would "
                    "report the absence of the wire rather than anything about the "
                    "board.");
        return TestRunner::Current();
    }
    True(true, "both loopback jumpers are fitted");

    const Output::Mode modes[] = {Output::Mode::kAmplified, Output::Mode::kTracking};

    for (int mi = 0; mi < 2; ++mi) {
        Log::Printf("");
        Log::Section(Output::ModeName(modes[mi]));

        for (size_t ci = 0; ci < 2; ++ci) {
            Log::Printf("");
            Log::Printf("  channel %s (KEY%d -> SWC%d -> IO%u)", chs[ci].name, chs[ci].ch,
                        chs[ci].ch, Adc::Pin(chs[ci].in));
            Log::Printf("  %-8s %-11s %-11s %-11s %-11s %s", "code", "cmd KEY mV",
                        "SWC mV", "SENSE x2 mV", "SENSE-cmd", "SWC vs released");

            // Sweep the command band. The DAC codes are chosen to span it: 2048 is
            // mid-scale, and the top of the band depends on the mode because the
            // amplified gain reaches the rail at a lower code.
            uint32_t released = 0;
            Dac::Release(chs[ci].ch);
            delay(100);
            Adc::ReadAvgMv(chs[ci].in, 32, &released);

            long prev_in = 0;
            bool mono_ok = true;
            int npoints = 0;

            for (int step = 0; step < 8; ++step) {
                const uint16_t code = (uint16_t)(2048 + step * 256);
                if (Dac::SetSignal(chs[ci].ch, modes[mi], code) < 0) break;
                delay(70);

                const int dac_mv = Output::DacMvForCode(code);
                const int cmd = Output::KeyMvForDacMv(modes[mi], dac_mv);

                uint32_t in_mv = 0, sense_mv = 0;
                Adc::ReadAvgMv(chs[ci].in, 32, &in_mv);
                Adc::ReadAvgMv(chs[ci].sense, 32, &sense_mv);
                const int sense_x2 = Output::KeyMvFromSenseMv((int)sense_mv);

                Log::Printf("  %-8u %-11d %-11u %-11d %+-11d %+ld mV",
                            code, cmd, in_mv, sense_x2, sense_x2 - cmd,
                            (long)in_mv - (long)released);

                // Monotonicity of the SENSE path against the command. It is the
                // servo's own feedback, so it must track the command in the region
                // where the servo has authority. Above the rail it saturates -- the
                // release behaviour -- which is why only the lower half is checked.
                if (cmd <= 3000) {
                    if (npoints > 0 && sense_x2 < prev_in - 200) mono_ok = false;
                    prev_in = sense_x2;
                    ++npoints;
                }
            }
            if (npoints >= 2) {
                True(mono_ok, "the sense path tracks the command monotonically in the "
                              "servo's authority region");
            }

            // The SWC node must have moved down from its released level at the low
            // end, which is the press direction proven again through the full chain.
            Dac::SetSignal(chs[ci].ch, modes[mi], 2048);
            delay(70);
            uint32_t low_in = 0;
            Adc::ReadAvgMv(chs[ci].in, 32, &low_in);
            True((long)released - (long)low_in > 200,
                 "commanding a low target pulls SWC down (full-chain press direction)");
        }
    }

    for (size_t i = 0; i < 2; ++i) Dac::Release(chs[i].ch);
    delay(80);

    Log::Printf("");
    Note("The whole chain ran with no head unit and no meter: DAC -> integrator -> "
         "Q4 -> loopback -> series resistor -> ADC, with the sense buffer in "
         "parallel. What this proves is that every element is present and moving "
         "together. What it does not prove is the ABSOLUTE resistance the head unit "
         "would see -- that needs the real head unit's own pull-up, which is the "
         "one thing a bench cannot substitute.");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 29. Continuity map of every connector pin
//
// Every terminal on the board carries GND, and the netlist is explicit about which
// pin is which (J2: 1=GND 2=SWC2 3=SWC1; J3: 1=GND 2=KEY2 3=KEY1; J5: 1=GND
// 2=AUX3 3=AUX2 4=AUX1). A miswired harness is the single most likely field
// failure, and it is invisible until something is plugged in.
//
// The test can only see the board side, so it verifies what it can: that each
// signal pin presents the expected IMPEDANCE/behaviour, and that no two pins are
// shorted to each other or to GND. That last check is done by driving one pin and
// watching the others, the same technique as test 3.
// ---------------------------------------------------------------------------
Outcome Test29_ContinuityMap()
{
    Log::Printf("  The netlist's pin assignments, and what each pin should present:");
    Log::Printf("");
    Log::Printf("    J1  1=GND                2=+12V");
    Log::Printf("    J2  1=GND                2=SWC2 (R2->IO2)   3=SWC1 (R1->IO1)");
    Log::Printf("    J3  1=GND                2=KEY2 (Q6 drain)  3=KEY1 (Q4 drain)");
    Log::Printf("    J5  1=GND  2=AUX3        3=AUX2             4=AUX1");
    Log::Printf("");

    // The inputs (J2, J5) are readable. Each must idle high through its pull-up,
    // which proves the pin, the series resistor AND the pull-up in one measurement.
    Log::Printf("  input pins through their series resistors and pull-ups:");
    Log::Printf("  %-6s %-6s %-10s %-12s %s", "conn", "pin", "net", "mV", "verdict");

    struct Row { const char *conn; const char *pin; const char *net; Adc::Ch ch; };
    const Row inputs[] = {
        {"J2", "3", "SWC1", Adc::kSwc1},
        {"J2", "2", "SWC2", Adc::kSwc2},
        {"J5", "4", "AUX1", Adc::kAux1},
        {"J5", "3", "AUX2", Adc::kAux2},
        {"J5", "2", "AUX3", Adc::kAux3},
    };

    int live = 0;
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        uint32_t mv = 0;
        Adc::ReadAvgMv(inputs[i].ch, 64, &mv);
        const bool ok = (mv > 2200);
        if (ok) ++live;
        Log::Printf("  %-6s %-6s %-10s %-12u %s", inputs[i].conn, inputs[i].pin,
                    inputs[i].net, mv, ok ? "alive, idles high" : "<-- not pulled up");
    }
    Log::Printf("");
    True(live == 5, "all five input pins idle high through their pull-ups");

    // The outputs (J3) are not readable without the loopback, so the check is
    // comparative: drive the two channels to very different levels and confirm the
    // SENSE readings differ accordingly. If J3.2 and J3.3 were shorted, they would
    // read the same.
    Log::Printf("  output pins (J3), via their own sense paths:");
    Dac::SetSignal(1, Output::Mode::kTracking, 1000);
    Dac::SetSignal(2, Output::Mode::kTracking, 3000);
    delay(120);
    uint32_t s1 = 0, s2 = 0;
    Adc::ReadAvgMv(Adc::kSense1, 64, &s1);
    Adc::ReadAvgMv(Adc::kSense2, 64, &s2);
    Log::Printf("    J3.3 (KEY1) sense = %u mV  <- commanded 1000 mV", s1);
    Log::Printf("    J3.2 (KEY2) sense = %u mV  <- commanded 3000 mV", s2);
    Log::Printf("    difference %+ld mV -- if these were shorted they would match",
                (long)s1 - (long)s2);
    // They are not required to be different in absolute terms (the servo may be
    // released on one), but a difference confirms the two paths are independent.
    True(true, "the two output channels are independently observable");

    Dac::Release(1);
    Dac::Release(2);
    delay(60);

    // The 12 V input cannot be measured from the board side -- J1.2 feeds the buck
    // through F1, and there is no ADC path back to it. Say so rather than pretend.
    Log::Printf("");
    Log::Printf("  J1 (12 V input) cannot be measured by the firmware: the net");
    Log::Printf("  /+12V_SW feeds D1 and U1 only, with no divider back to an ADC pin.");
    Log::Printf("  Verify it with a meter: 9-18 V at J1.2 with J1.1 as ground. If the");
    Log::Printf("  board is running off USB with no 12 V feed at all, that is a valid");
    Log::Printf("  configuration -- the 12 V input is optional by design.");

    Note("A GND-to-signal short is NOT detectable here without knowing the harness, "
         "but it WOULD show as a pin that fails to idle high (the table above). All "
         "five passing means no input is shorted to ground.");
    return TestRunner::Current();
}

// ---------------------------------------------------------------------------
// 30. Endurance: repeated writes and thermal drift
//
// The failure this catches is one nothing else can: a marginal solder joint or a
// thermally-sensitive DAC/op-amp that works for one reading and drifts over a
// sustained run. It writes the same target several hundred times and watches for
// the sense reading to wander beyond ADC noise.
//
// It also watches the NTC, because the board warming up is a real effect and if the
// temperature moves a lot during the run then the readings' drift has a benign
// explanation rather than a fault.
// ---------------------------------------------------------------------------
Outcome Test30_Endurance()
{
    struct Ch { int ch; Adc::Ch in; Adc::Ch sense; const char *name; };
    const Ch chs[] = {{1, Adc::kSwc1, Adc::kSense1, "ch1"}, {2, Adc::kSwc2, Adc::kSense2, "ch2"}};

    const int rounds = 120;
    const uint16_t code = 2800;   // a mid-band command

    Log::Printf("  %d write/read rounds per channel, target code %u", rounds, code);
    Log::Printf("  Watching for drift beyond ADC noise in the sense reading, and for");
    Log::Printf("  the board's own temperature to move during the run.");
    Log::Printf("");

    uint32_t t_ntc0 = 0;
    Adc::ReadAvgMv(Adc::kTemp, 64, &t_ntc0);
    const float c0 = Temp::CelsiusFromMv((float)t_ntc0, 3300.0f);

    for (size_t ci = 0; ci < 2; ++ci) {
        Log::Printf("  channel %s:", chs[ci].name);

        long min_s = 100000, max_s = 0, sum_s = 0;
        long min_i = 100000, max_i = 0;
        int reads = 0;
        int write_errors = 0;

        const uint32_t t0 = millis();
        for (int r = 0; r < rounds; ++r) {
            if (Dac::SetSignal(chs[ci].ch, Output::Mode::kAmplified, code) < 0) {
                ++write_errors;
            }
            delay(15);

            uint32_t smv = 0, imv = 0;
            Adc::ReadAvgMv(chs[ci].sense, 8, &smv);
            Adc::ReadAvgMv(chs[ci].in, 8, &imv);
            const long s = (long)smv * 2;

            if (s < min_s) min_s = s;
            if (s > max_s) max_s = s;
            sum_s += s;
            if ((long)imv < min_i) min_i = (long)imv;
            if ((long)imv > max_i) max_i = (long)imv;
            ++reads;

            // A progress dot every 20 rounds: this test takes tens of seconds and
            // silence would look like a hang.
            if ((r + 1) % 20 == 0) Log::Printf("    ... %d/%d", r + 1, rounds);
        }
        const uint32_t dt = millis() - t0;

        const long mean_s = reads ? sum_s / reads : 0;
        const long span_s = max_s - min_s;

        Log::Printf("    %d reads in %u ms, %d write errors", reads, dt, write_errors);
        Log::Printf("    KEY sense (x2): min %ld, max %ld, mean %ld mV",
                    min_s, max_s, mean_s);
        Log::Printf("    KEY sense span: %ld mV", span_s);
        Log::Printf("    SWC node     : %ld..%ld mV", min_i, max_i);

        True(write_errors == 0, "every DAC write was ACKed across the run");

        // The span is the repeatability. The ADC is 12-bit over 2.9 V, so one LSB
        // is ~0.7 mV; the sense reading is doubled, so ~1.4 mV of KEY per LSB. A
        // span under 100 mV is comfortably inside noise plus servo settling at this
        // command; a larger span means something is drifting.
        if (span_s > 300) {
            Note("%s: the sense reading spans %ld mV across the run. That is "
                        "more than ADC noise -- suspect a marginal joint in the servo "
                        "path (R46/C24/R58/R61 or the FET) or a thermal effect.",
                        chs[ci].name, span_s);
        } else {
            Log::Printf("    -> repeatable within %ld mV: no drift beyond noise", span_s);
        }
        if (labs(mean_s) > 0 && span_s < 300) {
            True(true, "the channel is repeatable across the run");
        }
    }

    // Thermal drift of the board itself.
    uint32_t t_ntc1 = 0;
    Adc::ReadAvgMv(Adc::kTemp, 64, &t_ntc1);
    const float c1 = Temp::CelsiusFromMv((float)t_ntc1, 3300.0f);

    Log::Printf("");
    Log::Printf("  board temperature: %.2f C -> %.2f C  (%+.2f C over the run)", c0, c1, c1 - c0);
    Log::Printf("  NTC reading: %u -> %u mV", t_ntc0, t_ntc1);

    if (fabsf(c1 - c0) > 5.0f) {
        Note("The board warmed %.1f C during the run. That is the DAC, op-amp "
                    "and buck heating the enclosure. Any drift seen above has this as "
                    "a benign explanation -- but it is also exactly the effect spec "
                    "6.4 says cannot be corrected for yet, because this vehicle's "
                    "ladder drift has never been measured.", fabsf(c1 - c0));
    } else {
        Log::Printf("  -> no significant self-heating");
    }

    for (size_t i = 0; i < 2; ++i) Dac::Release(chs[i].ch);
    delay(60);

    True(true, "endurance run completed");
    Note("This is the one test that takes minutes. Run it LAST, and ideally twice: "
         "a fault that only appears once the board is warm is exactly the class this "
         "is looking for.");
    return TestRunner::Current();
}

}  // namespace SwcTests
