#!/usr/bin/env python3
"""Prove the N-92 fix on the DUT: the PRODUCTION release URL (no override) now
resolves for an unauthenticated client.

The device's OTA check fetches `MaintenanceRadio.cpp`'s `kReleaseManifestUrl` --
`https://github.com/OETSolutions/swc-module/releases/latest/download/version_manifest.json`
-- over VERIFIED TLS with no credential. While the repo was PRIVATE that URL
answered 404 to any anonymous client, so `/api/ota/check` could only ever say
"the release manifest could not be read". With the repo public it must instead
report "an update is available: 1.0.0".

This drives the REAL URL: the DUT is built at version 0.9.0 with NO
`SWC_BENCH_MANIFEST_URL` override, so the compiled-in constant is exercised
exactly as a shipped device would exercise it.
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bench_ladder import Dut                     # noqa: E402
import bench_prov                                # noqa: E402
import bench_wifi_upload as wu                   # noqa: E402
import bench_ota_release as rel                  # noqa: E402

PORT = "/dev/cu.usbmodem1234561"
DUT_VERSION = "0.9.0"
EXPECT_SUBSTRING = "update is available"
EXPECT_VERSION = "1.0.0"


def build_and_flash(port):
    import tempfile
    workdir = tempfile.mkdtemp(prefix="swc_produrl_")
    dut_dir = os.path.join(workdir, "dut_build")
    # NO extra_flags -> the production kReleaseManifestUrl is compiled in.
    img = rel.build_image(dut_dir, DUT_VERSION, "produrl")
    if img is None:
        return False
    if not rel.flash_usb(port, img):
        return False
    time.sleep(3)
    return True


def main():
    if "--skip-dut-flash" not in sys.argv:
        print("== building + flashing the DUT at v%s with the REAL URL baked in"
              % DUT_VERSION)
        if not build_and_flash(PORT):
            print("ERROR: build/flash failed", file=sys.stderr)
            return 1
    else:
        print("== reusing the already-flashed DUT")

    ssid = wu.env_value("WIFI_SSID")
    passphrase = wu.env_value("WIFI_PASSWORD")
    if not ssid or passphrase is None:
        print("ERROR: WIFI_SSID/WIFI_PASSWORD not in code/.env", file=sys.stderr)
        return 2
    esp_prov = bench_prov.find_esp_prov()
    if esp_prov is None:
        print("ERROR: esp_prov.py not found", file=sys.stderr)
        return 2

    dut = Dut(PORT)
    try:
        print("DUT running version:", rel.running_version(dut))
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
            print(bench_prov.strip_ansi(out)[-500:], file=sys.stderr)
            return 1
        ip = None
        deadline = time.time() + 40
        while time.time() < deadline:
            cand = wu.arp_ip_for_mac(wu.station_mac(ble_name)) or wu.DEFAULT_LAN_IP
            st, _ = rel.http_post(cand, "/api/status", token, timeout=6)
            if st is not None:
                ip = cand
                break
            time.sleep(1)
        if not ip:
            print("ERROR: device never answered on the LAN", file=sys.stderr)
            return 1
        print(f"device on the LAN at {ip}")

        print("\n== POST /api/ota/check against the PRODUCTION URL (no override) ==")
        st, body = rel.http_post(ip, "/api/ota/check", token, timeout=60)
        text = body.decode("utf-8", "replace")
        ok = st == 200 and EXPECT_SUBSTRING in text and EXPECT_VERSION in text
        print(f"   -> {st} {text.strip()!r}")
        print("   RESULT:", "PASS -- the anonymous fetch of the real release URL "
              "now resolves" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        dut.close()


if __name__ == "__main__":
    raise SystemExit(main())
