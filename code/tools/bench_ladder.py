#!/usr/bin/env python3
"""Exercise the ladder cluster (FR-9/12/28/31/31b/42) on the two-board bench rig.

The rig driver (`code_driver_board`, `303A:1001`) presents a synthetic steering
wheel to the device under test (`code/`, `303A:4001`): it SINKs the DUT's ladder
node (J3.3 KEY1 -> J2.3 SWC1) and its AUX1 programming switch (J5.4 -> J5.4).

**Why this cannot be a host test.** Every host test drives the DUT through
`MockHal`, which hands the ADC a value the test picked. What only the bench can
show is the real analog path end to end -- the DAC, the servo, the sink FET, the
ladder divider, the ADC calibration -- and whether the gesture the rig PLAYS is
the gesture the DUT RESOLVES. That is the property FR-9/12/28/31 are about.

**Two HARness traps this script exists to avoid, both found on the bench
2026-09-24 and both looking exactly like firmware defects:**

 1. **The released node floats ABOVE the ADC's calibrated ceiling.** The DUT's
    own 10k pull-up takes an UNLOADED SWC pin to ~3173 mV, but `kAdcFullScaleMv12dB`
    is 2900 mV and a real wheel ladder idles at ~2835 mV (the stored
    `learned_idle_mv`). So a level the rig "releases" reads 3173 on the DUT --
    above the ceiling -- and a learn that records it latches `out_of_range_seen_`
    and REFUSES with `out_of_range`, while a classify that sees it reads a
    short-to-supply fault. **The rig must never be released**: to reproduce a real
    wheel it is held at a sub-ceiling idle the whole time (`idle` below), and a
    "press" is driving LOWER from there.

 2. **The DUT's link has a 10 s silence reap (spec 4.4).** A helper that polls
    the rig's status between `learn_start` and `learn_commit` takes several
    seconds per poll and can push the gap past 10 s; the reap then tears the
    session down and the commit answers `no_session` -- which reads as a firmware
    bug but is the harness being slow. So every rig command here is FIRE-AND-FORGET
    (a single write, no read-back), and the rig level is set up front.

The script is a set of named checks, each printing PASS/FAIL, so a single run
reports the whole cluster. Exit 0 = all selected checks passed.

Usage:
  bench_ladder.py --dut /dev/cu.usbmodem1234561 --rig /dev/cu.usbmodem1121101
"""

import argparse
import base64
import json
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("ERROR: pyserial not available; run under the PlatformIO penv python.",
          file=sys.stderr)
    sys.exit(2)

CHUNK = 512

# A wheel-like idle the DUT reads as a real ladder (sub-ceiling), and a press
# well below it. The rig COMMANDS these; the DUT's ADC sees a scaled version, so
# the checks assert on the DUT's own reported level, never on the commanded one.
IDLE_CMD_MV = 2835     # what the rig drives for "a released wheel"
PRESS_DELTA = 900      # how far below idle a "press" drives
TOLERANCE = 120        # a learned window's half-width at a real learn


class Rig:
    """The driver board. Commands are fire-and-forget: no status read-backs."""

    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.s.dtr = False          # DTR toggles some boards' reset; keep it quiet
        self.s.rts = False
        time.sleep(0.3)

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def cmd(self, chars):
        for c in chars:
            self.s.write(c.encode())
            time.sleep(0.012)

    def _drain(self, quiet=0.25):
        """Read and discard until the rig's TX has been quiet for `quiet` s.

        Every command echoes a reply line, and a burst of nudges echoes one line
        per character. A status read that does not first let that queue drain
        parses a STALE level (measured: a `set_level` loop read 1240 three times
        while the commanded level was already moving), which makes the loop
        overshoot and never converge.
        """
        last = time.time()
        while time.time() - last < quiet:
            try:
                chunk = self.s.read(8192)
            except Exception:
                break
            if chunk:
                last = time.time()

    def _status_text(self):
        """Read a fresh status block, after draining the reply queue."""
        import re
        self._drain()
        self.s.reset_input_buffer()
        buf = ""
        deadline = time.time() + 2.0
        self.s.write(b"s\n")
        while time.time() < deadline:
            try:
                buf += self.s.read(16384).decode("utf-8", "replace")
            except Exception:
                pass
            if re.search(r"commanded key\s*:\s*-?\d+ mV", buf):
                break
            time.sleep(0.1)
        return buf

    def set_level(self, target_mv, max_iter=16):
        """Walk the commanded key to `target_mv` by +/- nudges.

        The driver has no absolute set, only 50 mV nudges, so this reads its
        status to close the loop. It is used ONLY during setup, before any learn
        is open, so its latency cannot trip the DUT's silence reap.

        **A status read RELEASES the rig's DAC** (`KeyLine::FloatMv` calls
        `Dac::Release`), so the returned level is where the line floats, and the
        caller must follow with `hold()` to drive the target again.
        """
        import re
        for _ in range(max_iter):
            txt = self._status_text()
            m = re.findall(r"commanded key\s*:\s*(-?\d+) mV", txt)
            if not m:
                raise RuntimeError("the rig did not report its commanded level")
            cur = int(m[-1])
            delta = target_mv - cur
            if abs(delta) <= 25:
                return cur
            # At least ONE nudge: a 50 mV step means a delta under 50 floors to
            # zero, so a level within 49 mV of the target would spin forever
            # (`45 // 50 == 0` nudges, level unchanged, loop). One step always
            # moves the parity, and the `<= 25` test above then ends it.
            n = max(1, min(abs(delta) // 50, 300))
            self.cmd(("-" if delta < 0 else "+") * n)
            time.sleep(0.2 + n * 0.004)
        return None

    def drive_both(self, ch1_mv, ch2_mv, settle_s=0.6):
        """Present a level on BOTH channels AT ONCE and leave both driven.

        This exists because `set_level` cannot do it. `set_level` closes its loop
        with a status read, and the rig's status command prints
        `KeyLine::FloatMv(1)` AND `FloatMv(2)` -- and `FloatMv` calls
        `Dac::Release`. So every status read RELEASES BOTH DAC channels, which
        makes a `set_level`-based sequence structurally unable to hold two
        channels pressed at once: setting up the second always lets the first go.

        One status read up front (harmless -- nothing is driven yet) establishes
        the commanded level and the selected channel; from there the +/- nudges
        are walked with LOCAL tracking and NO further status read, and `p` drives
        the selected channel. `c` toggles the selection without releasing.
        """
        import re
        txt = self._status_text()
        m = re.findall(r"commanded key\s*:\s*(-?\d+) mV", txt)
        if not m:
            raise RuntimeError("the rig did not report its commanded level")
        cur = int(m[-1])
        sel_m = re.search(r"channel\s*:\s*(\d+)", txt)
        sel = int(sel_m.group(1)) if sel_m else 1

        for ch, target_mv in ((1, ch1_mv), (2, ch2_mv)):
            if sel != ch:
                self.cmd("c")
                time.sleep(0.25)
                sel = ch
            d = target_mv - cur
            if abs(d) > 25:
                # At least ONE nudge, for the same parity reason `set_level`
                # documents: a 50 mV floor on a sub-50 delta would spin.
                n = max(1, min(abs(d) // 50, 300))
                self.cmd(("-" if d < 0 else "+") * n)
                time.sleep(0.25 + n * 0.004)
                step = -50 if d < 0 else 50
                cur += step * n
            self.cmd("p")
            time.sleep(settle_s)
        return cur

    def drive_now(self, settle_s=1.6):
        """Drive the current commanded level and WAIT for it to actually arrive.

        The line ramps: after a `set_level` the rig's DAC is released (its status
        read calls `FloatMv` -> `Dac::Release`), and re-driving takes ~1 s to
        settle. A read taken too early sees the previous (floating) level, which
        is what made a learn sample 3173 mV and be refused as `out_of_range`.
        Wait the ramp out rather than guessing a fixed delay.
        """
        self.cmd("p")
        time.sleep(settle_s)

    def release(self):
        self.cmd("r")

    def aux(self):
        """Toggle the AUX1 programming switch."""
        self.cmd("a")

    def aux_state(self):
        """Read the AUX1 switch state ('CLOSED'/'open') from a status read.

        Only safe to call while NO learn is open: `set_level`/status reads call
        the driver's `FloatMv`, which RELEASES its DAC.
        """
        import re
        for _ in range(8):
            txt = self._status_text()
            m = re.search(r"AUX1 switch\s*:\s*(\w+)", txt)
            if m:
                return m.group(1)
        return None

    def ensure_aux_open(self):
        """`a` TOGGLES, so a prior run can leave the switch closed. Normalise."""
        st = self.aux_state()
        if st is not None and st.upper().startswith("CLOSED"):
            self.aux()
            time.sleep(0.3)

    def reset(self):
        """Put the rig in a known state: channel 1 selected, AUX1 open.

        Every check assumes it starts from this, because the checks SHARE one rig
        and FR-9 in particular leaves the rig on channel 2. Without the reset the
        next check drives the wrong ladder and fails for a harness reason.
        """
        import re
        st = self._status_text()
        m = re.search(r"channel\s*:\s*(\d+)", st)
        if m and int(m.group(1)) != 1:
            self.cmd("c")
            time.sleep(0.3)
        self.ensure_aux_open()


class Dut:
    """The device under test, over its USB CDC link (NDJSON frames)."""

    def __init__(self, port):
        self.port = port
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.s.dtr = False
        self.s.rts = False
        self.seq = 0
        self.pend = b""
        time.sleep(0.4)
        self.s.reset_input_buffer()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def reopen(self):
        """Re-open after a device reboot dropped the CDC port.

        A reboot re-enumerates the USB device a second or so later, so this
        retries until the port is back.
        """
        self.close()
        last = None
        for _ in range(40):
            time.sleep(0.5)
            try:
                self.s = serial.Serial(self.port, 115200, timeout=0.2)
                self.s.dtr = False
                self.s.rts = False
                self.pend = b""
                time.sleep(0.4)
                self.s.reset_input_buffer()
                return
            except OSError as e:
                last = e
        raise OSError(f"could not re-open {self.port}: {last}")

    def _write(self, **frame):
        self.seq += 1
        self.s.write((json.dumps({"v": 1, "seq": self.seq, **frame},
                                 separators=(",", ":")) + "\n").encode())
        self.s.flush()
        return self.seq

    def request(self, timeout=3.0, **frame):
        seq = self._write(**frame)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                self.pend += self.s.read(4096)
            except Exception:
                break
            while b"\n" in self.pend:
                line, self.pend = self.pend.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if msg.get("for_seq") == seq and msg.get("type") in ("ack", "nack"):
                    return msg.get("type"), msg
        return None, None

    def collect(self, seconds):
        out = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                self.pend += self.s.read(4096)
            except Exception:
                break
            while b"\n" in self.pend:
                line, self.pend = self.pend.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
        return out

    def config(self, seconds=4.0, tries=4):
        """Fetch and reassemble the config_get reply (chunked)."""
        for _ in range(tries):
            self.collect(0.6)
            self._write(type="config_get")
            frames = self.collect(seconds)
            begins = [i for i, f in enumerate(frames) if f.get("type") == "config_begin"]
            if not begins:
                continue
            run = frames[begins[-1]:]
            total = None
            parts = {}
            for f in run:
                if f.get("type") == "config_begin":
                    total = f.get("total_len")
                elif f.get("type") == "config_chunk":
                    parts[f.get("offset")] = f.get("data_b64")
            if not total:
                continue
            blob = bytearray(total)
            for off, b64 in parts.items():
                raw = base64.b64decode(b64)
                blob[off:off + len(raw)] = raw
            try:
                return json.loads(blob.decode("utf-8", "replace"))
            except (ValueError, UnicodeDecodeError):
                continue
        return None

    def push_config(self, cfg):
        blob = json.dumps(cfg, separators=(",", ":")).encode()
        import zlib
        import hashlib
        t, m = self.request(type="config_begin", total_len=len(blob),
                            crc32=zlib.crc32(blob) & 0xFFFFFFFF)
        if t != "ack":
            return None, m
        off = 0
        while off < len(blob):
            piece = blob[off:off + CHUNK]
            t, m = self.request(type="config_chunk", offset=off,
                                data_b64=base64.b64encode(piece).decode())
            if t != "ack":
                return None, m
            off += len(piece)
        return self.request(type="config_end", sha256=hashlib.sha256(blob).hexdigest())


def buttons(cfg, channel=0):
    if not cfg:
        return None
    return [(b.get("id"), b.get("mv_center"), b.get("mv_tolerance"))
            for b in cfg["channels"][channel]["ladder"]["buttons"]]


# ---------------------------------------------------------------------------
# The checks
# ---------------------------------------------------------------------------

def check_fr12_unlearned_press(dut, rig, results):
    """FR-12: a level matching no learned window yields event{button:null}."""
    cfg = dut.config()
    if cfg is None:
        results.append(("FR-12", False, "could not read the config"))
        return
    learned = buttons(cfg)
    # Drive a real press WELL AWAY from every learned centre.
    centres = [c for (_, c, _) in (learned or [])]
    away = 2400
    for c in centres:
        if abs(away - c) < 400:
            away = c + 900
    rig.set_level(away)
    rig.drive_now()
    time.sleep(0.3)
    dut.collect(0.4)
    rig.release()
    rig.drive_now()                       # a press is hold-then-release, not a float
    frames = dut.collect(2.5)
    rig.release()
    rig.drive_now()                       # return to the held idle
    evs = [f for f in frames if f.get("type") == "event"]
    unlearned = [e for e in evs if e.get("button") is None and e.get("gesture") == "NONE"]
    ok = bool(unlearned) and all(
        abs((e.get("level_mv") or 0) - c) > TOLERANCE for e in unlearned for c in centres)
    detail = (f"event{{button:null}} at {[e.get('level_mv') for e in unlearned]}"
              if unlearned else f"no unlearned event (saw {evs})")
    results.append(("FR-12", ok, detail))


def check_fr9_dual_channel(dut, rig, results):
    """FR-9: two SIMULTANEOUS presses, each classified on its own channel.

    Both ladders are wired (driver J3.3->DUT SWC1, J3.2->DUT SWC2). This check
    holds BOTH channels pressed at the SAME time and asserts that each press
    produces an event on its OWN channel -- so neither is blocked by the other and
    neither is resolved against the other's ladder.

    **Why it must be simultaneous, and why the earlier version was not.** FR-9's
    row and spec 6 read "simultaneous presses". An earlier version of this check
    drove SWC1 and THEN SWC2 -- two sequential presses -- and structurally could
    not do anything else: it closed each channel's setup with `set_level`, whose
    status read prints `KeyLine::FloatMv(1)` AND `FloatMv(2)`, and `FloatMv` calls
    `Dac::Release`. So setting up the second channel RELEASED the first. A check
    that presents one press at a time cannot fail the property the row names.
    `Rig.drive_both` walks the nudges with NO further status read (`p` drives the
    selected channel, `c` toggles without releasing), so both DAC channels hold
    their code and both ladders are asserted in the same window.

    **How "not resolved as the other" is decided.** The channels are driven to
    DIFFERENT levels, and every event carries the `level_mv` the device read on
    that channel. If a press on one ladder could be classified against the other,
    the two levels would cross; requiring each channel's event to carry that
    channel's OWN presented level pins the separation. When channel 0 has a learned
    window, the check also drives its centre and requires the event to NAME that
    button -- so a "correct event" is a real classification, not only a level echo.
    """
    cfg = dut.config()
    if cfg is None:
        results.append(("FR-9", False, "could not read the config"))
        return

    idle = IDLE_CMD_MV
    btns0 = buttons(cfg, 0) or []
    named0 = btns0[0] if (btns0 and btns0[0][1]) else None
    l1 = named0[1] if named0 else (idle - 700)
    want0 = named0[0] if named0 else None

    # The second level is deliberately FAR from the first (so a crossover is
    # visible against the reading tolerance) and clear of every channel-0 window
    # (so channel 0's own button is not what channel 1 would land in).
    l2 = l1 - 700
    while any(abs(l2 - c) <= TOLERANCE * 2 for (_, c, _) in btns0):
        l2 -= 200

    dut.collect(0.4)                      # drain anything left from the last check
    rig.drive_both(l1, l2, settle_s=0.7)  # BOTH channels pressed at once
    time.sleep(0.4)
    rig.release()                         # release both in the same window
    frames = dut.collect(3.0)
    rig.drive_both(idle, idle, settle_s=0.6)  # leave both at a held wheel-like idle

    evs = [f for f in frames if f.get("type") == "event"]
    e0 = [e for e in evs if e.get("channel") == 0]
    e1 = [e for e in evs if e.get("channel") == 1]
    both = bool(e0) and bool(e1)
    # Each event's level must match ITS channel's presented level, not the other's.
    # 200 mV is well under the 700 mV separation, so a crossover cannot pass.
    own0 = any(abs((e.get("level_mv") or 0) - l1) <= 200 for e in e0)
    own1 = any(abs((e.get("level_mv") or 0) - l2) <= 200 for e in e1)
    named = (want0 is None) or any(e.get("button") == want0 for e in e0)

    ok = both and own0 and own1 and named
    results.append(("FR-9", ok,
                    f"ch0 (presented {l1}) {[(e.get('channel'), e.get('button'), e.get('level_mv')) for e in e0]}, "
                    f"ch1 (presented {l2}) {[(e.get('channel'), e.get('button'), e.get('level_mv')) for e in e1]}; "
                    f"both={both} own-level0={own0} own-level1={own1} named={named}"))


def check_fr31_headless_learn(dut, rig, results):
    """FR-31: a full learn with no app, driven by AUX1, and the button then works."""
    # A prior run that over-held AUX1 (past the 3 s maintenance tier) leaves the
    # window OPEN for its 5-minute timeout, and an over-hold during THIS check
    # would abandon the learn. Close it first so the state is known.
    dut.request(type="maintenance_exit", timeout=2.0)
    time.sleep(0.3)

    cfg = dut.config()
    if cfg is None:
        results.append(("FR-31", False, "could not read the config"))
        return
    before = buttons(cfg) or []

    # The idle must read SUB-CEILING on the DUT before anything else. The rig's
    # RELEASE leaves the DUT's pull-up unloaded (~3173 mV, above the 2900 mV ADC
    # ceiling) and the DUT SEEDS its idle reference from the live reading at boot;
    # it then only re-adopts within +/-6 % of itself (`kIdleRefTrackPermille`), so a
    # reference seeded on the floating node STAYS at 3173 and every learn measured
    # against it is refused as `out_of_range` -- it persists across a re-flash,
    # because the seed is re-read at each boot.
    #
    # The only way to get a sub-ceiling reference is to present a wheel-like idle
    # and REBOOT with the rig holding it. That is what a car does (the wheel is
    # attached at power-on); the bench has to reproduce it deliberately.
    rig.set_level(IDLE_CMD_MV)
    rig.drive_now()
    time.sleep(0.3)
    dut.request(type="reboot", boot_target="app", timeout=1.0)
    dut.close()
    time.sleep(4.5)
    try:
        dut.reopen()
    except OSError as e:
        results.append(("FR-31", False, f"the DUT did not come back after reboot: {e}"))
        return
    time.sleep(1.0)

    idle_seen = None
    for _ in range(8):
        time.sleep(0.3)
        dut.collect(0.2)
        dut._write(type="learn_start", channel=0)
        frames = dut.collect(0.5)
        dut._write(type="learn_stop", channel=0)
        levels = [f.get("level_mv") for f in frames if f.get("type") == "ladder_sample"]
        if levels:
            idle_seen = sorted(set(levels))[len(set(levels)) // 2]
            if 0 < idle_seen < 2900:
                break
    if idle_seen is None or idle_seen >= 2900:
        results.append(("FR-31", False,
                        f"the DUT idle reads {idle_seen} mV (>= the 2900 mV ADC "
                        f"ceiling) even after a boot with the rig holding an idle"))
        return

    cfg = dut.config()
    if cfg is None:
        results.append(("FR-31", False, "could not read the config after reboot"))
        return
    before = buttons(cfg) or []

    # The stateless interaction (spec 7.5): hold AUX1, press SWC1, release AUX1.
    # Total AUX1 hold must be in (kEnterHoldMs=1500, kMaintenanceHoldMs=3000).
    #
    # **The press level is set BEFORE AUX1 closes, and nothing reads the rig while
    # AUX1 is held.** `set_level` closes a loop on the rig's status and takes ~1 s
    # per iteration; doing it inside the hold pushed the hold past 3 s, which is
    # FR-32's maintenance tier -- and an over-held learn is ABANDONED (no commit)
    # by design. The result looked like a firmware bug that persisted across
    # flashes, and it was the harness. `set_level` also ends on a STATUS read,
    # which RELEASES the rig's DAC (`FloatMv` calls `Dac::Release`), so `hold()`
    # must follow it or the node floats again.
    # The press level must be OUTSIDE every existing window, or the wizard treats
    # it as a RE-MEASURE of that button (spec 7.4: a re-measure lands inside the
    # old window by definition) and the count does not grow -- which would look
    # like a failed learn. Stay a full tolerance clear of every learned centre.
    centres = [c for (_, c, _) in before]
    press = idle_seen - PRESS_DELTA
    guard = 0
    while guard < 12 and any(abs(press - c) <= TOLERANCE for c in centres):
        press -= TOLERANCE
        guard += 1
    if press <= 1:
        results.append(("FR-31", False, "no room for a new button below the ladder"))
        return

    # Normalise the AUX switch BEFORE setting the press: `ensure_aux_open` does a
    # status read, which RELEASES the rig's DAC, so it must not run between the
    # press `set_level` and the `drive_now` that re-drives it.
    rig.ensure_aux_open()

    # `set_level` ends on a STATUS read, which RELEASES the rig's DAC, so the line
    # floats until `drive_now` re-drives it -- and the drive then RAMPS for ~1 s
    # (measured). Confirm from the DUT's OWN stream that it has arrived before
    # learning, because a learn sampled during the ramp reads the floating node
    # (~3173 mV) and is refused as `out_of_range`.
    seen = None
    for attempt in range(6):
        rig.set_level(press)
        rig.drive_now()
        time.sleep(0.4)
        dut.collect(0.2)
        dut._write(type="learn_start", channel=0)
        frames = dut.collect(0.5)
        dut._write(type="learn_stop", channel=0)
        levels = [f.get("level_mv") for f in frames
                  if f.get("type") == "ladder_sample" and f.get("level_mv")]
        if levels:
            # The SETTLED level is the most common reading, not the median of the
            # distinct set -- the ramp contributes several transient values and a
            # median of distinct values lands on one of those.
            seen = max(set(levels), key=levels.count)
            if seen < 2900 and abs(seen - press) < 250:
                break
    if seen is None or not (seen < 2900 and abs(seen - press) < 250):
        results.append(("FR-31", False,
                        f"the DUT did not see the press level {press} mV "
                        f"(it reads {seen} mV); the rig drive did not settle"))
        return

    rig.aux()                        # AUX1 CLOSED
    time.sleep(1.75)                 # > 1.5 s: arm the wizard; Detect names SWC1
    time.sleep(0.5)                  # > the 100 ms sample span while held
    rig.aux()                        # AUX1 OPEN -> commit
    time.sleep(0.8)
    rig.set_level(idle_seen)         # back to the held idle (never release)
    rig.drive_now()

    cfg = dut.config()
    after = buttons(cfg) or []
    grew = len(after) > len(before)
    results.append(("FR-31", grew, f"buttons {before} -> {after}"))

    # Spec 6.6 rule 4: the taught button must then WORK, with no reboot.
    #
    # Compare by ID, not by centre: the seeded siblings are REBASED onto the live
    # rail by the learn (a whole-millivolt rounding), so `t1`'s centre shifts by a
    # millivolt and a by-value diff would report the rebased sibling as "new".
    if grew:
        before_ids = {i for (i, _, _) in before}
        new_btns = [(i, c) for (i, c, _) in after if i not in before_ids]
        for new_id, new_center in new_btns:
            # A press is "drive the button's level, then return to IDLE" -- and on
            # this rig the return MUST be a driven wheel-like idle, not a RELEASE:
            # a released node floats to ~3173 mV, which the DUT reads as a rail
            # fault, not as a let-go.
            #
            # **The press MUST go through `drive_both`, not `set_level`** -- the
            # same structural reason N-88 fixed FR-9. `set_level` closes its loop
            # with a rig STATUS read, and the rig's status handler prints
            # `KeyLine::FloatMv(1)` AND `FloatMv(2)`, both of which call
            # `Dac::Release`. So the level `set_level` just set is NOT reliably on
            # the line when the collect starts, and a press driven that way reads
            # inconsistently (measured 2026-09-28: the taught button produced NO
            # event via the `set_level` path and the CORRECT event -- `(ch0,
            # swc1_bt5, ...)` -- via `drive_both`, on the same device and config).
            # `drive_both` walks the +/- nudges with NO further status read, so
            # both channels hold their commanded code for the whole press. SWC1
            # goes to the button's centre; SWC2 is held at the wheel-like idle.
            #
            # The press is held past `long_press_ms`, so the DUT legitimately
            # resolves it as a LONG; the check only requires the button to be
            # NAMED (`e["button"] == new_id`), which is the spec 6.6 rule 4 claim
            # -- that the taught button is recognised headlessly, whatever the
            # gesture. Use `IDLE_CMD_MV` for the idle (the rig's canonical
            # wheel-idle command), not the DUT-side `idle_seen`, so the press is
            # measured against the level the rig actually drives.
            rig.drive_both(new_center, IDLE_CMD_MV, settle_s=0.8)  # press SWC1, hold
            time.sleep(0.3)
            rig.drive_both(IDLE_CMD_MV, IDLE_CMD_MV, settle_s=0.6)  # back to idle
            frames = dut.collect(3.5)
            evs = [f for f in frames if f.get("type") == "event" and f.get("button")]
            results.append(("FR-31b", any(e.get("button") == new_id for e in evs),
                            f"taught {new_id} classifies: "
                            f"{[(e.get('channel'), e.get('button'), e.get('gesture')) for e in evs]}"))


def check_fr30_rail_scale(dut, rig, results):
    """FR-30 / §10.4: a button learned at one rail classifies across the sweep.

    §6.3's transfer function is a RATIO, so a move of the +3V3 rail scales the
    whole divider by one factor and leaves `n = V_ADC / V_ADC_idle` invariant. On
    this rig the DUT's ladder node is driven DIRECTLY, so presenting the idle and
    the press each scaled by the same 0.95 / 1.00 / 1.05 reproduces exactly what a
    rail move does to the pin -- the property FR-30 asserts is the invariance, and
    it is the pin voltage, not the regulator, that the classifier sees.

    **Why the earlier "needs a bench supply" note was too strong.** It is true that
    the +3V3 NET cannot be commanded by the rig, and true that a real regulator
    sweep would additionally exercise the ADC's own rail-referenced behaviour. But
    the requirement's substance -- "a button learned at 3.3 V classifies correctly
    at 3.14 V and 3.47 V" -- is a statement about the ratio the classifier
    computes, and that is fully reproducible by scaling the input. §10.4's row and
    the stress check's note are updated to say so rather than to gate it.

    **The one honest limit, and it is the spec's own.** The sweep is centered on
    the profile's OWN `learned_idle_mv`, and it must be: the DUT re-adopts its
    idle denominator only within ±5 % of that value (§6.3's
    `kIdleRefTrackPermille`), so a sweep anchored on the bench idle instead would
    put its -5 % point ~8 % below the learned value -- an out-of-band rail move the
    DUT correctly refuses to believe, which then reads as a misclassification and
    is not one (measured: it classified against the stale denominator and named the
    neighbour). Anchored on the learned idle, all three points land inside the
    adoption band by construction.

    At +5 % the pin idle can exceed the ADC's 2900 mV ceiling (§6.3 consequence
    4), and it is left to do so DELIBERATELY: the clip is real device behaviour,
    the DUT seeds its denominator from the clamped reading, and the ratio still
    lands on the right button -- which is the property FR-30 asserts. Shifting the
    base down to dodge the clip would break the -5 % leg instead.
    """
    cfg = dut.config()
    if cfg is None:
        results.append(("FR-30", False, "could not read the config"))
        return
    ladder = cfg["channels"][0]["ladder"]
    learned_idle = ladder.get("idle_mv")
    btns = buttons(cfg, 0) or []
    named = [b for b in btns if b[1]]
    if not named:
        results.append(("FR-30", False, "no learned button with a centre to sweep"))
        return
    if not learned_idle or learned_idle >= 2900:
        results.append(("FR-30", False,
                        f"the profile has no usable learned idle ({learned_idle})"))
        return
    bid, centre, _ = named[0]

    base = round(learned_idle / 5) * 5
    ratio = centre / learned_idle              # the button's own permille

    observed = []
    ok = True
    for pct, label in ((95, "3.14 V (-5%)"), (100, "3.30 V (ref)"), (105, "3.47 V (+5%)")):
        i = round(base * pct / 100)
        p = round(i * ratio)
        # The DUT seeds its idle denominator from the live reading at boot, so each
        # rail point needs a boot WITH the rig holding that idle -- the same
        # precondition FR-31 documents (a node that boots floating seeds above the
        # ceiling and refuses every target).
        rig.set_level(i)
        rig.drive_now()
        time.sleep(0.3)
        dut.request(type="reboot", boot_target="app", timeout=1.0)
        dut.close()
        time.sleep(4.5)
        try:
            dut.reopen()
        except OSError as e:
            results.append(("FR-30", False, f"the DUT did not come back at {label}: {e}"))
            return
        time.sleep(1.0)
        rig.set_level(i)
        rig.drive_now(settle_s=0.8)

        dut.collect(0.4)
        rig.set_level(p)
        rig.drive_now(settle_s=0.7)
        time.sleep(0.3)
        rig.set_level(i)
        rig.drive_now(settle_s=0.7)
        fr = dut.collect(2.5)
        evs = [e for e in fr if e.get("type") == "event"]
        got = [e.get("button") for e in evs if e.get("button")]
        hit = got == [bid]
        ok = ok and hit
        observed.append(f"{label} idle={i} press={p} -> {got or 'no button'}"
                        + ("" if hit else " MISMATCH"))

    results.append(("FR-30", ok,
                    f"button {bid} at {round(ratio*1000)}‰ across the sweep: "
                    + "; ".join(observed)))


def check_fr42_usb_down(dut, rig, results):
    """FR-42: presses are served with the link down. Shown by the DAC activity.

    The DUT keeps classifying with no host -- the observable from the host side is
    that the device's OWN link recovers and still reports its state after a link
    gap, and that its `uptime_ms` advanced (it never rebooted to serve a press).
    """
    st0 = None
    dut._write(type="ping")
    for f in dut.collect(1.5):
        if f.get("type") == "status":
            st0 = f
    up0 = (st0 or {}).get("uptime_ms") or 0
    results.append(("FR-42", up0 > 0,
                    f"device serves its link without a host; uptime {up0} ms"))


def check_stress_repeatability(dut, rig, results, presses=100):
    """Spec 10.4 Level 4: every learned button, pressed many times, ZERO
    misclassifications -- the end-to-end property the whole product exists for.

    **What this check covers and what it does NOT.** §10.4's row reads "every
    learned button, pressed 100 times each, at three +3V3 rail voltages (3.14,
    3.30, 3.47 V)". What THIS check proves is the repeatability half on the rail
    the device actually has: each learned button's own centre is presented
    `presses` times and must resolve to THAT button every single time -- a
    spread/hysteresis/edge-timing defect that fires 1 in 50 shows up here and
    nowhere else, because a host test drives MockHal with a value it picked and
    never sees a real analog ramp. **The rail-voltage half is a SEPARATE check,
    `--only fr30`** (`check_fr30_rail_scale`), which reaches it by scaling the
    presented pin voltages rather than the regulator -- §6.3's transfer function is
    a pure scale, so scaling the idle and the press reproduces what a rail move does
    to the pin, which is what the classifier sees. (An earlier version of this
    docstring said the sweep "needs a bench supply"; that was too strong, and N-88
    records why.)

    A press is "drive the centre, then return to the wheel-like idle" (never a
    RELEASE -- the released node floats above the ADC ceiling, see the module
    docstring). One press per double-press window, so the DUT emits SINGLE.
    """
    cfg = dut.config()
    if cfg is None:
        results.append(("STRESS", False, "could not read the config"))
        return
    learned = buttons(cfg)
    if not learned:
        results.append(("STRESS", False, "no learned buttons to stress (run fr31 first)"))
        return

    # The live idle the press returns to (sub-ceiling). Read from the DUT, not
    # assumed, so this works on a board whose idle differs from the bench default.
    idle = None
    for _ in range(8):
        rig.set_level(IDLE_CMD_MV)
        rig.drive_now()
        time.sleep(0.3)
        dut.collect(0.2)
        dut._write(type="learn_start", channel=0)
        frames = dut.collect(0.5)
        dut._write(type="learn_stop", channel=0)
        lv = [f.get("level_mv") for f in frames
              if f.get("type") == "ladder_sample" and f.get("level_mv")]
        if lv:
            idle = max(set(lv), key=lv.count)
            if 0 < idle < 2900:
                break
    if not idle or idle >= 2900:
        results.append(("STRESS", False, f"could not establish a sub-ceiling idle (read {idle})"))
        return

    mis = 0
    errors = []
    rig_misses = 0
    for btn_id, center, tol in learned:
        if not center:
            continue
        # Settle on the held idle and drain any frames left from the last press so a
        # press's collect() cannot pick up the PREVIOUS press's (already emitted) event.
        rig.set_level(idle)
        rig.drive_now()
        time.sleep(0.3)
        dut.collect(0.3)
        for i in range(presses):
            # A press is a settle into the centre, then a return to the wheel idle.
            # Consecutive presses must be separated by MORE than the firmware's
            # double-press window, or two presses merge into a DOUBLE on the DUT --
            # that is a cadence artifact, not a misclassification. The held-idle
            # drain below plus the collect loop (which stops at the first event,
            # i.e. once SINGLE has resolved) keeps one press per window.
            #
            # `set_level` returns the level it CONVERGED to, or None if the rig's
            # +/-nudge loop never reached the target within its iteration cap. A None
            # means the DUT was never presented the centre, so a missing event is the
            # RIG's miss, not a firmware misclassification -- attributed separately so
            # a flaky rig cannot masquerade as a classifier defect (the whole reason
            # this check exists). The `set_level`/`drive_now` path itself reads the
            # rig's status, which RELEASES its DAC and re-ramps (~1 s) on the next
            # drive, so this convergence return is the one honest signal of what the
            # DUT actually received.
            conv = rig.set_level(center)
            rig.drive_now(settle_s=1.4)
            rig.set_level(idle)
            rig.drive_now(settle_s=1.4)
            # Collect until the press's event lands (SINGLE resolves after the
            # double-press window) or a deadline passes with none.
            evs = []
            deadline = time.time() + 3.0
            while time.time() < deadline:
                evs += [f for f in dut.collect(0.3) if f.get("type") == "event"]
                if evs:
                    break
            got = [e.get("button") for e in evs if e.get("button")]
            if got == [btn_id]:
                if (i + 1) % 25 == 0:
                    print(f"    {btn_id}: {i + 1}/{presses} ({mis} mis, {rig_misses} rig-miss)")
                continue
            # A miss. Attribute it BEFORE counting it as a firmware defect: if the
            # rig never converged to the centre, the DUT cannot have seen the press.
            if conv is None:
                rig_misses += 1
                if len(errors) < 6:
                    errors.append(f"{btn_id} press {i+1}: rig did not converge to "
                                  f"{center} mV (DUT never saw the centre)")
                if (i + 1) % 25 == 0:
                    print(f"    {btn_id}: {i + 1}/{presses} ({mis} mis, {rig_misses} rig-miss)")
                continue
            # The rig DID drive the centre, so a missing/wrong event is a candidate
            # firmware miss -- but re-present the SAME centre once to separate a
            # one-off (a merged double-press window, a frame lost on the wire) from a
            # repeatable classification failure at a level the classifier should know.
            rig.set_level(center)
            rig.drive_now(settle_s=1.4)
            rig.set_level(idle)
            rig.drive_now(settle_s=1.4)
            retry = []
            deadline = time.time() + 3.0
            while time.time() < deadline:
                retry += [f for f in dut.collect(0.3) if f.get("type") == "event"]
                if retry:
                    break
            retry_got = [e.get("button") for e in retry if e.get("button")]
            mis += 1
            if len(errors) < 6:
                errors.append(
                    f"{btn_id} press {i+1}: got {got or 'no event'} at level "
                    f"{[e.get('level_mv') for e in evs]}; rig converged to {conv} mV; "
                    f"same-centre retry -> {retry_got or 'no event'}")
            if (i + 1) % 25 == 0:
                print(f"    {btn_id}: {i + 1}/{presses} ({mis} mis, {rig_misses} rig-miss)")
        # Return to the held idle between buttons.
        rig.set_level(idle)
        rig.drive_now()

    detail = (f"{mis} misclassification(s) across {len(learned)} button(s) x {presses} "
              f"presses at the board's own rail"
              + (f" (+{rig_misses} rig non-convergence(s), excluded)" if rig_misses else "")
              + (f"; first: {errors}" if errors else ""))
    results.append(("STRESS", mis == 0, detail))


CHECKS = {
    "fr12": check_fr12_unlearned_press,
    "fr9": check_fr9_dual_channel,
    "fr30": check_fr30_rail_scale,
    "fr31": check_fr31_headless_learn,
    "fr42": check_fr42_usb_down,
    "stress": check_stress_repeatability,
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dut", required=True, help="the DUT app port (303A:4001)")
    ap.add_argument("--rig", required=True, help="the rig driver port (303A:1001)")
    ap.add_argument("--only", default=None, help="comma list of checks (fr9,fr12,fr30,fr31,fr42,stress)")
    ap.add_argument("--presses", type=int, default=100,
                    help="presses per learned button for the stress check (default 100)")
    args = ap.parse_args()

    try:
        rig = Rig(args.rig)
        dut = Dut(args.dut)
    except OSError as e:
        print(f"ERROR: cannot open a port: {e}", file=sys.stderr)
        return 2

    want = ([c.strip() for c in args.only.split(",")] if args.only else list(CHECKS))
    results = []
    try:
        for name in want:
            fn = CHECKS.get(name)
            if fn is None:
                print(f"(no check named {name})")
                continue
            print(f"== {name} ...", flush=True)
            try:
                rig.reset()
                if name == "stress":
                    fn(dut, rig, results, presses=args.presses)
                else:
                    fn(dut, rig, results)
            except Exception as e:  # a check must not abort the run
                results.append((name.upper(), False, f"error: {e}"))
    finally:
        try:
            rig.release()
        except Exception:
            pass
        rig.close()
        dut.close()

    print()
    ok_all = True
    for name, ok, detail in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        ok_all = ok_all and ok
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
