// The test registry: the ONE ordered list of tests, read by both front ends.
//
// Adding a test means adding its body (TestsA/B/C/D.cpp) and one row here. There
// is deliberately no second place that enumerates them -- a menu and a web table
// maintained separately is exactly how a test becomes unreachable, or run twice.

#include "TestRunner.h"

// Bodies, from the shared declaration header. Including it rather than
// re-declaring them here is what guarantees the registry and the definitions
// agree -- see TestDecls.h for the link failure that made this necessary.
#include "TestDecls.h"
#include "SetupPrompts.h"
using namespace SwcTests;

namespace {
const TestRunner::Test kTests[] = {
    { 1, "Boot identity, flash, PSRAM and heap",
      "nothing -- this runs on the bare board",
      nullptr, Test01_BootIdentity,
      "U3 module identity, flash size, PSRAM absence, reset reason" },

    { 2, "Power rails, VBUS detect and NTC reference",
      "USB-C or 12 V to J1 (either is enough; the 12 V feed is optional)",
      nullptr, Test02_Rails,
      "/VBUS_VALID (IO10), the 3V3 rail via its dividers, RT1's divider" },

    { 3, "Spare test-point GPIOs and solder bridges",
      "nothing attached to TP1-TP6",
      nullptr, Test03_SpareGpio,
      "IO3/IO11/IO12/IO15/IO16/IO21 (netlist: single-pad nets, test points only)" },

    { 4, "I2C bus scan and MCP4728 address",
      "nothing -- the DAC is on-board",
      nullptr, Test04_I2cScan,
      "I2C on IO17/IO18, R5/R6 pull-ups, U4 strap address (spec item N-4)" },

    { 5, "DAC write and read-back at seven codes",
      "nothing -- read-back needs no meter",
      nullptr, Test05_DacWriteReadback,
      "U4 Multi-Write frame (3 bytes), latch-on-ACK, 12-bit code" },

    { 6, "DAC power-down modes (the gain selector)",
      "nothing",
      nullptr, Test06_DacPowerModes,
      "U4 PD1:PD0 field; the 1k mode is the 5V-range gain selector" },

    { 7, "ADC calibration source and channel stability",
      "nothing",
      nullptr, Test07_AdcCalibration,
      "eFuse curve-fitting availability, 12 dB / 2.9 V ceiling, channel settling" },

    { 8, "Every ADC input is live",
      "a jumper wire, to touch inputs to GND (optional)",
      Setup08_AdcChannels, Test08_AdcChannelsLive,
      "All eight ADC1 inputs and their pull-ups (R15-R19)" },

    { 9, "MCP4728 EEPROM power-on state",
      "nothing",
      nullptr, Test09_DacEeprom,
      "U4 EEPROM contents and the cold-start state they imply (DESIGN 4.6)" },

    { 10, "~LDAC is idle-high and never pulsed",
      "nothing",
      nullptr, Test10_LdacNeverPulsed,
      "IO48 ~LDAC, R13 pulldown, per-channel latching (spec 2.5.1)" },

    { 11, "Sense path: divider, buffer and the ADC ceiling",
      "nothing to J3 (open-circuit is what this test wants)",
      nullptr, Test11_SensePath,
      "U6B/U6D followers, R54/R50 and R55/R51 dividers, SENSE1/SENSE2" },

    { 12, "Output servo, channel 1 (KEY1)",
      "voltmeter on J3 pin 3 (KEY1), black lead to J3 pin 1 (GND)",
      nullptr, Test12_ServoChannel1,
      "U6A integrator, R46/C24, Q4 sink, R58/R61 gain, J3.3" },

    { 13, "Output servo, channel 2 (KEY2)",
      "voltmeter on J3 pin 2 (KEY2), black lead to J3 pin 1 (GND)",
      nullptr, Test13_ServoChannel2,
      "U6C integrator, Q6 sink, R59/R60 gain, J3.2" },

    { 14, "Channel 1 loopback: SWC_OUT1 to SWC_IN1",
      "JUMPER from J3 pin 3 (KEY1) to J2 pin 3 (SWC1)",
      nullptr, Test14_ServoLoopback1,
      "End-to-end: DAC -> servo -> Q4 -> KEY1 -> SWC1 -> R1 -> ADC" },

    { 15, "Channel 2 loopback: SWC_OUT2 to SWC_IN2",
      "JUMPER from J3 pin 2 (KEY2) to J2 pin 2 (SWC2)",
      nullptr, Test15_ServoLoopback2,
      "End-to-end on channel 2 through R2" },

    { 16, "SWC ladder inputs and the press direction",
      "the vehicle's button pod on J2, OR nothing (it reports resting levels)",
      nullptr, Test16_LadderInputs,
      "R1/R2 10k series, R15/R16 10k pull-ups, D4/D5 clamps, C3/C4" },

    { 17, "AUX1-AUX3 inputs",
      "a button or jumper on J5 (optional; it reports resting levels)",
      nullptr, Test17_AuxInputs,
      "R23-R25 1k series, R17-R19 10k pull-ups, D8-D10 clamps, IO4/IO5/IO6" },

    { 18, "NTC temperature sensor",
      "nothing; ideally let the board sit to room temperature first",
      nullptr, Test18_Temperature,
      "RT1 10k B3380, R29 10k, C19 -- and the measured rail in the denominator" },

    { 19, "Buzzer",
      "listen -- you will hear a pattern, and confirm it by ear",
      nullptr, Test19_Buzzer,
      "BZ1 via Q3, R27 100R gate resistor, R28 pulldown, D11 freewheel, IO13" },

    { 20, "Status and second LEDs",
      "watch D6 (/LED_STAT) and D12 (/LED2)",
      nullptr, Test20_Leds,
      "D6 via R7 on IO47, D12 via R26 on IO14, and their polarity" },

    { 21, "Gain-mode auto-selection from the sensed idle",
      "a resistor from J3 KEY1 to GND that mimics a head unit's pull-up",
      nullptr, Test21_GainAutoSelect,
      "Spec 6.2 AUTO: envelope, guard band, and the safe default" },

    { 22, "Servo software trim loop (bounded supervisor)",
      "the test 14 loopback jumper in place",
      nullptr, Test22_ServoTrimLoop,
      "Spec 6.5: 1-2 Hz, +/-1 LSB, deadband, bounded authority, trim DISABLED" },

    { 23, "Idle is high-impedance (the central safety property)",
      "nothing on J3 to start; a 10k-100k resistor to 3V3 for part B",
      Setup23_IdleSafety, Test23_IdleSafety,
      "Spec 6.7 / DESIGN 4.6: release needs no mode; Q4 only sinks" },

    { 24, "USB link (TinyUSB CDC on the native OTG controller)",
      "accept that the serial console will be RE-ASSIGNED during this test",
      nullptr, Test24_UsbLink,
      "Spec 4.1: the OTG controller vs USB-Serial-JTAG on the shared PHY" },

    { 25, "WiFi radio and credentials",
      "the configured access point must be in range and on 2.4 GHz",
      nullptr, Test25_Wifi,
      "Radio, scan, association, DHCP (credentials from the untracked .env)" },

    { 26, "NVS read/write/erase",
      "nothing",
      nullptr, Test26_Nvs,
      "ConfigStore's storage contract: nvs_set 0 on success, nvs_get len or -1" },

    { 27, "Press classification and gesture logic",
      "the loopback jumper (test 14) OR a button on J2.3",
      nullptr, Test27_GesturePassthrough,
      "Filter, classify, single/double/long press (spec 6.6 ordering)" },

    { 28, "Full pass-through sweep across the output envelope",
      "loopback jumpers on BOTH channels (tests 14 and 15)",
      nullptr, Test28_FullPassthrough,
      "The whole signal chain swept in both gain modes" },

    { 29, "Continuity map of every connector pin",
      "jumpers, applied as each step asks",
      nullptr, Test29_ContinuityMap,
      "J1/J2/J3/J5 pin-by-pin against the netlist" },

    { 30, "Endurance: repeated writes and thermal drift",
      "loopback jumpers on both channels; this one takes a minute or two",
      nullptr, Test30_Endurance,
      "Integrator stability, DAC repeatability, NTC drift over a sustained run" },
};
}  // namespace

namespace TestRunner {

static const size_t kCount = sizeof(kTests) / sizeof(kTests[0]);

const Test *AllTests() { return kTests; }
size_t      Count()    { return kCount; }

const Test *Get(size_t index)
{
    return index < kCount ? &kTests[index] : nullptr;
}

}  // namespace TestRunner
