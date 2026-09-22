#pragma once

#include "Config/ConfigModel.h"

/*
 * The pass-through default config: what the device runs when NVS holds nothing
 * (FR-25) and what a `config_get` reply carries before the app has ever sent one.
 *
 * ONE definition, in lib/, because three callers need it and they must agree:
 * `SystemOrchestratorCreate` (the boot config), `CommandRouter` (the reply to
 * `config_get`), and any future factory-reset path. It was previously built
 * inline inside `SystemOrchestratorCreate`; adding a second inline copy in the
 * router is exactly the two-homes defect this plan keeps re-discovering.
 *
 * It is deliberately NOT the same thing as the test fixture
 * `MockHalDefaultsConfig()`: that one carries spec 3.7's worked ladder so
 * classification tests have real button windows to match. This one is the
 * "nothing configured yet" device -- channels present and ENABLED, but with no
 * learned buttons (`ladder.count == 0`) -- which is a different config for a
 * different purpose, not a second copy of the same one. The channels are enabled
 * because spec 3.4 scopes `enabled` to CLASSIFICATION: a channel with no learned
 * buttons is described by its zero `count`, and a channel that is not present at
 * all by `channel_count`. Shipping them disabled also made every binding the app
 * pushed unfindable on a fresh device.
 *
 * It is a valid config (`ConfigValidate` accepts it): every channel is named and
 * its idle reference is a plausible ADC reading. That matters because the app
 * may send it straight back, and a default the device itself would reject is a
 * trap.
 *
 * **`out` is written IN PLACE, and there is deliberately no by-value form.**
 * `sizeof(Config)` is 8,912 B, and returning one by value makes the RETURNING
 * function's frame that big -- which, on this device's 4 KB TinyUSB task and
 * 3.5 KB main task, is an immediate stack overflow. `ConfigDefault()` as a
 * value-returning function measured a 17,856-byte frame for `config_get`'s
 * reply, a 17,920-byte frame for `config_patch` and an 18,704-byte frame for
 * `Boot()`, against the 3,584-byte `CONFIG_ESP_MAIN_TASK_STACK_SIZE`. The host
 * suite cannot see any of it: the crash needs a real task stack, and the board
 * has not been flashed. A pointer the caller owns (a static, or a member that is
 * already in BSS) is the same config with none of the frame.
 */
void ConfigDefault(Config *out);
