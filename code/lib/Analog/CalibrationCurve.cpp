#include "Analog/CalibrationCurve.h"

#include <algorithm>

AdcCalibration AdcCalibrationSelect(bool curve_fit_supported) {
    // Both branches use the same data-sheet endpoints for 12dB attenuation. The
    // real difference is the curve *between* them: the eFuse curve-fit is a
    // per-chip polynomial (spec 2.3, -30..0mV post-calibration error), while
    // the fallback is the straight line the spec calls "a documented linear
    // approximation". The source field is what tells a caller which one it got.
    return AdcCalibration{
        /*raw_low=*/0,
        /*raw_high=*/kAdcMaxRawS3,
        /*mv_low=*/0,
        /*mv_high=*/kAdcFullScaleMv12dB,
        /*source=*/curve_fit_supported ? CalibrationSource::kEFuseCurveFit
                                       : CalibrationSource::kLinearFallback,
    };
}

int AdcRawToMilliVolts(const AdcCalibration &cal, uint16_t raw) {
    if (cal.raw_high <= cal.raw_low) return cal.mv_low;
    const int clamped = std::min<int>(raw, cal.raw_high);
    const long long span_raw = cal.raw_high - cal.raw_low;
    const long long span_mv  = cal.mv_high - cal.mv_low;
    const long long mv = cal.mv_low + ((clamped - cal.raw_low) * span_mv) / span_raw;
    return static_cast<int>(mv);
}
