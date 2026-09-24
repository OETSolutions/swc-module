#pragma once

#include <stdint.h>

constexpr int kAdcMaxRawS3       = 4095;
// Spec 3.2's `MilliVolt` bound: the calibrated ceiling of any pin reading at
// 12 dB attenuation. This is the ONE home for that fact. It was spelled three
// times -- here, and as an anonymous-namespace `kAdcCeilingMv` in BOTH
// `LadderDecode.cpp` and `LearnSession.cpp` -- with nothing comparing them, so a
// re-tune of one would silently disagree with the ADC's own endpoint. Both files
// now read this through `LadderDecode.h`, which includes it.
constexpr int kAdcFullScaleMv12dB = 2900;

// Spec 3.2: curve-fitting calibration is eFuse-backed and per-chip. It is NOT
// universally available -- adc_cali_create_scheme_curve_fitting() returns
// ESP_ERR_NOT_SUPPORTED on modules with blank eFuses, and the spec requires the
// firmware to fall back to a documented linear approximation *and report that
// it did*, rather than silently mis-scaling every reading.
//
// **`source` is now the SINGLE home for which curve is in use (N-64 resolved).**
// EspHal reports `g_state.curve.source` in `HalCalibrationDegraded` /
// `EspHalCalibrationIsDegraded` and its parallel `cali_degraded` bool is gone, so
// there is one derivation of "the calibrated curve or the fallback", not two that
// could drift. (An earlier comment here wrongly claimed the report went through
// this field while it did not; that duplication was N-64.)
enum class CalibrationSource { kEFuseCurveFit, kLinearFallback };

// Two-point calibration, which is the shape the ESP-IDF curve-fit calibration
// exposes after its own polynomial stage. Keeping the curve's *effect* behind
// this interface is what lets the host tests run without the eFuse.
struct AdcCalibration {
    uint16_t          raw_low;
    uint16_t          raw_high;
    int               mv_low;
    int               mv_high;
    CalibrationSource source;
};

// Chooses the curve for this chip. Task 14 passes the real
// adc_cali_create_scheme_curve_fitting() return code; the host tests pass both
// values to exercise the fallback without an eFuse.
AdcCalibration AdcCalibrationSelect(bool curve_fit_supported);

int AdcRawToMilliVolts(const AdcCalibration &cal, uint16_t raw);
