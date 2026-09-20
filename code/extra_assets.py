"""Run tools/gen_assets.py before a device build.

PlatformIO has no built-in "compile an asset into flash" step, and spec 9.2's
partition table has no filesystem, so the maintenance page has to be a compiled-in
array. This hook regenerates it whenever an asset changes.

**Why a `extra_scripts` hook rather than a checked-in generated header alone:** a
stale generated header is a silent divergence -- the page served would not be the
page in `assets/`, and nothing would say so. Regenerating on every build removes
the window. The generated file is also checked in, so a host build (which does not
run this hook) still compiles.
"""

import pathlib
import subprocess
import sys

Import("env")  # noqa: F821 -- provided by PlatformIO's SCons environment

PROJECT = pathlib.Path(env.subst("$PROJECT_DIR"))  # noqa: F821
ASSETS = PROJECT / "assets"
GENERATOR = PROJECT / "tools" / "gen_assets.py"
OUTPUT = PROJECT / "lib" / "Maintenance" / "WebPageAssets.h"


def _needs_regen() -> bool:
    if not OUTPUT.exists():
        return True
    newest = max((f.stat().st_mtime for f in ASSETS.glob("*.html")), default=0)
    return newest > OUTPUT.stat().st_mtime


def _generate(source, target, env):  # noqa: ANN001
    result = subprocess.run(
        [sys.executable, str(GENERATOR)], capture_output=True, text=True
    )
    if result.returncode != 0:
        # Fail the BUILD, not the flash: a broken generator that only complained at
        # runtime would ship a device whose maintenance page does not exist.
        sys.stderr.write(result.stderr or result.stdout)
        env.Exit(1)  # noqa: F821
    for line in result.stdout.splitlines():
        print(line)


# `AlwaysBuild` plus the mtime check keeps the common case (nothing changed) from
# shelling out on every incremental build, while still noticing an edited page.
if _needs_regen():
    _generate(None, None, env)  # noqa: F821
