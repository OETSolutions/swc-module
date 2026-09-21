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

// The MCP4728's 7-bit address is set by its A0/A1/A2 strap pins.
//
// SPEC ITEM N-4: this value is a bring-up MEASUREMENT, not a datasheet fact --
// the strap is set by how the board ties those pins, and the spec lists it as
// open (section 12.1). 0x60 is the all-low default, which is what a board with
// the strap pins grounded produces. The bring-up log's step 2 records the
// address read off the bus; if it differs, change it HERE and nowhere else.
#define SWC_MCP4728_ADDR   0x60

// MCP4728 I2C address (datasheet DS22187E).
//   The Multi-Write *frame bytes* are NOT here. They are built by
//   DacFrame::EncodeSet in DacFrame.h, which is host-testable -- EspHal.cpp is
//   the one lib/ file the host build excludes, so a byte layout written inline
//   in it is checked by no test. An earlier revision kept the command constants
//   here, wrote a FOUR-byte frame inline, and shipped a DAC that was never
//   addressed at all. See DacFrame.h's header comment.
#define SWC_MCP4728_ADDR   0x60

// ADC attenuation. 12 dB is the only setting whose calibrated ceiling is 2.9 V
// (spec 2.1); the 11 dB setting is NOT a substitute for it and the S3 has no
// on-chip DAC, so no analog output pin exists to confuse this with.
#define SWC_ADC_ATTEN_DB_12 3   /* adc_atten_t: ADC_ATTEN_DB_12 */
#define SWC_ADC_BITWIDTH    12  /* adc_bitwidth_t: ADC_BITWIDTH_12 */

// NVS namespace. Must match what ConfigStore expects; it is passed through the
// HAL's nvs_get/nvs_set keys, not open-coded per call site.
#define SWC_NVS_NAMESPACE   "swc"
