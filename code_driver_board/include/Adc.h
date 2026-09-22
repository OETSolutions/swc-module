#pragma once

// ADC, on the ESP-IDF driver directly rather than through Arduino's wrapper.
//
// WHY NOT analogReadMilliVolts(): it does the right thing internally (it builds
// the curve-fitting eFuse handle and calls adc_cali_raw_to_voltage, verified in
// cores/esp32/esp32-hal-adc.c), but it hides two facts this tool exists to
// MEASURE and report:
//
//   1. Whether the eFuse calibration was actually available. On a blank-eFuse
//      module batch adc_cali_create_scheme_curve_fitting() returns
//      ESP_ERR_NOT_SUPPORTED and the core logs and falls back -- but it reports
//      nothing to the caller, and the difference is hundreds of millivolts at the
//      top of the range. The spec requires the fallback to be REPORTED, not silent
//      (spec 3.2). A calibration test that cannot see the fallback cannot test it.
//   2. The raw count, which is what actually tells you a pin is shorted, floating
//      or clamped. Every diagnostic here prints raw and mV together.
//
// So this wraps the same underlying IDF calls and keeps both halves visible.

#include <stdint.h>
#include <stddef.h>

namespace Adc {

struct Channel {
    uint8_t pin;
    const char *name;
};

// The eight ADC1 inputs the board connects, in a fixed order so the report table
// and the web UI agree. Order matches BoardPins.h.
enum Ch {
    kSwc1 = 0,
    kSwc2,
    kTemp,
    kSense1,
    kSense2,
    kAux1,
    kAux2,
    kAux3,
    kCount,
};

const char *Name(Ch ch);
uint8_t     Pin(Ch ch);

// Bring up ADC1 at 12 dB / 12-bit and build the per-chip calibration curve.
// Returns true if the eFuse curve-fitting scheme was available; false means the
// documented linear fallback is in use and every reading is degraded. Records the
// result so CalibrationDegraded() can report it later.
bool Begin();

// True when the eFuse calibration was NOT available and the fallback is in use.
bool CalibrationDegraded();

// The fallback path's description, for the log and the web page.
const char *CalibrationSourceName();

// One conversion, calibrated. Returns false on a driver error (in which case
// *out_mv is 0). The raw count is returned through *out_raw when non-null.
bool ReadMv(Ch ch, uint32_t *out_mv, uint16_t *out_raw = nullptr);

// Average `samples` conversions, discarding nothing -- a boxcar is right here
// because these are DC levels and the only noise is ADC noise. The spec caps the
// sense sample rate at <= 100 Hz with 16-64 oversamples averaged (6.5); the servo
// test honours that by pacing its own loop, not by slowing this down.
bool ReadAvgMv(Ch ch, int samples, uint32_t *out_mv, uint16_t *out_raw = nullptr);

// The raw count with no calibration applied -- for the short/floating diagnostics,
// where the question is "is this pin nailed to a rail" and calibration only
// obscures it.
bool ReadRaw(Ch ch, uint16_t *out_raw);

}  // namespace Adc
