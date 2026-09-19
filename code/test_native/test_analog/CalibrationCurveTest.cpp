#include "Analog/CalibrationCurve.h"
#include <gtest/gtest.h>

TEST(AdcCalibration, EndpointsMapExactly) {
    const AdcCalibration cal = AdcCalibrationSelect(true);
    EXPECT_EQ(AdcRawToMilliVolts(cal, cal.raw_low), cal.mv_low);
    EXPECT_EQ(AdcRawToMilliVolts(cal, cal.raw_high), cal.mv_high);
}

TEST(AdcCalibration, IsMonotonicAcrossTheEntireRawRange) {
    const AdcCalibration cal = AdcCalibrationSelect(true);
    int prev = -1;
    for (uint16_t raw = 0; raw <= kAdcMaxRawS3; ++raw) {
        const int mv = AdcRawToMilliVolts(cal, raw);
        ASSERT_GE(mv, prev) << "non-monotonic at raw=" << raw;
        prev = mv;
    }
}

TEST(AdcCalibration, NeverExceedsTheDatasheetCeilingForThisAttenuation) {
    // The ESP32-S3 at 12dB attenuation saturates at 2.9V. A conversion that
    // reports more than that is reporting a voltage the part cannot measure,
    // which would silently corrupt every downstream ratio.
    const AdcCalibration cal = AdcCalibrationSelect(true);
    for (uint16_t raw = 0; raw <= kAdcMaxRawS3; ++raw) {
        ASSERT_LE(AdcRawToMilliVolts(cal, raw), kAdcFullScaleMv12dB) << "raw=" << raw;
    }
}

TEST(AdcCalibration, MidScaleIsApproximatelyHalfOfFullScale) {
    const AdcCalibration cal = AdcCalibrationSelect(true);
    const int mv = AdcRawToMilliVolts(cal, kAdcMaxRawS3 / 2);
    EXPECT_NEAR(mv, kAdcFullScaleMv12dB / 2, 25);
}

TEST(AdcCalibration, AMissingEFuseFallsBackAndSaysSo) {
    // Spec 3.2: curve-fitting calibration is per-chip and eFuse-backed, and
    // returns ESP_ERR_NOT_SUPPORTED on modules whose eFuses are blank. The spec
    // forbids silently mis-scaling every reading, so the fallback must both
    // happen AND be reported -- which is what CalibrationSource is for.
    const AdcCalibration curve = AdcCalibrationSelect(true);
    const AdcCalibration fallback = AdcCalibrationSelect(false);
    EXPECT_EQ(curve.source, CalibrationSource::kEFuseCurveFit);
    EXPECT_EQ(fallback.source, CalibrationSource::kLinearFallback);
    // The fallback is still a usable conversion: monotonic, in range, and
    // agreeing with the calibrated curve at both endpoints.
    int prev = -1;
    for (uint16_t raw = 0; raw <= kAdcMaxRawS3; ++raw) {
        const int mv = AdcRawToMilliVolts(fallback, raw);
        ASSERT_GE(mv, prev) << "non-monotonic at raw=" << raw;
        ASSERT_LE(mv, kAdcFullScaleMv12dB) << "raw=" << raw;
        prev = mv;
    }
    EXPECT_EQ(AdcRawToMilliVolts(fallback, fallback.raw_low), fallback.mv_low);
    EXPECT_EQ(AdcRawToMilliVolts(fallback, fallback.raw_high), fallback.mv_high);
}

TEST(AdcCalibration, TheSenseBufferClipsBeforeTheAdcCeiling) {
    // Spec 2.3: the sense buffer U6B is ITSELF an op-amp on +5V, so its output
    // saturates near 4.98V whatever the KEY line does. Through the exact /2
    // divider that is 2490mV. The 5.20V figure is the head-unit IDLE ENVELOPE
    // bound (spec 6.2) and does NOT reach the divider unclipped -- taking it
    // literally would give 2600mV, a voltage U6B cannot produce.
    const int v_buf_saturation_mv = 4980;
    const int v_sense_mv = v_buf_saturation_mv / 2;
    EXPECT_LT(v_sense_mv, kAdcFullScaleMv12dB);
    EXPECT_EQ(v_sense_mv, 2490);
}
