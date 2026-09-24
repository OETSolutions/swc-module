#pragma once

// Every GPIO the board connects, verified against the netlist (spec 2.2).
//
// Do NOT renumber these from memory or from another ESP32 board definition.
// IO4/IO5/IO6 are the AUX analog inputs and are the user-facing programming
// controls; IO0 is the recessed BOOT strapping pin and is recovery only (spec
// 2.2). An earlier revision of the plan carried the AUX pins as IO0/IO1/IO2,
// which would have made the programming button unreachable.

#define SWC_PIN_SWC1_ADC   1    /* ADC1_CH0, /SWC1_ADC */
#define SWC_PIN_SWC2_ADC   2    /* ADC1_CH1, /SWC2_ADC */
#define SWC_PIN_TEMP_ADC   7    /* ADC1_CH6, NTC RT1 divider */
#define SWC_PIN_SENSE1     8    /* ADC1_CH7, /SENSE1 = KEY1 voltage / 2 */
#define SWC_PIN_SENSE2     9    /* ADC1_CH8, /SENSE2 = KEY2 voltage / 2 */
#define SWC_PIN_VBUS_VALID 10   /* D-in, VBUS present via R56/R57 divider */
#define SWC_PIN_BUZZ       13   /* D-out, buzzer Q3 gate via R27 */
#define SWC_PIN_LED2       14   /* D-out, D12 via R26 */
#define SWC_PIN_LED_STAT   47   /* D-out, D6 via R7 */
#define SWC_PIN_I2C_SDA    17   /* I2C to U4 MCP4728 */
#define SWC_PIN_I2C_SCL    18   /* I2C to U4 MCP4728 */
#define SWC_PIN_DAC_LDAC_B 48   /* D-out, MCP4728 ~LDAC, R13 10k pulldown */
#define SWC_PIN_BOOT       0    /* strapping; recovery only -- never hold at power-on */
#define SWC_PIN_AUX1       4    /* ADC1_CH3 -- the programming button (spec 7.5) */
#define SWC_PIN_AUX2       5    /* ADC1_CH4 */
#define SWC_PIN_AUX3       6    /* ADC1_CH5 */

// I2C.
#define SWC_I2C_FREQ_HZ    400000

// The MCP4728's 7-bit address (datasheet DS22187E), set by its A0/A1/A2 strap
// pins. ONE definition: the bring-up notes below say to change it "here and
// nowhere else", and a second copy of this line made that untrue -- a bring-up
// address correction would have landed on one and been silently overridden by
// the other.
//
// CONFIRMED ON THE BOARD 2026-09-24 (spec item N-4, closed). No bus scan was
// needed: the firmware's boot-time verify reads each channel's code back from
// the MCP4728 (N-21), and the DUT reports `output_safe: true`, which is only set
// when that real I2C transaction to THIS address returns valid bytes -- so 0x60
// is the strap this board actually has. A mis-strapped respin would fail
// `output_safe` rather than silently address nothing. Change it HERE and nowhere
// else if a future board differs.
//
// The Multi-Write *frame bytes* are NOT here. They are built by
// DacFrame::EncodeSet in DacFrame.h, which is host-testable -- EspHal.cpp is
// the one lib/ file the host build excludes, so a byte layout written inline
// in it is checked by no test. An earlier revision kept the command constants
// here, wrote a FOUR-byte frame inline, and shipped a DAC that was never
// addressed at all. See DacFrame.h's header comment.
#define SWC_MCP4728_ADDR   0x60

// ADC attenuation. 12 dB is the only setting whose calibrated ceiling is 2.9 V
// (spec 2.1); the 11 dB setting is NOT a substitute for it and the S3 has no
// on-chip DAC, so no analog output pin exists to confuse this with.
#define SWC_ADC_ATTEN_DB_12 3   /* adc_atten_t: ADC_ATTEN_DB_12 */
#define SWC_ADC_BITWIDTH    12  /* adc_bitwidth_t: ADC_BITWIDTH_12 */

// NVS namespace. Must match what ConfigStore expects; it is passed through the
// HAL's nvs_get/nvs_set keys, not open-coded per call site.
#define SWC_NVS_NAMESPACE   "swc"
