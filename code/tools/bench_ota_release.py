#!/usr/bin/env python3
"""Prove FR-35's git-release update path end to end on the board: the device
fetches a release MANIFEST over verified TLS, decides, downloads the named image
over HTTPS through the same verify gate, commits it, and reboots into it.

**What this adds over `bench_wifi_upload.py`.** That tool proves the *upload*
endpoint (`POST /api/ota/upload`, a browser-pushed file). This one proves the
*scheme* endpoints `/api/ota/check` and `/api/ota/pull` -- the git-release path
spec §9.5 describes, where the device fetches the manifest and image itself with
`esp_http_client` + the shared `OtaBegin`/`OtaChunk`/`OtaEnd` gate
(`OtaWifiCheck`/`OtaWifiInstall`). Nothing else exercises those two functions on
silicon.

**How the release is served without publishing one.** The manifest URL is a
compile-time constant (`MaintenanceRadio.cpp`'s `kReleaseManifestUrl`, a GitHub
`releases/latest/download` alias). A bench build can override it with
`-D SWC_BENCH_MANIFEST_URL=\"https://...\"` (a `SWC_BENCH_*` switch, so a shipped
build cannot be repointed). This tool stands up a **Cloudflare quick tunnel**
(`cloudflared tunnel --url http://localhost:PORT`, no account needed) in front of
a local static server holding a manifest and a real image. The tunnel hostname is
**publicly trusted** (a real `*.trycloudflare.com` cert), so the device's
VERIFIED-TLS fetch succeeds -- and would NOT over a self-signed LAN cert. That is
why a tunnel and not a local HTTPS server.

**The clock defect this found.** The device has no RTC and `time_sync` is a no-op,
so a fresh boot's clock reads 1970 and mbedTLS rejects a modern server cert as
"not yet valid". `OtaWifiCheck` maps that to "the release manifest could not be
read", which names nothing near the cause. The firmware now starts SNTP on
`IP_EVENT_STA_GOT_IP` (`MaintenanceRadio.cpp`), so the clock is valid by the time
the fetch runs. This tool is what surfaced it.

**Sequence (all on the bench host; the Mac's own Wi-Fi is never touched):**

  1. Start the static server + tunnel; build and stage a SERVED image whose
     version is NEWER than the running firmware, and a manifest naming it (true
     size + sha256, so a successful install also exercises the digest gate).
  2. Build the DUT image with the tunnel URL baked in and flash it over USB OTA,
     unless `--skip-dut-flash` (reuse a DUT already running such an image).
  3. Open the maintenance window, provision the DUT onto the bench LAN (Sec1 over
     BLE with the `code/.env` credentials), reach its HTTP server at its LAN IP.
     **Provision and drive HTTP in the SAME window** (an exit->enter cycle leaves
     the driver STA-only -- see `bench_wifi_upload.py`).
  4. `POST /api/ota/check` -> "an update is available: <version>". This is
     `OtaWifiCheck` on silicon: manifest fetched over TLS, `ReleaseCheck` decided.
  5. `POST /api/ota/pull` -> "installing"; the device downloads + verifies +
     commits; then a reboot shows the NEW version running.

Usage:
  # full run (builds + flashes; ~15 min):
  python3 tools/bench_ota_release.py
  # reuse a tunnel you already have and a DUT already flashed with an override:
  python3 tools/bench_ota_release.py --manifest-url https://x.trycloudflare.com/v.json \
      --skip-dut-flash

Exit codes: 0 the round-trip passed, 1 a check failed (a real defect), 2 usage /
environment error.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_ladder import Dut                      # noqa: E402
import bench_prov                                 # noqa: E402
import bench_wifi_upload as wu                    # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PORT = "/dev/cu.usbmodem1234561"
NEW_VERSION = "1.0.0"


# --- the release host (static server + quick tunnel) -----------------------

def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ReleaseServer:
    """A static file server behind a Cloudflare quick tunnel: a PUBLIC,
    publicly-trusted HTTPS URL the device's verified-TLS fetch accepts."""

    def __init__(self, docroot, logdir):
        self.docroot = docroot
        self.logpath = os.path.join(logdir, "cloudflared.log")
        self.port = None
        self.url = None
        self._http = None
        self._cf = None

    def start(self):
        self.port = _free_port()
        self._http = subprocess.Popen(
            [sys.executable, "-m", "http.server", str(self.port),
             "--bind", "127.0.0.1", "--directory", self.docroot],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cf = shutil.which("cloudflared")
        if cf is None:
            raise RuntimeError("cloudflared not found. The device's fetch needs a "
                               "PUBLICLY TRUSTED https host; install cloudflared.")
        log = open(self.logpath, "w")
        self._cf = subprocess.Popen([cf, "tunnel", "--url",
                                     f"http://localhost:{self.port}"],
                                    stdout=log, stderr=log)
        deadline = time.time() + 40
        while time.time() < deadline:
            m = re.search(r"https://[a-z0-9-]+\.trycloudflare\.com",
                          open(self.logpath).read())
            if m:
                self.url = m.group(0)
                return
            time.sleep(0.5)
        raise RuntimeError("cloudflared did not report a tunnel URL in 40 s")

    def reachable(self, path="/", timeout=40):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(self.url + path, timeout=8) as r:
                    if r.status == 200:
                        return True
            except Exception:
                pass
            time.sleep(1)
        return False

    def stop(self):
        for p in (self._cf, self._http):
            if p is not None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()


def stage_release(docroot, base_url, image_path):
    """Copy the image in and write a manifest whose version is newer than the
    running firmware, carrying the image's TRUE size and sha256."""
    data = open(image_path, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    shutil.copyfile(image_path, os.path.join(docroot, "firmware.bin"))
    manifest = {
        "latest_version": NEW_VERSION,
        "channel": "stable",
        "firmware": {"version": NEW_VERSION, "url": f"{base_url}/firmware.bin",
                     "size_bytes": len(data), "sha256": sha},
        "min_from_version": "0.0.0",
    }
    with open(os.path.join(docroot, "version_manifest.json"), "w") as f:
        json.dump(manifest, f)
    return len(data), sha


# --- build + flash ---------------------------------------------------------

def build_image(outdir, version, sha_tag, extra_flags=None):
    env = dict(os.environ)
    env["SWC_FW_VERSION"] = version
    env["SWC_GIT_SHA"] = sha_tag
    env["PLATFORMIO_BUILD_DIR"] = outdir
    if extra_flags:
        env["PLATFORMIO_BUILD_FLAGS"] = extra_flags
    print(f"== building an image (v{version}) into {outdir}")
    r = subprocess.run(["pio", "run", "-e", "esp32s3"], cwd=REPO, env=env,
                       capture_output=True, text=True, timeout=900)
    if r.returncode != 0:
        print(r.stdout[-2000:], file=sys.stderr)
        print(r.stderr[-2000:], file=sys.stderr)
        return None
    path = os.path.join(outdir, "esp32s3", "firmware.bin")
    return path if os.path.exists(path) else None


def flash_usb(port, image):
    print("== flashing the DUT over USB OTA (several minutes)…")
    r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "dev_push_ota.py"),
                        "--port", port, "--image", image],
                       capture_output=True, text=True, timeout=1200)
    print("   " + "\n   ".join((r.stdout or "").splitlines()[-2:]))
    return r.returncode == 0


# --- HTTP against the device -----------------------------------------------

def http_post(ip, path, token, timeout=120):
    req = urllib.request.Request(f"http://{ip}{path}", method="POST")
    req.add_header("X-SWC-Token", token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return None, str(e).encode()


def running_version(dut):
    dut.s.dtr = True                      # `hello` is emitted on a DTR-asserted connect
    try:
        for f in dut.collect(3.0):
            if f.get("type") == "hello":
                return f.get("fw_version")
    finally:
        dut.s.dtr = False
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--ssid", default=None)
    ap.add_argument("--passphrase", default=None)
    ap.add_argument("--ip", default=None)
    ap.add_argument("--manifest-url", default=None,
                    help="reuse an existing tunnel's manifest URL instead of "
                         "starting one (the DUT's baked override must point here)")
    ap.add_argument("--skip-dut-flash", action="store_true",
                    help="assume the DUT already runs an image whose override "
                         "points at --manifest-url; do not build/flash it")
    ap.add_argument("--keep-servers", action="store_true")
    args = ap.parse_args()

    ssid = args.ssid or wu.env_value("WIFI_SSID")
    passphrase = args.passphrase if args.passphrase is not None else wu.env_value("WIFI_PASSWORD")
    if not ssid or passphrase is None:
        print("ERROR: set WIFI_SSID/WIFI_PASSWORD in code/.env.", file=sys.stderr)
        return 2
    esp_prov = bench_prov.find_esp_prov()
    if esp_prov is None:
        print("ERROR: esp_prov.py not found.", file=sys.stderr)
        return 2
    if args.skip_dut_flash and not args.manifest_url:
        print("ERROR: --skip-dut-flash needs --manifest-url (the URL the DUT's "
              "baked override already points at).", file=sys.stderr)
        return 2

    workdir = tempfile.mkdtemp(prefix="swc_release_")
    docroot = os.path.join(workdir, "www")
    os.makedirs(docroot)
    server = None
    dut = None
    failed = False
    try:
        # --- 1. the release host + a newer image -----------------------------
        if args.manifest_url:
            manifest_url = args.manifest_url
            base_url = manifest_url.rsplit("/", 1)[0]
            print(f"reusing manifest {manifest_url}")
        else:
            server = ReleaseServer(docroot, workdir)
            server.start()
            base_url = server.url
            manifest_url = server.url + "/version_manifest.json"
            print(f"release host up: {base_url}")

        served_dir = os.path.join(workdir, "served_build")
        served = build_image(served_dir, NEW_VERSION, "benchrel")
        if served is None:
            return 1
        size, sha = stage_release(docroot, base_url, served)
        print(f"staged: v{NEW_VERSION}, {size} bytes, sha {sha[:16]}…")
        if server and not server.reachable("/version_manifest.json"):
            print("ERROR: the staged manifest is not reachable through the tunnel.",
                  file=sys.stderr)
            return 2

        # --- 2. the DUT image with the override baked in ---------------------
        if not args.skip_dut_flash:
            dut_dir = os.path.join(workdir, "dut_build")
            # The override is a plain string define; no shell is involved, so the
            # escaped quotes are literal and reach the C preprocessor as-is.
            flags = f'-D SWC_BENCH_MANIFEST_URL=\\"{manifest_url}\\"'
            dutimg = build_image(dut_dir, "0.9.0", "benchrel", flags)
            if dutimg is None:
                return 1
            if not flash_usb(args.port, dutimg):
                print("ERROR: USB-OTA flash failed.", file=sys.stderr)
                return 1
            time.sleep(3)

        dut = Dut(args.port)
        print(f"\nDUT running version: {running_version(dut)}")

        # --- 3. window + provisioning + LAN ----------------------------------
        open_ = wu.open_window(dut)
        if not open_:
            return 1
        ble_name, pop, token = open_
        adv = "SWC-" + ble_name
        print(f"window open: adv {adv}, token {token}")
        time.sleep(4)
        rc, out = bench_prov.run_esp_prov(esp_prov, adv, pop, ssid, passphrase,
                                          timeout=180)
        joined = "WiFi state: Connected" in bench_prov.strip_ansi(out)
        print(f"provision rc={rc} connected={joined}")
        if not joined:
            print(bench_prov.strip_ansi(out)[-400:], file=sys.stderr)
            return 1
        ip = args.ip
        deadline = time.time() + 40
        while time.time() < deadline:
            cand = ip or wu.arp_ip_for_mac(wu.station_mac(ble_name)) or wu.DEFAULT_LAN_IP
            st, _ = http_post(cand, "/api/status", token, timeout=6)
            if st is not None:
                ip = cand
                break
            time.sleep(1)
        if not ip:
            print("ERROR: the device never answered on the LAN.", file=sys.stderr)
            return 1
        print(f"device on the LAN at {ip}")

        # --- 4. /api/ota/check : the manifest fetch + decision, on silicon ---
        st, body = http_post(ip, "/api/ota/check", token, timeout=60)
        text = body.decode("utf-8", "replace")
        ok = st == 200 and "update is available" in text and NEW_VERSION in text
        print(f"\n4. POST /api/ota/check -> {st} {text.strip()!r}  "
              f"{'PASS (manifest fetched + newer)' if ok else 'FAIL'}")
        failed |= not ok

        # --- 5. /api/ota/pull : download over HTTPS, verify, commit ----------
        st, body = http_post(ip, "/api/ota/pull", token, timeout=240)
        text = body.decode("utf-8", "replace")
        ok = st == 200 and "installing" in text
        print(f"5. POST /api/ota/pull  -> {st} {text.strip()!r}  "
              f"{'PASS (download started)' if ok else 'FAIL'}")
        failed |= not ok

        # --- 6. the new image is running after a reboot ----------------------
        print("   waiting for the install to commit, then rebooting…")
        time.sleep(20)
        try:
            dut.request(type="reboot", boot_target="app", timeout=3.0)
        except Exception:
            pass
        dut.close()
        dut = None
        time.sleep(6)
        dut = Dut(args.port)
        newver = running_version(dut)
        ok = (newver == NEW_VERSION)
        print(f"6. after reboot the device runs {newver}  "
              f"{'PASS (new image running)' if ok else 'FAIL'}")
        failed |= not ok
    finally:
        if dut is not None:
            try:
                dut.request(type="maintenance_exit", timeout=3.0)
            except Exception:
                pass
            dut.close()
        if server is not None and not args.keep_servers:
            server.stop()

    print("\n" + ("FAIL: see the step above." if failed else
                  "PASS: FR-35 git-release -- manifest fetched over verified TLS, "
                  "image downloaded + verified + committed, device rebooted into it."))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
