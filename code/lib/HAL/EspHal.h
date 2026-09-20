#pragma once

#include "HAL/IHAL.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The real hardware implementation of IHAL. This is the only file in the
 * firmware that touches ESP-IDF drivers for I/O, and it is deliberately thin:
 * everything with logic lives above it and is host-tested against MockHal.
 *
 * Init is separate from the interface because the drivers can fail (I2C bus,
 * ADC calibration, NVS namespace). It returns the IHAL* so `main.c` can pass it
 * straight to the orchestrator, but on a fatal driver failure it returns null
 * rather than a half-initialized interface -- a device that cannot read its own
 * inputs must not appear to be running.
 *
 * ADC calibration: the eFuse curve-fit scheme is per-chip and NOT universally
 * available (blank eFuses on some module batches return ESP_ERR_NOT_SUPPORTED).
 * When that happens the HAL falls back to the documented linear approximation
 * AND REPORTS IT (spec 3.2, FR-24's class of condition) rather than silently
 * mis-scaling every reading.
 */
IHAL *EspHalInit(void);

// True when the ADC fell back to the linear approximation because the eFuse
// curve was unavailable. The orchestrator plays BOOT_DEGRADED on boot for this,
// the same class of condition as a config fallback.
bool EspHalCalibrationIsDegraded(void);

#ifdef __cplusplus
}
#endif
