# SWC firmware + Android app — session handoff

> **STATUS — the tree has been COMMITTED.** The 100+ uncommitted files this
> document was written around are now commit **`38f579c`** ("Land the
> host-verified firmware and app, and make CI honest"; 104 files, +10,819/−673)
> on `feat/swc-firmware-android-app`. **§5 below is now historical** — the
> hazards it describes (the `.env`, the directory-wide-add trap) still apply to
> any future commit, but there is no longer a large uncommitted tree to lose.
>
> What the commit covered: all tracked modifications under `code/`, the
> repo-root `.github/workflows/` move (N-33 — Actions never read the old path,
> so no gate had ever run in CI), the six previously-untracked static gates +
> three gate self-tests, `code/docs/HANDOFF.md` itself, and the spec/plan
> corrections. The staged set was audited file-by-file before committing.
>
> Deliberately **left uncommitted**: `SWC.kicad_pcb` / `SWC.kicad_prl` and
> `plastic_case/SWC_Enclosure.3mf` (PCB half), plus three pieces of debris that
> are **not** product source and should not be swept in later by accident:
> a stray extension-less Kotlin file `code/android/.../swc/module`, a stray
> 47-line log fragment at **`docs/bring-up-log.md`** (repo root — its content
> was folded verbatim into `code/docs/bring-up-log.md`, so this copy is now
> redundant), and the resistor-values PNG.
>
> **Verified at commit time:** 503/503 native tests under ASan+UBSan, zero
> sanitizer reports; 120/120 Android unit tests; device build + size gate pass.
> The Android suite **requires JDK 17** — the machine default is JDK 25, which
> makes Robolectric fail 25 tests with `IllegalArgumentException at
> ClassReader.java:200` (an ASM bytecode-version error, NOT a code defect). Run
> it with `JAVA_HOME=$(/usr/libexec/java_home -v 17)` and `ANDROID_HOME` set.

Written 2026-09-24, at the point the **assembled PCB is in hand and the next
session is bring-up on real hardware**. This is a starting point for a new
session: read it top to bottom, then open the two governing documents.

## 0. Orientation — where everything is

| Thing | Path |
| --- | --- |
| Repo root | `<repo-root>` |
| Branch | `feat/swc-firmware-android-app` (uncommitted, see §5) |
| Software root | `code/` — ESP-IDF firmware (`lib/`, `src/`) + Android app (`code/android/`) |
| **Spec** (authority) | `docs/superpowers/specs/2026-09-18-swc-firmware-android-app-design.md` |
| **Plan** (task briefs) | `docs/superpowers/plans/2026-09-18-swc-firmware-android-app.md` |
| Agent instructions | `AGENTS.md` (repo root — the PCB doc split; also covers the PCB half) |
| Bring-up log | `code/docs/bring-up-log.md` (toolchain fixes, measured numbers) |
| SDD progress + history | `.superpowers/sdd/2026-09-18-swc-firmware-android-app/progress.md` |
| Static gates | `code/tools/` (see §4) |

**The spec/plan live under the repo root's `docs/`, not `code/docs/`.** The
harness resets the shell cwd to `code/` between Bash calls, so spec reads need an
absolute path or an explicit `cd` to the repo root.

## 1. What this product is (the one non-negotiable design fact)

An adapter between a factory steering-wheel resistor ladder and an aftermarket
Android head unit. Firmware on an **ESP32-S3** (pioarduino / IDF 5.5.5) decodes
the ladder, resolves a gesture (single/double/long), and drives an **MCP4728**
I²C DAC → servo → sink FET to present the head unit the KEY level it expects.
An Android app (Kotlin/Compose) talks to it over USB serial.

**The app has NO learning screen. The AUX1 button is the ONLY production learn
path** — hold AUX1 ~1.5 s to enter the learn wizard; a longer ~3 s hold opens the
maintenance window. It must work **with no app and no host**. An unbound gesture's
default is **passed through** to the head unit. Do not add a learning screen.

Two channels, SWC1 and SWC2. `DacChannel` → A=KEY1, B=ADJ1, C=KEY2, D=ADJ2.

## 2. Verify loop — run this before and after every change

```bash
cd code

# Host suite (591 tests) under ASan+UBSan, in an ISOLATED build dir so a
# concurrent session cannot clobber it. SWC_FW_VERSION/SHA must be set here too,
# or PlatformIO auto-cleans .pio/build/ (see the note below the loop):
SWC_FW_VERSION=dev SWC_GIT_SHA=local \
PLATFORMIO_BUILD_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -Wno-deprecated-declarations" \
PLATFORMIO_BUILD_DIR=/tmp/swc_asan_iso pio test -e native

# Device image + size gate, also isolated (SWC_FW_VERSION/SHA must be set or the
# version string is stale):
SWC_FW_VERSION=dev SWC_GIT_SHA=local PLATFORMIO_BUILD_DIR=/tmp/swc_size_iso pio run -e esp32s3
python3 tools/check_size.py --bin /tmp/swc_size_iso/esp32s3/firmware.bin

# Static gates (each is also run by CI; all live in code/tools/):
python3 -m pytest tools/ -q          # gate self-tests
python3 tools/check_hal_contracts.py
python3 tools/check_frame_handlers.py
python3 tools/check_app_limits.py
python3 tools/check_task_ownership.py
python3 tools/check_maintenance_radio.py
python3 tools/check_sdkconfig_keys.py
python3 tools/check_spec_example.py
python3 tools/check_stack_usage.py
bash   tools/crosscheck_config.sh
```

Last verified: **591/591 native tests green, zero sanitizer reports**; device
build succeeds at 74.5 % of the app slot; Python gates green (incl. the new
`check_maintenance_radio.py`, which pins the four maintenance-radio defects).

**Always use isolated build dirs.** Peer sessions share `code/.pio/build/` and
will race you (`PLATFORMIO_BUILD_DIR=…` per the commands above).

**And set `SWC_FW_VERSION`/`SWC_GIT_SHA` on EVERY PlatformIO invocation, not just
the device build.** Measured on the dev box: PlatformIO keys its auto-clean on
ONE `.pio/build/project.checksum` shared by every env, and `platformio.ini:41-42`
bakes `sysenv.SWC_FW_VERSION`/`sysenv.SWC_GIT_SHA` into the checksummed config.
So a `pio test -e native` run *after* a var'd `pio run -e esp32s3` computes a
different checksum and PlatformIO **deletes all of `.pio/build/`** before
rebuilding — taking the device tree with it. (`pio run -e native` does the same;
this is not specific to `pio test`.) Two consequences:

- The device-only TUs vanish from the call graph, and `check_stack_usage.py`
  used to report that as `app_main` being a *renamed root* — a source-drift
  misdiagnosis of a wiped directory. It now refuses instead, naming the env vars
  (`build_tree_error`, pinned by a self-test).
- Isolated dirs sidestep the peer race but **not** this: a native run in the
  default dir still evicts the device dir. Set both vars (any value, but the
  *same* value) on every invocation.

**Mutation-test every new assertion**: revert the fix, confirm the test *fails*,
restore. Several real defects were found by a guard that was itself untested.

## 3. Device-only blind spots — the important list for a real-board session

Two files are **excluded from the host build** (`build_src_filter`):
`lib/HAL/EspHal.cpp` and `lib/Link/UsbLink.cpp`. **The host suite cannot compile
them**, so every defect in them is invisible until the board is flashed. This is
where the remaining risk concentrates. The history already includes three such
clusters (NVS mount/write/read, the DAC I²C frame, the ADC calibration path).

When you flash, watch specifically for:
- **NVS** — `HalNvsSet` return contract and `ReadSlot` chunk-0 width (both were
  broken on-device while the host suite was green).
- **ADC** — `adc_read_mv` returns **-1 on error**, and **0 mV is a LEGAL value**.
  Every raw read must test `>= 0`; a failed read HOLDS state, never counts as a
  press. Three sites had this wrong (N-43).
- **USB DTR** — the `SET_CONTROL_LINE_STATE` wIndex must name the CDC **COMM**
  interface, not the DATA interface (TinyUSB matches the COMM one).
- **cJSON nesting** — `CJSON_NESTING_LIMIT` (default 1000) vs a **3,584-byte**
  `app_main` stack (~64 B/level on xtensa). A ~1 KB line of balanced brackets
  crashes the parser, and the stack gate is recursion-blind (N-71). The only
  valid test uses BALANCED brackets.

## 3a. Flashing the DUT without a BOOT press (added 2026-09-24, N-80)

**Do not reach for the recessed BOOT pin first.** The firmware accepts
`{"type":"reboot","boot_target":"bootloader"}`, which restarts the device into
the ROM download loader, so a full flash runs over the device's own USB cable.

```bash
cd code
tools/dev_flash.sh                 # build, then push over USB OTA (the default)
tools/dev_flash.sh --loader        # build, then raw esptool write via the loader
tools/dev_flash.sh --no-build      # skip the build
tools/dev_flash.sh --loader --rom-port /dev/cu.usbmodem113101   # device already in a loader
```

Two routes, and they are not interchangeable:

- **`--ota` (default)** — pushes the image over the app link (`ota_begin` /
  `ota_chunk` / `ota_end`, spec 9.3), commits it, reboots. No loader, no esptool.
  Slow (~3 min; the wire chunk is 512 B) but it cannot leave the device
  unreachable, and it is the one to prefer. It manages the two app slots properly.
- **`--loader`** — sends the `bootloader` frame, waits for the ROM interface, and
  runs esptool. Fast (~10 s) and works even when the running app is too broken to
  answer frames. **It writes BOTH app slots**, because `otadata` names whichever
  slot the last OTA wrote and a single-slot write can reboot a stale image.

### The three non-obvious things, all found on the bench

1. **A USB bus reset is required after the frame — but resetting is a FALLBACK,
   and it can WEDGE the device.** macOS keeps the stale TinyUSB node (`303A:4001`)
   bound to the port, so the ROM interface never appears in `/dev` and the device
   looks *wedged* — enumerated, silent, unreachable. The script's
   `tools/dev_usb_reset.py` issues `Device.reset()` through pyusb, after which the
   loader shows up as `303A:0009` (PID = the S3 chip id, 9). If you ever see the
   wedged state by hand, that reset is the fix — the device is fine.

   **The trap (this killed the DUT once, 2026-09-23): the reset is only safe once
   the stub has FINISHED enumerating.** A bus reset that lands while the ROM
   download stub is still bringing its USB peripheral up collides with that
   bring-up — the control transfer never completes (`[Errno 60] Operation timed
   out`) and the stub's USB peripheral wedges such that it never enumerates
   again. The board then vanishes from the bus entirely, and no host action
   recovers it (esptool in every `--before` mode, repeated pyusb resets, and
   `uhubctl` hub power cycles all failed; only a manual reset brought it back).
   The helper now **polls for the loader first and resets only if it does not
   come up on its own** — the loader usually does, so the bus is untouched on the
   happy path. A *failed* reset is a hard, non-retryable error (exit 2), never
   the old reassuring "device re-enumerated; continuing". **If you see it: stop,
   do not retry, do not power-cycle a hub port, reset by hand, and use `--ota`.**
2. **The loader identity is `303A:0009`, not `303A:1001`.** `303A:1001` is the ROM
   USB-Serial-JTAG a BOOT-press gives, and the **rig driver enumerates as it too**
   — so never auto-select `303A:1001` without excluding pre-existing ports, or you
   will flash the driver board.
3. **Bootstrap needs one BOOT press, and only once.** A blank board, or firmware
   predating N-80 (it refuses `bootloader` with `bad_target`), has no software way
   in; `dev_flash.sh` reports this rather than hiding it. After that first flash,
   every later one uses the tools above.

`--after watchdog-reset` is what boots the device back into the app after an
esptool write. `--after hard-reset` does not work here (this board has no
auto-reset wiring), and if the app node does not reappear within a few seconds,
one more `dev_usb_reset.py` run re-enumerates it.

### Two route hazards found on the bench (2026-09-24)

- **`--loader` used to pass `--verify`, which esptool v5 REMOVED** (verification
  is unconditional now). The flag made the whole command fail with
  `No such option '--verify'` *after* the device had already been dropped into
  the ROM loader — so the board sat in the loader with nothing written and looked
  broken. Fixed in `dev_flash.sh`; if you see that error, the fix is to drop the
  flag, not to press BOOT.
- **`--ota` (the default) is SLOW and LOOKS HUNG. Measured 2026-09-24 and
  re-measured: a full 1,464,592-byte image takes ~7.4 minutes (~3300 B/s), because
  the wire chunk is 512 B and the device ACKS EACH ONE (2,861 round trips). The CPU
  sits in `select` the whole time, which is what a slow serial read loop looks like
  — the earlier "1 s CPU in 12 min = stuck, not slow" reading was WRONG, and the
  push actually completes: `ota_end` → `ack result=ok`. **So do not kill it on a
  stall; watch for the progress line**, which `tools/bench_rollback.py` prints every
  256 chunks. `--loader` remains the faster route (~25 s, both slots) and the one to
  use when you want speed or the device is not answering. One real caveat from the
  first run: if the device is rebooted MID-PUSH the run aborts and `ota_end` answers
  `no_run` — the device is unharmed and no commit happens, but the push must be
  redone.

**Bench proof scripts** (they need the live device; CI cannot run them):

```bash
# a corrupt image is REFUSED and the running slot is untouched (FR-36)
~/.platformio/penv/bin/python tools/bench_ota_corrupt.py --port /dev/cu.usbmodem1234561

# a COMMITTED image that panics is ROLLED BACK (FR-37/FR-41). Builds the bad
# image itself (-D SWC_BENCH_PANIC_IMAGE), pushes it, reboots, and asserts the
# device comes back on the old image. ~7 min for the push.
~/.platformio/penv/bin/python tools/bench_rollback.py --port /dev/cu.usbmodem1234561

# the ladder cluster on the two-board rig (FR-9/12/31/31b/42): the rig driver
# presents a synthetic wheel, the DUT classifies it. Needs --dut AND --rig.
~/.platformio/penv/bin/python tools/bench_ladder.py \
    --dut /dev/cu.usbmodem1234561 --rig /dev/cu.usbmodem1121101

# the provisioning handshake (FR-34): runs Espressif's reference client
# `esp_prov.py` against the device over BLE. Opens the window, reads the BLE name
# + PoP over USB, proves the Sec1 session + `CmdSetConfig`/`CmdApplyConfig` (both
# status 0) with the RIGHT PoP and that a WRONG PoP is refused. Needs `bleak` and
# `protobuf` in the penv python; the WiFi JOIN is FR-35's dependency, not FR-34's.
~/.platformio/penv/bin/python tools/bench_prov.py --port /dev/cu.usbmodem1234561

# the output line on the rig (FR-16/39/40): drives a key via `test_key` and
# watches the DUT's own KEY output through the loopback, proving release is
# high-Z (FR-16), a reset never leaves the line driving (FR-39), and safe idle is
# re-established before a key after a reset (FR-40). Needs --dut AND --rig.
~/.platformio/penv/bin/python tools/bench_output.py \
    --dut /dev/cu.usbmodem1234561 --rig /dev/cu.usbmodem1121101

# FR-19's bring-up enable: measure the servo's static error with the trim loop
# DISABLED vs ENABLED. Build the DUT with the loop first:
#   SWC_FW_VERSION=dev SWC_GIT_SHA=local PLATFORMIO_BUILD_FLAGS="-D SWC_BENCH_TRIM_LOOP" \
#     PLATFORMIO_BUILD_DIR=/tmp/swc_trimimg pio run -e esp32s3
#     ~/.platformio/penv/bin/python tools/dev_push_ota.py --port <app> --image /tmp/swc_trimimg/esp32s3/firmware.bin
# This is the measurement that found N-84 (the enabled loop moved the output the
# WRONG way, ~11 mV = one max_step, because it was fed the pre-drive reading).
# The script now establishes the idle-seeding precondition itself (drive an idle,
# reboot the DUT holding it) -- without it every pulse acks but never drives.
~/.platformio/penv/bin/python tools/bench_trim.py \
    --dut /dev/cu.usbmodem1234561 --rig /dev/cu.usbmodem1121101
```

**Silicon result, 2026-09-24 (the FR-19 fix confirmed).** Settled-level static
error, ENABLED (trim) vs DISABLED (open-loop), `test_key` held 1 s:

| target | disabled | enabled |
| --- | --- | --- |
| 2500 mV | −37 mV | **−29 mV** |
| 2200 mV | −33 mV | **−33 mV** |
| 2800 mV | −10 mV | **−6 mV** |

Enabled is **no worse than disabled**, i.e. the N-84 wrong-direction step is
gone. Enabled repeat spread was 0–9 mV → **no ADC-noise injection**. The loop
only runs while a pulse is on the line, so a per-pulse settled level is its
observable; a continuous gain-convergence figure is the only FR-19 item left.

**Bench traps `bench_ladder.py` documents, because they look like firmware bugs
and are not:**

1. **The released SWC node floats to ~3173 mV — above the ADC's 2900 mV
   calibrated ceiling.** The DUT's own pull-up takes an UNLOADED SWC pin there,
   while a real wheel ladder idles at ~2835 mV. The DUT SEEDS its idle reference
   from the live reading at boot and only re-adopts within ±6 % of itself, so a
   boot on the floating node leaves the reference at 3173 and every learn is
   refused as `out_of_range` — persisting across re-flashes, because the seed is
   re-read each boot. **The rig must present a wheel-like idle and REBOOT the DUT
   with it held** (which is what a car does — the wheel is attached at power-on).
2. **A rig STATUS read RELEASES its DAC** (`KeyLine::FloatMv` calls
   `Dac::Release`), and the drive then RAMPS for ~1 s. A level read too early sees
   the previous float; a `set_level` loop that reads without draining the rig's
   reply queue parses a stale level and never converges. `drive_now()` re-drives
   and waits the ramp out.

3. **The loopback (DUT KEY out → driver SWC in) is a WIRE PER CHANNEL, and both
   boards are identical, so each has its own pull-up.** An absent loopback wire
   reads the released level and looks *identical* to a released DUT — the only way
   to tell is to DRIVE the channel and see whether the observer moves.
   `bench_output.py` probes this and reports which channels are observable rather
   than failing on a half-wired rig. On the 2026-09-24 bench only **channel 1**'s
   KEY output was wired to the loopback; channel 2's drive is verified instead by
   the boot read-back (`VerifySafeIdleIdleCodes` reads channel C's DAC), which is
   per-channel and is the same check FR-37's board proof exercises.

4. **The `test_key` hold is bounded (1000 ms).** `HandleTestKey` NACKs
   `hold_ms out of range` above `kTestKeyMaxHoldMs`, so a longer hold is refused,
   not clamped — a bench script that asks for 1500 ms gets a nack, not a 1000 ms
   pulse.

5. **The released level is ABOVE the output envelope's low edge but the sweep
   targets must stay BELOW the released (pull-up) level.** The output only sinks,
   so a `test_key` above the line's rest turns the FET off and just floats the
   line — `bench_trim.py`'s sweep stays under it for that reason.

6. **A pulse ACKS but does not DRIVE unless the rig is presenting an idle and the
   DUT booted holding it (trap 1), and this presents as "nothing drives".** The
   command band's ceiling is the DUT's own live idle minus the headroom, so with
   the line floating every target sits above the reachable band: `DriveBoundLevelMv`
   returns nothing to drive, the pulse is still acked, and the loopback reads the
   released level for every sweep point. `bench_output.py` and `bench_trim.py`
   both establish the idle-seeding state first; a script that fires `test_key`
   against a floating line measures the release and will look like a dead output
   stage (`bench_output.py` reports "channels observable: none" rather than a
   pass/fail, which is the signal that the rig is in the wrong state).

## 4. Static gates

All in `code/tools/`; the repo-root `.github/workflows/firmware.yml` invokes
them by name and CI runs from `code/`. Every gate and its `test_check_*.py`
self-test is committed as of `38f579c`, and CI runs each one; a gate added later
must be added to the workflow by name as well as to the verify loop in §2.

- `check_maintenance_radio.py` (added with N-15) pins the maintenance radio's
  window-scoped invariants — FR-32's teardown pairing, every reply setting its
  status line, the failure count being window-scoped and assigned rather than
  accumulated, peer body lengths clamped before narrowing, and the BLE scheme
  being re-enterable (`FREE_BT`, not the one-shot `FREE_BTDM`). `MaintenanceRadio.cpp`
  is device-only, so nothing else in the suite can see any of them.
- `check_stack_usage.py` reads `sizeof(Config)` from the `static_assert` in
  `lib/Config/ConfigModel.h` (added this session) rather than carrying its own
  copy; deleting that assertion fails the gate loudly instead of silently
  reverting to a remembered number.

## 5. Uncommitted state — READ BEFORE TOUCHING GIT

**110+ files are modified/untracked on `feat/swc-firmware-android-app`, nothing
committed.** This is the single biggest hazard for a new session.

### Hard rules (a mistake here loses work or leaks secrets)

1. **`code/.env` contains LIVE credentials** (WiFi SSID + password, OpenWeatherMap
   API key, a Home Assistant long-lived token, location values) and belongs to a
   **different project**. It must **never be committed**. It is in
   `code/.gitignore` line 14.
2. **Never `git add code/` or `git add -A`.** Only explicit file lists. The
   untracked-tools situation (§4) and Drive's `.~*.insyncdl` placeholders inside
   source dirs make a directory-wide add unsafe.
3. `code/scratch.txt` (gitignored) and `code/.kilo/` are **not part of this
   project** — never stage.
4. `plastic_case/SWC_Enclosure.3mf` has a working-tree modification — **do not
   stage or commit it**.
5. `SWC.kicad_pcb` is modified in the working tree (PCB half) — leave it alone
   unless the task is the PCB.
6. **`git push` needs explicit user confirmation** — shared remote side effect.

Before committing anything, `git status` and read what you staged. The tools
under `code/tools/` (including the untracked gates) are the first explicit
`git add` batch to make CI honest.

## 6. Known-open items (prioritized for a real-board session)

These are **recorded gaps, not regressions** — the firmware is honest about
them (a router nack that says `not_implemented` is telling the truth). Full
detail is in the auto-memory index (`MEMORY.md`) under the N-numbers.

**Wire these as part of bring-up, using the board to verify:**

| Item | What's missing |
| --- | --- |
| **OTA — USB path** | ✅ **WIRED 2026-09-24 (N-14).** The router dispatches `ota_begin`/`ota_chunk`/`ota_end` to `OtaUsb`, `hello` advertises `"ota"`, the app's Update screen pushes a picked file with a progress bar. **Still board-gated:** the `esp_ota_*` flash write itself — verify on hardware with a deliberately CORRUPT image first. ✅ **VERIFIED 2026-09-24** by `tools/bench_ota_corrupt.py`: a real 398,592-byte image with ONE payload byte flipped at offset 199,296 was pushed with the TRUE size and TRUE sha256 (so the refusal can only come from OUR stream digest, not a header/magic test); `ota_end` returned **`nack: verify_failed`**, `uptime_ms` kept CLIMBING across the push (215,463 → 415,503 ms, i.e. no reboot), `heap_free` and `config_state` were unchanged, and the device still acked a command afterwards. The corrupt image was refused and the running slot was left untouched. **Also proven 2026-09-24: a COMMITTED image that panics is rolled back** (`tools/bench_rollback.py`; otadata shows the pending entry `ABORTED` and the previous entry `VALID`) — FR-37/FR-41. |
| **OTA — WiFi path** | ✅ **WIRED 2026-09-24 (N-15).** `OtaWifiCheck`/`OtaWifiInstall` are now reached from the maintenance page's `POST /api/ota/check` and `POST /api/ota/pull`. ✅ **The WiFi HTTP round-trip is PROVEN ON THE BENCH 2026-09-24** by `tools/bench_wifi_upload.py`: the DUT is provisioned onto the bench LAN (Sec1 over BLE with the `code/.env` credentials), then driven over HTTP at its LAN IP (`<device-lan-ip>`, sta mac `fc:01:2c:c0:a1:b2`). One run PASSes all four: **`GET /?token=` → 200** (the 9,321-byte page served), **wrong token → 401**, **upload with no token → 401**, **a CORRUPT image (true size + true digest of the corrupt bytes) → `500 "install failed"` refused, device still serving 200 afterwards**. The refusal can only come from the streamed digest, so the WiFi path shares `OtaUsb`'s verify gate (spec 9.4). The tool does NOT join the device's AP and does NOT touch the host's Wi-Fi. *(An earlier "blocked by this bench host" claim was WRONG: the device is LAN-reachable once provisioned; the flakiness was a `maintenance_exit`→`enter` cycle leaving the driver in STA-only without reconnecting — the tool now provisions and drives HTTP in the SAME window.)* **Still board-gated:** ~~the end-to-end `esp_https_ota` **git-release fetch**~~ — **CLOSED 2026-09-24 (N-85).** `tools/bench_ota_release.py` stands up a Cloudflare quick tunnel serving a manifest + image at a publicly-trusted HTTPS URL (a bench build bakes it in over `kReleaseManifestUrl` with `-D SWC_BENCH_MANIFEST_URL`), and the DUT (v0.9.0) does the real round-trip: `POST /api/ota/check` → **"an update is available: 1.0.0"** (manifest fetched over VERIFIED TLS), `POST /api/ota/pull` → **"installing"**, download through the shared digest gate, commit, reboot → **device runs 1.0.0**. **This found and fixed N-85:** the device has no RTC and `time_sync` is a no-op, so the clock reads 1970 and mbedTLS rejects a modern server cert as not-yet-valid — `OtaWifiCheck` mapped that to "the release manifest could not be read", naming nothing near the cause. Fixed by starting SNTP on `IP_EVENT_STA_GOT_IP` (stopped in `TearDown`). **The whole WiFi OTA scheme was effectively dead before this** — only the TLS-free USB/upload paths worked. |
| **Maintenance radio** | ✅ **DONE 2026-09-24 (N-15).** The window now has a radio behind it. `lib/Maintenance/MaintenanceRadio.cpp` (device-only TU, third host-excluded file) brings up `esp_netif` + `esp_event` + `esp_wifi` (APSTA) + NimBLE `wifi_provisioning` (BLE transport, Sec1 with the MAC-derived PoP) + `esp_http_server`, on the OFF→ON transition of `SystemOrchestratorIsMaintenanceActive` in `src/main.cpp`, and `TearDown()` frees exactly what each per-resource flag says exists, in reverse — so FR-32's "no radio outside the window" holds. The PoP, web token, setup URL and BLE name reach the app over the already-trusted USB link as a new `maintenance` frame (emitted only on change; forced on connect so a late-joining app learns an already-open window). The security decision — the token gate, the route table, the upload metadata check — lives in the host-tested `MaintenanceHttp` (17 tests), not in the untestable `esp_http_server` handler. ✅ **VERIFIED ON THE BENCH 2026-09-24 (N-82):** the AP appears in a Wi-Fi scan as `SWC-A1B2` (RSSI −39 dBm); `maintenance_enter` returns the frame with a real PoP (`<PoP>`) and token; heap drops 141,360 → 8,928 on entry and returns to ~135,800 on exit (FR-32 real); **three consecutive enter/exit cycles no longer reboot** after the N-82 fix. ✅ **The provisioning handshake itself is now PROVEN on the bench 2026-09-24 (FR-34)** by `tools/bench_prov.py`, which runs Espressif's own reference client `esp_prov.py` (the protocol the phone app wraps; its macOS BLE client is `bleak`, so the same reference client runs here): a PoP-authenticated **Sec1 session established**, `CmdSetConfig` returned **`status: 0x0`** and `CmdApplyConfig` returned **`status: 0x0`**, the device did **not** reset across the exchange (uptime climbed), and a **WRONG PoP was refused** (no session; the SRP6a client proof rejected). The WiFi *join* is not part of FR-34 — it needs a real AP and is FR-35's dependency; the tool drives a non-existent SSID by default and reports the join as the only remaining step. ✅ **FR-38's activity-renewal half is PROVEN on the bench 2026-09-24** by `tools/bench_maintenance_renewal.py`: with `maintenance_timeout_ms` set live to 12000 and a real HTTP request every 3 s, the window still answered after **38 s** (past the 12 s timeout — the timeout is inactivity, not a deadline), then closed **12.2 s** after the requests stopped (detected via the `maintenance` frame, not an HTTP probe, which would itself renew), with the server down afterwards (FR-32). |
| **Release check** | ✅ **DONE 2026-09-24 (N-12).** The APP performs the full spec §9.5 path: `update/ReleaseManifest.kt` (a faithful mirror of the firmware's `ReleaseCheck` + `SemverCompare`, differential-verified identical on 19 version pairs), `update/ManifestFetcher.kt` (HTTPS manifest fetch + image download, `INTERNET` now declared), and `AppViewModel`'s check → decide → download → verify `sha256`/`size` → push-over-USB flow. ✅ **The firmware's own `ReleaseCheck` now has a production caller too (N-15):** the maintenance page's `/api/ota/check` reaches it through `OtaWifiCheck`. |
| **DAC fault path** | ✅ **DONE 2026-09-24 (N-21).** FR-13's read-back (`IHAL::dac_read_code` → `DacFrame::DecodeReadCode`, compared per channel at boot); §6.8's retry-with-backoff (`DacRetry.h`, 3 attempts 1/2 ms) on both DAC writes; the latch (HAL `dac_faulted` + the orchestrator's `dac_verify_failed_` for a wrong-VALUE read); and `FAULT_DAC` now has a caller (`ReportDacFault`, an edge). ✅ **The real MCP4728 read is now PROVEN on the bench 2026-09-24:** a GOOD image pushed over USB OTA and rebooted left otadata's newest entry **`VALID`** — and `app_main` calls `esp_ota_mark_app_valid_cancel_rollback` only when `OutputVerified()` is true, which requires the read-back to have returned the exact idle code on the real part (`!dac_verify_failed_ && !dac_faulted`). So the 24-byte Read-Command response was parsed correctly against silicon, not just against a fixture |
| **NTC temperature** | ✅ **DONE 2026-09-24 (N-67).** `NtcConvert.h` (divider inversion + B3380 model, integer maths) + `SampleNtcTenthsC`; both learn paths record it in `temp_c_at_learn`. **VERIFIED ON THE DUT 2026-09-24:** `status.temp_c` reports `26.7` (a real room temperature, stable across frames) -- and the learn path was not enough for FR-1's "continuously": a device serving the wheel never read `ADC_CH_TEMP`, so `temp_c` was permanently `null` until `Tick` gained the ~1 Hz cadence. |
| **Servo trim loop (FR-19)** | ✅ **DEFECT FOUND+FIXED, AND SILICON-CONFIRMED 2026-09-24 (N-84).** The loop ships DISABLED (spec §6.5); enabling it (`-D SWC_BENCH_TRIM_LOOP`) found a real defect: the enabled loop moved the output ~11 mV (one `max_step`) in the WRONG direction, because `Update` was fed the sense reading taken BEFORE the command — the line's IDLE, not the code just written. **Fixed:** serviced from `Tick` after the servo settles (`ServiceTrim`, 60 ms settle then 200 ms cadence, reading the DRIVEN line), pinned + mutation-tested, native 592→593. **Re-measured on the DUT:** enabled settled-level error (−29/−33/−6 mV) is no worse than the disabled baseline (−37/−33/−10 mV), spread 0–9 mV (no noise). Only a continuous gain-convergence figure remains. The loop stays DISABLED in the shipped build either way. |
| **Reset safety (FR-39/FR-40)** | ✅ **DONE 2026-09-24.** Both reset sources reachable on this bench are proven. **Software reboot** (`tools/bench_output.py`): the loopback reads the released pull-up level before and after, so the reset left nothing driving; `status` reports `output_safe: true` afterwards. **Watchdog reset** (`tools/bench_wdt_reset.py`): the task-WDT panic is unset, but the **interrupt** watchdog reboots by default, so `-D SWC_BENCH_WDT_RESET` stalls with interrupts off on a non-WDT boot (self-limiting — it falls through on a WDT boot, so the device stays usable). Measured with three signals: **`status.reset_reason = 5` (`ESP_RST_WDT`)** — the device's own report — a multi-second silent stall, and `uptime_ms` restarting (70,667 → 2,497). After recovery `output_safe: true`, the loopback shows the KEY line idle (3,167 mV), and the device answers again. **`status.reset_reason` was ADDED for this** (IHAL `reset_reason`, §4.3), because the console is on the ROM USB-Serial-JTAG the firmware stops writing to once TinyUSB owns the PHY — the reset reason is the only peer-visible signal. **Brownout remains out of reach** (needs the 12 V cut); the safety property is a property of the boot sequence, so it holds for every source. |

| **Maintenance triggers (FR-33)** | ✅ **DONE 2026-09-24 (N-83, N-13's config-flag half).** §8.2's "config flag on next boot" is now `settings.maintenance_on_boot`, consumed and persisted by `SystemOrchestrator::Boot` so the window opens on the ONE boot the user asked for (without the consume it would reopen every boot forever — an unbounded window, the state FR-38 forbids). The field is optional on the wire, so a config written before it existed still loads. Added at **zero** `sizeof(Config)` cost by grouping the three count bytes into the tail padding. **Still open:** the reset-reason + no-config trigger needs a reset-reason source `IHAL` does not expose, and a no-config device already reaches pass-through, so it is redundant rather than missing. **Verified on the bench 2026-09-24:** a config with the flag set opens the window on the boot that follows (`active: true`, real PoP/URL) and clears the stored flag; a second boot reports `active: false`. The first version used `store_` in the persist guard, which is NULL during `Boot` on the device (`SetStore` is called from `UsbLinkStart`, which runs later), so the flag was never spent and the window reopened every boot — a device-only defect the host tests could not see. |
| **Multi-action bindings** | ✅ **DONE 2026-09-24 (N-29).** `BindingResolve` now returns the whole ordered `ResolvedBinding` list and `SystemOrchestrator::RunBindingActions` executes every firmware-owned action (`OUT_*`, `BUZZ`) while SKIPPING app-owned kinds rather than releasing. Pinned by 3 orchestrator + 2 resolver tests. |
| **`learn_channel_` is a constant 0** | ✅ **DONE 2026-09-24 (N-23).** The headless learn now has **no channel selector and no slot menu** — the user holds AUX1, presses the input being programmed, and releases; the wizard names the target by which input LEFT ITS IDLE (the 2022 `is_key_pressed()` mechanism). SWC1 and SWC2 are learned identically, and AUX2/AUX3 learn as switch windows. Presses during an armed learn no longer drive the radio. |
| **`status` rail/temp/heap** | ✅ **DONE 2026-09-24 (N-22/N-25).** `temp_c` (last good NTC reading, decimal, JSON null when unmeasured) and `heap_free` (new `IHAL::heap_free`) now have real producers; `rail_mv` is retired (no rail probe on this board). The app's live-idle gap is closed the honest way: `event` now carries `idle_mv` (the denominator the classifier used), and `LadderScreen.matched()` reproduces `LadderClassify`'s permille ratio. `gain_mode` still reports channel 0 only (N-60). |
| **App keepalive / liveness** | ✅ **DONE 2026-09-24 (N-27).** The app now implements its half of §4.4: `SwcClient.SilenceTick()` pings after 5 s of silence and raises `LinkState.SilenceExpired` after 10 s, `DriveLiveness()` is the loop (driven from the activity's lifecycle), the Link screen shows "Not responding" with physical guidance instead of a stale "Connected", and a frame revives the link without a reconnect. No board needed — JVM-tested against a controlled clock. |

**Documentation-accuracy leftovers (low severity, comment-only):** a handful of
comment-vs-code drifts remain unrecorded from the last audit passes. None are
logic or safety bugs. Do **not** spend a bring-up session on these.

**App-side items resolved 2026-09-24 (no board needed):** N-27 (the app's
keepalive + liveness), N-12 (the app's release check + USB install path), and
N-40 (the app's validator brought to rule-level parity with `ConfigValidate`,
with a differential harness over 55 mutants). None has a board-gated half left.

## 7. What was actually fixed recently (context, not TODO)

The audit history (~78 numbered findings) includes real, load-bearing fixes:
the `sizeof(Config)` by-value stack overflow (8912 B on 3.5–4 KB task stacks);
the NVS write/read contract failures; `-1` ADC sentinel consumed as a reading;
`uint16` wrap in the ratio mapping; cast-before-bounds UB on `1e999` (cJSON
parses to `+inf`); semver digit comparison dropping a component and overflowing a
`long` on xtensa; CI workflows at a path Actions never reads; learn-path
validity and config-write collapse; the unreachable pass-through. **This session
added** the `sizeof(Config)` assertion (§4) and made the stack gate read it.

**This session's hardware work (2026-09-24)** added the two-board bench rig
(`code_driver_board/`, committed `308c3c7`) and **found and fixed N-79**: the
head-unit-gone envelope check ran every tick *including while the device was
driving a key*, so a learned-only button — mapped onto the command band's
1800 mV floor, read back ~1790 mV through the device's own sense node — tripped
it, releasing and resetting the gesture machine every tick. On hardware that was
**26–177 duplicate `event` frames for a 2 s hold**, and `LONG` was unreachable.
Host-invisible: it needs the analog loop closed by a real sense node. Fixed by
`SystemOrchestrator::HeadUnitGone` (a driven line is judged only on a deep sag;
a released line's verdict must persist 250 ms). Native suite 503 → 504; device
build clean. Details in `code/docs/bring-up-log.md` and spec §12.1 (N-79).

**This session's software work (2026-09-24)** closed **N-15**, the last
genuinely-missing feature: the maintenance window had no radio behind it. Two
new modules now carry it — `lib/Maintenance/MaintenanceRadio.{h,cpp}`
(device-only; the third host-excluded TU alongside `EspHal.cpp` and
`UsbLink.cpp`) and `lib/Maintenance/MaintenanceHttp.{h,cpp}` (host-tested; the
token gate, route table and upload metadata check). The split is deliberate:
the security decision is tested on the host, only the IDF calls are not. The
window's PoP/token/URL/BLE name reach the app as a new `maintenance` frame.
Also fixed on the way: `UploadRefused`-style `strtoul` size parsing accepted a
**negative** declared size (`-5` → `ULONG_MAX−4`, past a `> 0` check → the gate
sees a ~4-exabyte image); the parse is now digit-only and pinned by tests over
`-5`, ` 100`, `+100`, `1e6`, `1000x`, empty. Native suite 504 → 585; Android
197; 39 Python gates; device build 74.5% of the app slot.


## 8. Environment notes

- **PlatformIO needs the pioarduino fork** (stock stops at IDF 4.6.1). Pinned in
  `code/platformio.ini`; see `code/docs/bring-up-log.md` for the toolchain-deleted-
  on-every-build fix (three stacked defects — read that section before
  "reinstalling the toolchain").
- ESP32-S3 has **no DAC** and the calibrated ADC ceiling is **2.9 V at 12 dB**,
  not 3.1 V. Envelope floor 1800 mV, guard low 2600 mV, gain 1.82/1.00.
- Android: `compileSdk=36`, `targetSdk=34`, `minSdk=26`, Robolectric 4.14.1,
  JDK 17.

## 9. First moves for the new session

1. `git status` and read §5 — protect the uncommitted tree and `.env` first.
2. Read the spec's bring-up / maintenance sections (§8, §12) and the tail of
   `code/docs/bring-up-log.md`.
3. Wire **one** device-only path end-to-end against the real board (start with
   OTA or the maintenance radio — each has a tested seam waiting for a caller)
   and verify it **on hardware**, not just in the host suite.
4. Only then decide whether to commit. Explicit file lists; commit the untracked
   `code/tools/` gates so CI is honest; never the PCB or enclosure 3MF.
