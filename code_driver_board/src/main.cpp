#include <Arduino.h>

#include "Adc.h"
#include "BoardPins.h"
#include "Driver.h"
#include "KeyLine.h"
#include "Log.h"

// The swc_logic library is pulled in from src/ (see the same include in
// Driver.cpp): PlatformIO's dependency finder follows src/ includes, not ones
// reached only through include/ headers.
#include "DriverHarness.h"
#include "swc_logic/Output.h"

// The rig driver board: it PRESENTS ladder voltages and a programming-switch
// state to the device under test, so the DUT firmware can be exercised end to end
// with no vehicle and no steering-wheel pod.
//
// This is a bench instrument, not the product. Read README.md for the wiring and
// the command list.
//
// Everything is single-threaded: a synthetic gesture is a real-time sequence of
// DAC writes and waits, and it must not be interleaved with anything else -- the
// DUT judges the timing, so a stutter in the driver is a wrong gesture.

static void PrintMenu()
{
    Log::Printf("");
    Log::Rule('=');
    Log::Printf("  RIG DRIVER -- commands");
    Log::Rule('=');
    Log::Printf("  %-4s %s", "cmd", "action");
    Log::Printf("  %-4s %s", "1", "present a SINGLE press on the selected channel");
    Log::Printf("  %-4s %s", "2", "present a DOUBLE press");
    Log::Printf("  %-4s %s", "3", "present a LONG press");
    Log::Printf("  %-4s %s", "p", "hold the key down (press and stay down)");
    Log::Printf("  %-4s %s", "r", "release the key line");
    Log::Printf("  %-4s %s", "l", "LEARN: close AUX1 (programming switch) + hold the key");
    Log::Printf("  %-4s %s", "L", "hold AUX1 ~3 s alone (open the maintenance window)");
    Log::Printf("  %-4s %s", "a", "toggle the AUX1 programming switch by hand");
    Log::Printf("  %-4s %s", "c", "toggle the selected DUT channel (1 <-> 2)");
    Log::Printf("  %-4s %s", "m", "cycle the gain mode (amplified / tracking)");
    Log::Printf("  %-4s %s", "M", "auto-select the mode from the released level");
    Log::Printf("  %-4s %s", "+/-", "nudge the commanded key level by 50 mV");
    Log::Printf("  %-4s %s", "d", "diagnostic: what the KEY line rests at, and reachability");
    Log::Printf("  %-4s %s", "w", "watch the DUT KEY output (loopback) for ~2 s");
    Log::Printf("  %-4s %s", "s", "status");
    Log::Printf("  %-4s %s", "?", "this menu");
    Log::Rule('=');
    Log::Printf("");
}

static void PrintStatus()
{
    Log::Section("RIG STATUS");
    Log::Printf("  channel          : %d", Driver::Channel());
    Log::Printf("  gain mode        : %s (gain %d/%d)",
                Output::ModeName(Driver::Mode()),
                Output::GainNum(Driver::Mode()), Output::GainDen(Driver::Mode()));
    Log::Printf("  commanded key    : %d mV", Driver::Rig().key_mv);
    const Harness::RigProblem v = Harness::Validate(Driver::Rig());
    Log::Printf("  timing config    : %s", Harness::ProblemName(v));
    Log::Printf("    press_on %u ms  inter_press %u ms  long %u ms  double_window %u ms",
                (unsigned)Driver::Rig().press_on_ms,
                (unsigned)Driver::Rig().inter_press_ms,
                (unsigned)Driver::Rig().long_press_ms,
                (unsigned)Driver::Rig().double_window_ms);
    Log::Printf("  AUX1 switch      : %s", Driver::AuxClosed() ? "CLOSED (pin low)" : "open (high-Z)");
    int mv = 0;
    if (Driver::ReadKeyMv(&mv)) {
        Log::Printf("  KEY line now     : %d mV", mv);
    } else {
        Log::Printf("  KEY line now     : (ADC read failed)");
    }
    Log::Printf("  released level   : ch1 %d mV, ch2 %d mV",
                KeyLine::FloatMv(1), KeyLine::FloatMv(2));
}

static void PrintDiagnostic()
{
    Log::Section("DIAGNOSTIC -- is the rig wired and reachable?");
    for (int ch = 1; ch <= 2; ++ch) {
        const int idle = KeyLine::FloatMv(ch);
        int now = 0;
        const bool ok = Driver::ReadKeyMv(&now);   // reads the SELECTED channel
        Log::Printf("  channel %d: released level %d mV", ch, idle);
        if (ch == Driver::Channel()) {
            Log::Printf("             now reads %d mV%s", now, ok ? "" : " (read failed)");
        }
        // The reachable band is everything below the released level, floored by
        // the servo envelope's low edge.
        const int reachable_hi = idle - Output::kCommandHeadroomMv;
        if (reachable_hi <= Output::kEnvelopeLowMv) {
            Log::Printf("             NO usable command band below %d mV "
                        "(is the DUT's pull-up present and is the wire attached?)",
                        Output::kEnvelopeLowMv);
        } else {
            Log::Printf("             commands reachable from %d mV up to %d mV",
                        Output::kEnvelopeLowMv, reachable_hi);
        }
    }
    Log::Printf("");
    // The LOOPBACK: the DUT's SWC out is wired to this board's SWC in, so the
    // driver's own ladder input reads back what the DUT is commanding on its KEY
    // line. This is the one place the rig can see the DUT's OUTPUT rather than
    // only its frames -- and it is how a "the DUT emitted an event but nothing
    // reached the head unit" fault is told from a reporting bug.
    uint32_t dut1 = 0, dut2 = 0;
    const bool l1 = Adc::ReadAvgMv(Adc::kSwc1, 16, &dut1);
    const bool l2 = Adc::ReadAvgMv(Adc::kSwc2, 16, &dut2);
    Log::Printf("  DUT KEY out (loopback) : ch1 %s%u mV, ch2 %s%u mV",
                l1 ? "" : "?", (unsigned)dut1, l2 ? "" : "?", (unsigned)dut2);
    Log::Printf("      (near the 3.3 V rail = the DUT is RELEASED; a low value = it is");
    Log::Printf("       driving a key. The DUT holds its line high when it has no head");
    Log::Printf("       unit to command -- see spec 6.2's envelope.)");
    Log::Printf("");
    Log::Printf("  A released level near 3.3 V says the DUT's 10k pull-up is doing its");
    Log::Printf("  job and the wire is attached. Near 0 V says the line is shorted or");
    Log::Printf("  nothing is driving it -- check the J3->J2 wiring and the common ground.");
}

// The learn entry the DUT's spec 7.4 describes: the programming switch (AUX1)
// held, then `slot` selection presses, then the key held while the DUT samples.
// The whole sequence lives in Driver::Learn, so the menu is only the prompt.
static void DoLearnPrompt()
{
    Log::Prompt("  which slot (1-8)? ");
    const uint32_t t0 = millis();
    int slot = 1;
    while (millis() - t0 < 4000) {
        if (Serial.available()) {
            const int c = Serial.read();
            if (c >= '1' && c <= '8') { slot = c - '0'; break; }
            if (c == '\r' || c == '\n') break;
        }
        delay(10);
    }
    Log::Printf("%d", slot);
    Driver::Learn(slot);
}

// Watch the LOOPBACK (this board's SWC in = the DUT's KEY out) for a couple of
// seconds and report min/max/mean, so a key pulse the DUT drives for only its
// send_duration is not missed by a one-shot read.
static void WatchLoopback()
{
    Log::Section("WATCH -- the DUT's KEY output (loopback), ~2 s");
    const uint32_t t0 = millis();
    uint32_t min1 = 0xFFFFFFFF, max1 = 0, sum1 = 0, n1 = 0;
    uint32_t min2 = 0xFFFFFFFF, max2 = 0, sum2 = 0, n2 = 0;
    while (millis() - t0 < 2000) {
        uint32_t v1 = 0, v2 = 0;
        if (Adc::ReadMv(Adc::kSwc1, &v1)) {
            if (v1 < min1) min1 = v1;
            if (v1 > max1) max1 = v1;
            sum1 += v1; ++n1;
        }
        if (Adc::ReadMv(Adc::kSwc2, &v2)) {
            if (v2 < min2) min2 = v2;
            if (v2 > max2) max2 = v2;
            sum2 += v2; ++n2;
        }
        delay(5);
    }
    Log::Printf("  ch1: min %u  max %u  mean %u  (%u samples)",
                (unsigned)min1, (unsigned)max1, n1 ? (unsigned)(sum1 / n1) : 0, (unsigned)n1);
    Log::Printf("  ch2: min %u  max %u  mean %u  (%u samples)",
                (unsigned)min2, (unsigned)max2, n2 ? (unsigned)(sum2 / n2) : 0, (unsigned)n2);
    Log::Printf("  A max WELL below the released level means the DUT IS driving a key");
    Log::Printf("  during the window. A flat trace at the rail means it is released.");
}

static void ServiceSerial()
{
    if (!Serial.available()) return;
    const int c = Serial.read();

    switch (c) {
        case '1':
            Driver::Play(Harness::BuildSingle(Driver::Rig()), "SINGLE press");
            break;
        case '2':
            Driver::Play(Harness::BuildDouble(Driver::Rig()), "DOUBLE press");
            break;
        case '3':
            Driver::Play(Harness::BuildLong(Driver::Rig()), "LONG press");
            break;
        case 'p': {
            Log::Printf("press and HOLD (use 'r' to release)");
            const int code = Driver::DriveKeyMv(Driver::Rig().key_mv);
            Log::Printf("  key driven at %d mV (code %d)", Driver::Rig().key_mv, code);
            break;
        }
        case 'r':
            Driver::Release();
            Log::Printf("key released");
            break;
        case 'l':
            DoLearnPrompt();
            break;
        case 'L':
            Driver::OpenMaintenanceWindow();
            break;
        case 'a':
            Driver::SetAuxClosed(!Driver::AuxClosed());
            Log::Printf("AUX1 switch: %s", Driver::AuxClosed() ? "CLOSED" : "open");
            break;
        case 'c':
            Driver::SelectChannel(Driver::Channel() == 1 ? 2 : 1);
            Log::Printf("selected channel %d", Driver::Channel());
            break;
        case 'm': {
            const Output::Mode next = (Driver::Mode() == Output::Mode::kAmplified)
                                          ? Output::Mode::kTracking
                                          : Output::Mode::kAmplified;
            Driver::SetMode(next);
            Log::Printf("gain mode: %s", Output::ModeName(next));
            break;
        }
        case 'M':
            Driver::AutoSelectMode();
            break;
        case '+': {
            Harness::RigConfig r = Driver::Rig();
            r.key_mv += 50;
            Driver::SetRig(r);
            Log::Printf("commanded key: %d mV", r.key_mv);
            break;
        }
        case '-': {
            Harness::RigConfig r = Driver::Rig();
            r.key_mv -= 50;
            Driver::SetRig(r);
            Log::Printf("commanded key: %d mV", r.key_mv);
            break;
        }
        case 'd':
            PrintDiagnostic();
            break;
        case 'w':
            WatchLoopback();
            break;
        case 's': case '\n': case '\r':
            PrintStatus();
            break;
        case '?':
            PrintMenu();
            break;
        default:
            break;
    }
}

static bool s_greeted = false;

void setup()
{
    Log::Begin();
    Log::Rule('*');
    Log::Printf("  SWC RIG DRIVER -- presents ladder + AUX stimuli to the DUT");
    Log::Printf("  console: ROM USB-Serial-JTAG  |  this is a BENCH TOOL, not the product");
    Log::Rule('*');

    Adc::Begin();
    Log::Printf("ADC bring-up: %s", Adc::CalibrationSourceName());

    Driver::Begin();
    PrintMenu();
}

void loop()
{
    if (!s_greeted && Serial.available()) {
        s_greeted = true;
        Log::Printf("(a monitor just attached -- here is the menu)");
        PrintMenu();
    }
    ServiceSerial();
    delay(2);
}
