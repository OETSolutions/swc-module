#pragma once

#include "HAL/IHAL.h"
#include "System/SystemOrchestrator.h"

/*
 * Brings up the USB CDC app link on device and connects it to the command
 * vocabulary. This is the ONLY translation unit that names TinyUSB, so the
 * transport's logic stays host-testable behind `UsbCdc`'s injected `RawWrite`
 * (see UsbCdc.h).
 *
 * **The console stays on the ROM USB-Serial-JTAG peripheral.** Spec 4.1 makes
 * this a hard requirement: a debug `printf` landing on the app's CDC port is read
 * by the app as a protocol frame. Nothing here reconfigures the console, and
 * `test/test_hw/TestUsbCdc.cpp` asserts it rather than trusting it.
 *
 * Call once, AFTER `SystemOrchestratorCreate` has established the safe idle
 * output (FR-13). Nothing about the link runs before that, which is what makes
 * FR-42 structural: with no host, no app and no radio the device still serves
 * presses.
 *
 * Config reads and writes go through a `ConfigStore` over the same NVS the
 * orchestrator booted from -- the same slots, so a `config_get` reply and the
 * running config cannot diverge. `sys` is what `test_key` and `identify` drive;
 * the persistent HAL is what `reboot` uses.
 */
void UsbLinkStart(IHAL *hal, SystemOrchestrator *sys);

// Drains any pending router output and the TX buffer. Safe to call every poll
// tick, and a no-op before Start or when Start failed.
void UsbLinkService();
