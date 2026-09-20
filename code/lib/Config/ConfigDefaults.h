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
 * "nothing configured yet" device -- channels present but DISABLED, no learned
 * buttons -- which is a different config for a different purpose, not a second
 * copy of the same one.
 *
 * It is a valid config (`ConfigValidate` accepts it): every channel is named and
 * its idle reference is a plausible ADC reading. That matters because the app
 * may send it straight back, and a default the device itself would reject is a
 * trap.
 */
Config ConfigDefault();
