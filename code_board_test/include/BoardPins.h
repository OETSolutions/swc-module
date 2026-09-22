#pragma once

// Every GPIO the board connects, and every analog constant the tests need.
//
// PROVENANCE: each pin below was read out of `swc_module_pcb/production/netlist.ipc`
// (IPC-D-356, the netlist KiCad generated from the live PCB) as the `U3` pad -> net
// assignment, and cross-checked against the firmware spec's section 2.2 pin map and
// `code/lib/HAL/PinMap.h`. Where any of those three ever disagree, the netlist wins.
//
// Do NOT renumber these from memory or from another ESP32-S3 board's definition.
// The S3's GPIO-to-ADC1-channel mapping is NOT the identity -- IO7 is ADC1_CH6 and
// IO1 is ADC1_CH0 -- so arithmetic here would silently misread one pin.

#include <stdint.h>

// ---------------------------------------------------------------------------
// Analog inputs (all ADC1 -- spec 2.2; ADC2 is unusable while WiFi is up, and
// WiFi is on for most of this tool's life)
// ---------------------------------------------------------------------------
#define PIN_SWC1_ADC      1    // /SWC1_ADC   ADC1_CH0, ladder 1, R1 10k series
#define PIN_SWC2_ADC      2    // /SWC2_ADC   ADC1_CH1, ladder 2, R2 10k series
#define PIN_TEMP_ADC      7    // /TEMP_ADC   ADC1_CH6, NTC RT1 divider
#define PIN_SENSE1        8    // /SENSE1     ADC1_CH7, KEY1 voltage / 2
#define PIN_SENSE2        9    // /SENSE2     ADC1_CH8, KEY2 voltage / 2
#define PIN_AUX1          4    // /AUX1_F     ADC1_CH3, 1k series, R17 10k pull-up
#define PIN_AUX2          5    // /AUX2_F     ADC1_CH4, 1k series, R18 10k pull-up
#define PIN_AUX3          6    // /AUX3_F     ADC1_CH5, 1k series, R19 10k pull-up

// ---------------------------------------------------------------------------
// Digital
// ---------------------------------------------------------------------------
#define PIN_VBUS_VALID   10    // D-in,  VBUS present via R56/R57 divider
#define PIN_BUZZ         13    // D-out, buzzer Q3 gate via R27 100R
#define PIN_LED2         14    // D-out, D12 via R26 1k
#define PIN_LED_STAT     47    // D-out, D6 via R7 1k
#define PIN_I2C_SDA      17    // I2C to U4 MCP4728
#define PIN_I2C_SCL      18    // I2C to U4 MCP4728
#define PIN_DAC_LDAC_B   48    // D-out, MCP4728 ~LDAC, R13 10k pulldown
#define PIN_BOOT          0    // STRAPPING. Recovery only -- never hold low at power-on.

// Broken out to test points only -- no header, no user-facing function (spec 2.2).
// They are here so the continuity test can exercise them and so the spare-pin test
// has one definition of "spare" rather than a list written at the call site.
#define PIN_SPARE_IO3     3    // TP1
#define PIN_SPARE_IO11   11    // TP2
#define PIN_SPARE_IO12   12    // TP3
#define PIN_SPARE_IO15   15    // TP4
#define PIN_SPARE_IO16   16    // TP5
#define PIN_SPARE_IO21   21    // TP6
#define PIN_SPARE_TXD0   43    // TP7, ROM UART0 TX
#define PIN_SPARE_RXD0   44    // TP8, ROM UART0 RX

// ---------------------------------------------------------------------------
// The AUX stimulus pins -- TEST RIG, not board hardware.
// ---------------------------------------------------------------------------
// Test 31 exercises AUX1-AUX3 by pulling each one to GND, and it used to do that by
// asking the operator to fit a jumper for every input, twice. That was slow, fiddly,
// and produced six prompts per run. With a wire from each AUX input to one of these
// spare test points, the board drives its own stimulus and the test runs unattended.
//
// The wiring AS FITTED ON THE BENCH (measured, not assumed):
//
//     J5.4 (AUX1)  <->  IO43 (TP7)
//     J5.3 (AUX2)  <->  IO21 (TP6)
//     J5.2 (AUX3)  <->  IO16 (TP5)
//
// THE ORDER DOES NOT MATTER. Test 31 discovers which spare pin reaches which AUX
// input before it measures anything, so any input may be wired to any test point --
// the constants below are the SET of points, not a pairing. An earlier version
// hard-coded the pairing and reported "no response" for two perfectly healthy lines
// when the wires went the other way.
//
// DRIVING LOW simulates the button/short (the input is pulled to GND through the
// wire). FLOATING (INPUT, no pull) simulates it being open, which is the released
// state -- the board's own 10k pull-up then sets the level, exactly as it does with
// nothing attached.
//
// IO43 is TP7, traditionally "UART0 TX". This build sets ARDUINO_USB_MODE=1, so the
// console is the USB peripheral and UART0 is free -- verified in platformio.ini.
// Driving it is safe HERE and would not be on a build using the UART console.
#define PIN_AUX_STIM1    16    // TP5, wired to J5.4 (AUX1)
#define PIN_AUX_STIM2    21    // TP6, wired to J5.3 (AUX2)
#define PIN_AUX_STIM3    43    // TP7, wired to J5.2 (AUX3)

// How AUX_STIMx relates to the AUX channels, in one place.
#define AUX_STIM_FOR(idx) ((idx) == 0 ? PIN_AUX_STIM1 : \
                           (idx) == 1 ? PIN_AUX_STIM2 : PIN_AUX_STIM3)

// ---------------------------------------------------------------------------
// LED polarity, and why it is a constant rather than an inline HIGH/LOW
// ---------------------------------------------------------------------------
// The netlist shows D6/D12 with cathode on the LED net and anode off it, and the
// series resistors R7/R26 return to /LED_STAT and /LED2 -- i.e. the GPIO drives
// the resistor into the ANODE and the cathode is tied to GND (net "ND" is the
// unconnected/GND-side pad here, as it is for R50, R57, RT1 and R57's divider
// returns). So the port pin HIGH lights the LED. That is also the polarity the
// board's own bring-up assumes.
//
// If a future board revision flips this, flip it HERE -- the LED test toggles both
// ways, so a wrong polarity fails the test loudly rather than lighting nothing and
// being read as a dead LED.
#define LED_ON  1
#define LED_OFF 0

// ---------------------------------------------------------------------------
// I2C / DAC
// ---------------------------------------------------------------------------
#define I2C_FREQ_HZ       400000
// The MCP4728's 7-bit address, set by its A0/A1/A2 strap pins. 0x60 is the
// all-low default (spec item N-4 calls this a bring-up MEASUREMENT, not a
// datasheet fact). Test 4 scans the bus first and reports what it actually finds,
// so a board strapped differently is diagnosed rather than assumed.
#define MCP4728_ADDR      0x60

// ---------------------------------------------------------------------------
// The output servo, per channel. Every number here comes from DESIGN.md 4.4 and
// spec 2.3/6.2 and is verified against the netlist's resistor values:
//
//   V_KEY = (1 + R58/R61)*V_DAC - (R58/R61)*V_ADJ
//
// R58 = 82k, R61 = 100k -> R58/R61 = 0.82 exactly -> gain 1.82. Use the RATIO,
// never a rounded 1.812: the spec calls the decimal a misreading because it drifts
// across the envelope.
// ---------------------------------------------------------------------------
#define SERVO_R58_KOHM    82
#define SERVO_R61_KOHM    100

// The op-amp (U6) runs on +5 V, so V_buf cannot exceed ~4.98 V. Gain 1.82
// advertises 6.0 V but the rail binds first -- the surplus is deliberate RELEASE
// margin, not a bug (DESIGN.md 4.4).
#define SERVO_RAIL_MV     4980

// DAC full scale: U4 runs on +3V3 with VREF = VDD, so 0-3.3 V FS, 1 LSB = 806 uV.
// It is NOT the 4.096 V internal-reference mode (that needs VDD >= 4.096 V).
#define DAC_VREF_MV       3300
#define DAC_MAX_CODE      4095

// Spec 6.2's envelope and guard band. Outside 1.80-5.20 V measured at the head
// unit's KEY line there is no head unit. Between 2.6 and 3.4 V the two ranges are
// indistinguishable, so the spec says do NOT guess.
#define KEY_ENVELOPE_LOW_MV   1800
#define KEY_ENVELOPE_HIGH_MV  5200
#define KEY_GUARD_LOW_MV      2600
#define KEY_GUARD_HIGH_MV     3400

// Spec 6.2: a command target must stay this far BELOW the head unit's own measured
// idle. Above that point the sink FET can only be turned off, which is release,
// not a command.
#define KEY_COMMAND_HEADROOM_MV 200

// The sense divider is an exact /2 (R54/R50 = 10k/10k). Because the op-amp rail
// (4.98 V) binds before the ADC ceiling (2.9 V), the sense node can never exceed
// ~2.49 V and the ADC can never saturate -- so no clamp logic is needed anywhere.
#define SENSE_DIVIDER_NUM 1
#define SENSE_DIVIDER_DEN 2

// ---------------------------------------------------------------------------
// ADC
// ---------------------------------------------------------------------------
// 12 dB is the ONLY attenuation whose calibrated ceiling is 2.9 V. The 11 dB
// setting is not a substitute, and the S3 has no on-chip DAC so there is no output
// pin to confuse this with.
#define ADC_CEILING_MV_12DB 2900
#define ADC_RAW_MAX         4095

// ---------------------------------------------------------------------------
// Ladder inputs (spec 2.4 / DESIGN.md 4.1)
// ---------------------------------------------------------------------------
// THE topology, stated once because it has been got backwards before: the ladder's
// common is tied to GND and a button SHUNTS the node it sits at to that common, so
// a press pulls the input DOWN. Idle (no button) is the HIGH state, set by the
// pull-up. The pull-up is 10k to +3V3 (R15/R16 for SWC; R17/R18/R19 for AUX) --
// confirmed as net "3V3" in the netlist, and NOT 12 V as an earlier revision of
// DESIGN.md 4.1 claimed.
//
//   V_pin = 3.3 * R_ladder / (R_ladder + R_pullup)
//
// SWC series resistors are 10k (R1/R2); AUX are 1k (R23/R24/R25). The higher AUX
// series resistance with the same pull-up is why the AUX idle sits a little higher.
#define LADDER_PULLUP_OHM    10000
#define SWC_SERIES_OHM       10000
#define AUX_SERIES_OHM        1000

// The rail the pull-ups divide against. 3300 mV nominal; the spec's bring-up step 3
// sweeps it 3.14 -> 3.47 V, which is why every ladder report prints the measured
// 3V3 estimate alongside the pin voltage rather than assuming 3.30.
#define VRAIL_NOMINAL_MV     3300

// ---------------------------------------------------------------------------
// NTC RT1 -- 10k B3380, in a divider with R29 10k to +3V3, C19 100nF filter.
//   R_ntc = R29 * V_ntc / (V_rail - V_ntc)
//   T(K)  = 1 / (1/T0 + ln(R/R0)/B)     with R0 = 10k at T0 = 298.15 K (25 C)
// ---------------------------------------------------------------------------
#define NTC_R0_OHM       10000
#define NTC_T0_KELVIN    298.15f
#define NTC_BETA         3380.0f
#define NTC_SERIES_OHM   10000   // R29

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------
#define VBUS_VALID_DIVIDER_RATIO 2   // R56/R57 = 10k/10k, so the pin sees VBUS/2
#define V12_MIN_SANE_MV  9000        // below this the 12 V feed is absent/flat
#define V12_MAX_SANE_MV 18000        // above this D1 (SMBJ18A) should be clamping
#define V5_MIN_SANE_MV   4300        // the +5 V rail after the OR-ing diodes
#define V5_MAX_SANE_MV   5500
#define V33_MIN_SANE_MV  3150        // AMS1117-3.3 tolerance
#define V33_MAX_SANE_MV  3450
