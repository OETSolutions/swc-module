#!/usr/bin/env bash
#
# dev_flash.sh — flash the DUT over its own USB cable, with no BOOT press.
#
# ## The problem this exists for
#
# The product firmware hands the ESP32-S3's USB PHY to TinyUSB (the Android app
# link, `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` puts the ROM console on
# USB-Serial-JTAG but the app link reconfigures the peripheral). While the app
# firmware is running, the ROM USB-Serial-JTAG peripheral esptool needs is NOT
# present on that port, so `esptool` / `pio run -t upload` fail with "No serial
# data received" and the only documented way in was a physical BOOT press
# (spec 3.2). On a sealed enclosure that means opening the case and poking a
# recessed pin -- on every flash.
#
# The firmware accepts `{"type":"reboot","boot_target":"bootloader"}`
# (spec 4.3, N-80), which sets `RTC_CNTL_FORCE_DOWNLOAD_BOOT` and restarts. The
# ROM re-checks that bit on the next boot and enters its USB download loader. So
# the flash is driven over the app's own CDC port:
#
#   1. send the reboot frame over the app port (TinyUSB CDC, 303A:4001)
#   2. USB bus reset, so the host re-enumerates the loader (see below)
#   3. wait for the ROM loader port (303A:0009) to appear
#   4. esptool write-flash BOTH app slots
#   5. `--after watchdog_reset` boots back into the application
#
# **Step 2 is the non-obvious one.** macOS keeps the stale TinyUSB node bound to
# the port after the reset, so the ROM interface never appears in /dev and the
# device looks *wedged* -- enumerated, silent, unreachable by esptool. A USB bus
# reset (pyusb `Device.reset()`, in `dev_usb_reset.py`) forces re-enumeration;
# then the loader shows up as `303A:0009` (PID = the S3 chip id) and esptool
# connects immediately. Found on the bench 2026-09-24; without it the software
# route appears not to work at all.
#
# **Step 4 writes BOTH slots on purpose.** `otadata` names whichever app slot the
# last USB-OTA commit wrote, so writing `app0` alone can leave the device
# booting a stale `app1` -- a flash that silently does nothing.
#
# ## Bootstrap (ONE TIME, and the reason this is not a pure software feature)
#
# The frame only works on firmware that ALREADY implements it. A device running
# older firmware -- or a blank board -- has no software way in, and the first
# flash still needs the BOOT press. After that, every subsequent flash uses this
# script. That is the honest boundary; the script reports it rather than
# pretending otherwise.
#
# ## Why not just `pio run -t upload`
#
# `pio run -t upload` uses `--before default-reset --after hard-reset`, which
# drives DTR/RTS expecting an auto-reset circuit. This board's auto-reset lines
# are NOT wired (the bring-up notes record "Hard resetting via RTS" never
# releasing the device), and `--before` would anyway try to reset a chip that is
# already sitting in the loader. So this script invokes esptool directly with the
# two flags that match the hardware: `--before no_reset` (it is already in the
# loader) and `--after watchdog_reset` (a software reset the ROM honours even
# with no auto-reset circuit).
#
# Usage:
#   tools/dev_flash.sh                 # build then push over USB OTA (default)
#   tools/dev_flash.sh --loader        # build then flash via the ROM loader
#   tools/dev_flash.sh --no-build      # flash the existing firmware.bin
#   tools/dev_flash.sh --app-port /dev/cu.usbmodem1234561
#
# **Two routes, and the DEFAULT is USB OTA.** `--ota` is the default because it
# is the one that has been verified end to end on the bench, and because it
# cannot leave the device unreachable: it only uses the app link the device is
# already talking on. `--loader` is the raw esptool route (the `bootloader`
# reboot frame drops the device into the ROM download stub); it is kept for the
# case where the running app is too broken to answer frames, and it is the newer
# of the two — see the note in `--loader`'s section about what it still needs.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# The app link's USB identity: Espressif VID 0x303A, TinyUSB PID 0x4001. The
# firmware sets SER=123456, so `1234561` is the port name on macOS -- but the
# name is matched by PID, not by that literal, so a device with a different
# serial still works.
APP_VID_PID="303A:4001"
# The ROM download interface, and it has TWO identities depending on how the
# device got there (verified on the bench 2026-09-24):
#   * `303A:0009` -- the ROM's **USB-OTG serial download** peripheral. This is
#     what a device that entered the loader via the `bootloader` frame shows,
#     0x0009 being the ESP32-S3's chip id (esptool matches this exact pair in
#     `uses_usb_otg`). It is the one this script's route produces.
#   * `303A:1001` -- the ROM's **USB-Serial-JTAG** peripheral, what a BOOT-press
#     download shows. Also accepted, so a manually-BOOT-pressed device still
#     flashes with this script.
ROM_VID_PID_RE='303A:(0009|1001)'

APP_PORT=""
ROM_PORT=""
NO_BUILD=0
USE_OTA=1
FLASH_OFFSET="0x20000"   # app0, per partitions.csv

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build)    NO_BUILD=1; shift ;;
        --loader)      USE_OTA=0; shift ;;
        --ota)         USE_OTA=1; shift ;;
        --app-port)    APP_PORT="${2:?--app-port needs a path}"; shift 2 ;;
        --rom-port)    ROM_PORT="${2:?--rom-port needs a path}"; shift 2 ;;
        --offset)      FLASH_OFFSET="${2:?--offset needs a value}"; shift 2 ;;
        # `--help` prints the header comment block: everything from line 2 up to
        # the `set -euo pipefail` line, so it cannot drift from the usage text.
        -h|--help)     sed -n "2,$(($(grep -n '^set -euo' "$0" | head -1 | cut -d: -f1) - 1))p" "$0" \
                           | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# --- helpers ---------------------------------------------------------------

# Every serial port on this machine, as "port|hwid", via the PlatformIO that is
# already managing this project. `pio device list` is the one enumerator that
# does not need the project's python env bootstrapped, and it is what a developer
# already has on PATH.
list_ports() {
    pio device list --json-output 2>/dev/null \
      | python3 -c '
import json,sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for d in data:
    print("%s|%s" % (d.get("port",""), d.get("hwid","")))
'
}

# The `cu.` spelling, because a `tty.` open waits for DCD and can hang; esptool
# and pyserial both prefer `cu.` on macOS.
find_port_by_vidpid() {
    local want="$1"
    list_ports | awk -F'|' -v w="$want" \
        '$2 ~ w { if ($1 ~ /^\/dev\/cu\./) { print $1; exit } else { hold=$1 } }
         END { if (hold != "") print hold }'
}

esptool_bin() {
    # The esptool PlatformIO already installed for its espressif32 builder. Using
    # it rather than a `pip install`ed copy keeps the flash and the build on ONE
    # esptool version -- a mismatched esptool is its own class of confusing
    # failure.
    local pkg="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
    if [[ ! -f "$pkg" ]]; then
        echo "ERROR: esptool not found at $pkg" >&2
        echo "       (run a device build once so PlatformIO installs it)" >&2
        exit 1
    fi
    # The shim's shebang is `python`, which is not always on PATH; invoke it with
    # the PlatformIO venv's interpreter instead.
    echo "$(py_bin) $pkg"
}

# The interpreter that has pyserial and esptool: PlatformIO's own venv. The
# system `python3` is a different interpreter and does NOT have either, which is
# why every helper is invoked through this rather than a bare `python3`.
py_bin() {
    local py="$HOME/.platformio/penv/bin/python"
    if [[ -x "$py" ]]; then echo "$py"; else echo python3; fi
}

# --- 1. locate the app port -------------------------------------------------

# The app port is needed by the OTA route ALWAYS, and by the loader route only to
# send the download-mode frame. So it is found here, but a missing one is an
# error only on the OTA route -- `--loader --rom-port ...` is the "the device is
# already in a loader, just write it" path and must not require an app.
if [[ -z "$APP_PORT" ]]; then
    APP_PORT="$(find_port_by_vidpid "$APP_VID_PID" || true)"
fi
if [[ -z "$APP_PORT" && "$USE_OTA" -eq 1 ]]; then
    cat >&2 <<EOF
ERROR: no device on the app port ($APP_VID_PID) found.

The OTA route needs the device RUNNING the application firmware -- it pushes the
image over the app link. Check:
  * the device is plugged in and powered (the DUT's own USB cable, not the rig)
  * the firmware is booted, not stuck in a loader
  * \`pio device list\` shows a $APP_VID_PID port

If the board is blank or running firmware that predates the OTA path, use the
loader route once with the BOOT press: hold BOOT, tap RESET, release BOOT, then
  tools/dev_flash.sh --loader
EOF
    exit 1
fi
if [[ -n "$APP_PORT" ]]; then echo "== app port: $APP_PORT"; fi

# --- 2. build (unless told not to) -----------------------------------------

BIN="$REPO_ROOT/.pio/build/esp32s3/firmware.bin"
if [[ "$NO_BUILD" -eq 0 ]]; then
    echo "== building"
    SWC_FW_VERSION="${SWC_FW_VERSION:-dev}" SWC_GIT_SHA="${SWC_GIT_SHA:-local}" \
        pio run -e esp32s3
fi
if [[ ! -f "$BIN" ]]; then
    echo "ERROR: $BIN not found -- run without --no-build, or build first." >&2
    exit 1
fi
echo "== image: $BIN ($(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN") bytes)"

# --- 2b. USB OTA route (no loader, no esptool) ------------------------------

if [[ "$USE_OTA" -eq 1 ]]; then
    echo "== pushing over USB OTA (spec 9.3) -- slower than a raw flash, no loader needed"
    # `dev_push_ota.py` drives ota_begin/ota_chunk/ota_end and the reboot. It
    # aborts a stale run from an interrupted push on its own.
    "$(py_bin)" "$REPO_ROOT/tools/dev_push_ota.py" --port "$APP_PORT" --image "$BIN"
    echo "== done. The device rebooted into the new image."
    exit 0
fi

# --- 3. ask the device for the ROM loader -----------------------------------

# If no loader port was named, drive the device into the loader over its own app
# link. If one WAS named (`--rom-port`), skip to the write -- the "the device is
# already sitting in a loader" path.
if [[ -z "$ROM_PORT" ]]; then
    if [[ -z "$APP_PORT" ]]; then
        echo "ERROR: no app port to send the download-mode frame, and no --rom-port." >&2
        exit 1
    fi
    # Every loader-shaped port present BEFORE the request. The rig driver can be
    # sitting in a loader and enumerates as `303A:1001`, so the set (not a single
    # port) is what must be excluded when looking for the DUT's new one.
    ROM_PORT_BEFORE="$(list_ports | awk -F'|' -v re="$ROM_VID_PID_RE" \
        '$2 ~ re { p=$1; sub(/^\/dev\/tty\./, "/dev/cu.", p); print p }')"

    echo "== requesting download mode over $APP_PORT"
    if ! "$(py_bin)" "$REPO_ROOT/tools/dev_flash_send.py" \
            --port "$APP_PORT" --frame '{"v":1,"seq":1,"type":"reboot","boot_target":"bootloader"}' \
            --expect-ack; then
        cat >&2 <<EOF
ERROR: the device did not ack the download-mode request.

The most likely cause is firmware that predates this feature: it refuses
\`bootloader\` with \`bad_target\`. Flash once with the BOOT press, then this
script works for every later flash. (No ack at all means the app link is not
answering -- check the device is running and the port is right.)
EOF
        exit 1
    fi
    echo "   acked; the device is restarting into the ROM loader"

    # **A USB-level reset is REQUIRED here on macOS, and this is the non-obvious
    # step.** The frame acks, the firmware sets the force-download bit and
    # restarts, and the ROM brings up its download peripheral -- but macOS keeps
    # the OLD TinyUSB node (\`$APP_VID_PID\`) bound to the same physical port, so the
    # new ROM interface never appears in /dev and esptool sees only the dead app
    # node. A USB bus reset makes the host tear the stale node down and enumerate
    # the loader. Without it the device looks *wedged* -- enumerated, silent,
    # unreachable -- exactly how it behaved on the bench until this was added.
    #
    # **The reset must come FIRST, and the seek must poll for the loader.** An
    # earlier revision waited for the app node to VANISH and only then reset; on
    # the bench that blocked for 10 s on a node macOS never drops by itself,
    # and the loader was already up the whole time. Seeking the loader directly
    # is both correct and faster.
    # **Seek `303A:0009` first and alone for the software route.** A device that
    # entered the loader via the frame enumerates as the ROM's USB-OTG download
    # interface, whose PID is the chip id (`0x0009` on the S3). The alternative
    # identity, `303A:1001`, is shared with the rig driver's own loader and with
    # a BOOT-pressed device, so seeking it here could report the wrong board. The
    # shell loop below still accepts either; this seek is only a "has it come up
    # yet" gate, and the unambiguous PID is the right thing to gate on.
    "$(py_bin)" "$REPO_ROOT/tools/dev_usb_reset.py" --vid-pid "$APP_VID_PID" \
        --seek 303A:0009 --seek-timeout 20 || true

    # --- 4. wait for the ROM loader port ------------------------------------

    # **`awk` must not `exit` on the first match, and must exclude the WHOLE
    # before-set.** The rig driver is often already in a loader and enumerates as
    # `303A:1001` -- the same identity a BOOT-pressed device uses -- so the first
    # matching port is frequently the DRIVER, not the DUT. An `exit`-on-match
    # loop returns that one, the shell discards it, and the loop never looks at
    # the next line, which is the DUT's new port. The before-set is split into an
    # awk array and each candidate is tested against all of it.
    for _ in $(seq 1 60); do
        cand="$(list_ports | awk -F'|' -v re="$ROM_VID_PID_RE" -v before="$ROM_PORT_BEFORE" '
            BEGIN { n = split(before, b, "\n"); for (i = 1; i <= n; i++) seen[b[i]] = 1 }
            $2 ~ re {
                p = $1
                sub(/^\/dev\/tty\./, "/dev/cu.", p)
                if (!(p in seen)) { print p; exit }
            }')"
        if [[ -n "$cand" ]]; then
            ROM_PORT="$cand"
            break
        fi
        sleep 0.25
    done
    if [[ -z "$ROM_PORT" ]]; then
        cat >&2 <<EOF
ERROR: no ROM loader port ($ROM_VID_PID_RE) appeared after the request.

The frame was acked, so the firmware ran the download-mode action. Check:
  * the device did not power-cycle (the force-download bit lives in the RTC
    domain -- it survives a software reset, not a power cycle)
  * no OTHER board already occupies a loader port (the rig driver in its own
    loader enumerates as 303A:1001 too -- unplug it, or flash the DUT alone)
  * if a loader port IS present but this script missed it, pass --rom-port <path>
EOF
        exit 1
    fi
fi
echo "== ROM loader on $ROM_PORT"

# --- 5. flash ---------------------------------------------------------------

ET="$(esptool_bin)"

# **Write BOTH app slots, and this is not optional.** The app exists in two
# partitions (`app0` @ 0x20000, `app1` @ 0x200000 -- partitions.csv) and the ROM
# picks one from the `otadata` partition, which a USB-OTA commit sets to whichever
# slot it just wrote. So a loader flash that writes `app0` alone, on a device
# whose last update went to `app1`, writes an image the bootloader will NOT
# select: the device reboots into the OLD app and the flash looks like it
# silently did nothing. Found on the bench 2026-09-24, where both slots happened
# to match only because an OTA and a loader write had both just run.
#
# Two fixes were tried and rejected in favour of this one:
#   * Writing IDF's `ota_data_initial.bin` at 0x15000. That file is ALL 0xFF (it
#     is the blank selector), and esptool skips an all-0xFF region, so it changed
#     nothing -- verified by reading otadata back and finding the OTA-set sector
#     still in place.
#   * Erasing otadata (`erase_region`) so the ROM falls back to app0. Correct,
#     but it needs a SECOND esptool invocation (erase_region and write_flash are
#     separate subcommands) and the ROM port drops between the two.
#
# Writing both slots needs no assumption about otadata's format or the current
# selector state: whichever slot it names now holds the new image. It costs one
# extra ~3 s write, which is the right trade for a route whose failure mode is a
# silent stale boot.
APP0_OFFSET=0x20000
APP1_OFFSET=0x200000
FLASH_PAIRS=("$APP0_OFFSET" "$BIN")
if [[ "$FLASH_OFFSET" == "$APP0_OFFSET" ]]; then
    FLASH_PAIRS+=("$APP1_OFFSET" "$BIN")
    echo "== writing both app slots (whichever otadata selects holds the new image)"
else
    FLASH_PAIRS=("$FLASH_OFFSET" "$BIN")
fi

# The `--flash_*` options belong to the `write_flash` SUBcommand in this esptool
# (v4.11): `--flash_mode qio` after `write_flash -z` is parsed as the address
# argument and fails with 'Address "qio" must be a number'.
#
# **`--verify` is NOT passed, because esptool v5 removed the flag: verification is
# now unconditional.** Passing it fails the whole command with
# "No such option '--verify'" *after* the device has been dropped into the ROM
# loader, which leaves the board sitting in the loader with nothing written --
# found on the bench 2026-09-24 when PlatformIO's bundled esptool moved to 5.3.0.
# The write is verified either way; this only stops asking for what is default.
echo "== flashing $FLASH_OFFSET"
# shellcheck disable=SC2086
$ET --chip esp32s3 --port "$ROM_PORT" \
    --before no_reset --after watchdog_reset \
    write_flash -z --flash_mode qio --flash_freq 80m --flash_size 4MB \
    "${FLASH_PAIRS[@]}"

echo "== done. The device has been reset back into the application."
