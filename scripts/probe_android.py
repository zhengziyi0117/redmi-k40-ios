#!/usr/bin/env python3
"""Read Android porting prerequisites over ADB; write a report on the host only."""

import argparse
import datetime as dt
import json
from pathlib import Path
import shutil
import subprocess
import sys


PROPERTIES = (
    "ro.product.manufacturer", "ro.product.model", "ro.product.device",
    "ro.product.vendor.device", "ro.product.board", "ro.board.platform",
    "ro.soc.manufacturer", "ro.soc.model", "ro.product.cpu.abilist",
    "ro.build.version.release", "ro.build.version.sdk",
    "ro.build.version.security_patch", "ro.build.fingerprint",
    "ro.miui.ui.version.name", "ro.mi.os.version.name", "ro.boot.hardware",
    "ro.boot.slot_suffix", "ro.boot.flash.locked", "ro.boot.vbmeta.device_state",
    "ro.boot.verifiedbootstate", "ro.boot.veritymode", "ro.boot.dynamic_partitions",
    "ro.boot.boot_devices", "ro.boot.bootdevice", "ro.boot.hwc",
)

# These commands only read metadata. No su, reboot, fastboot, partition reads,
# package installs, settings changes, Wi-Fi queries, or writes on the phone.
PROBES = {
    "properties": "for k in " + " ".join(PROPERTIES)
        + '; do printf "%s=" "$k"; getprop "$k"; done',
    "kernel": "uname -srvm",
    "shell_uid": "id -u",
    "su_binary_present": "command -v su",
    "memory": "head -n 3 /proc/meminfo",
    "cpu_topology": 'for p in /sys/devices/system/cpu/possible '
        '/sys/devices/system/cpu/present /sys/devices/system/cpu/online; '
        'do if [ -r "$p" ]; then printf "%s: " "$p"; cat "$p"; fi; done',
    "device_tree_identity": 'for p in /sys/firmware/devicetree/base/model '
        '/sys/firmware/devicetree/base/compatible; do if [ -r "$p" ]; then '
        'printf "%s: " "$p"; tr "\\000" "\\n" < "$p"; fi; done',
    "display_size": "wm size",
    "display_density": "wm density",
    "input_device_names": "cat /sys/class/input/input*/name",
    "partition_links": "ls -l /dev/block/by-name",
    "block_sizes_sectors": 'for p in /sys/class/block/sd*/size; do '
        'if [ -r "$p" ]; then printf "%s: " "$p"; cat "$p"; fi; done',
    "loaded_module_names": "ls -1 /sys/module",
    "firmware_names": 'for p in /vendor/firmware /vendor/firmware_mnt/image '
        '/vendor/firmware/wlan/qca_cld /vendor/firmware_mnt/image/qca6390 '
        '/vendor/etc/wifi /odm/firmware; do if [ -d "$p" ]; then '
        'printf "directory: %s\\n" "$p"; ls -1 "$p" | '
        'grep -Ei "qca6390|amss20|m3[.]bin|bd_|bdwlan|regdb|a650_|novatek|focaltech|fts|touch"; '
        'fi; done',
}


def run(argv, timeout, serials=()):
    try:
        proc = subprocess.run(argv, capture_output=True, text=True,
                              errors="replace", timeout=timeout, check=False)
        result = {"returncode": proc.returncode,
                  "stdout": proc.stdout.strip(), "stderr": proc.stderr.strip()}
    except subprocess.TimeoutExpired:
        result = {"returncode": None, "stdout": "", "stderr": "command timed out"}
    except OSError as exc:
        result = {"returncode": None, "stdout": "", "stderr": str(exc)}
    for field in ("stdout", "stderr"):
        for serial in serials:
            result[field] = result[field].replace(serial, "[device serial omitted]")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="Select a device when several are attached")
    parser.add_argument("--out", type=Path, help="Host JSON report path (must not exist)")
    args = parser.parse_args()
    adb = shutil.which("adb")
    if not adb:
        print("adb is not installed or not on PATH", file=sys.stderr)
        return 2
    devices = run([adb, "devices"], 15)
    if devices["returncode"] != 0:
        print("ADB unavailable; no phone commands were run.\n" + devices["stderr"],
              file=sys.stderr)
        return 2
    entries = []
    for line in devices["stdout"].splitlines():
        columns = line.split()
        if len(columns) >= 2 and not line.startswith(("List of devices", "*")):
            entries.append((columns[0], columns[1]))
    eligible = [serial for serial, state in entries if state == "device"
                and (args.serial is None or serial == args.serial)]
    if len(eligible) != 1:
        states = [state for _, state in entries]
        print(f"Need one authorized Android device; found {len(eligible)} eligible. "
              f"Attached states: {states}. Unlock the screen and accept USB debugging; "
              "use --serial if several devices are authorized.", file=sys.stderr)
        return 2
    serial = eligible[0]
    output = args.out or Path("diagnostics") / (
        "android-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S") + ".json")
    if output.exists():
        print(f"Refusing to replace existing report: {output}", file=sys.stderr)
        return 2
    report = {
        "collected_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "mode": "read-only ADB metadata; no phone writes requested",
        "notes": ["No serial, IMEI, MAC, SSID, accounts or user files requested.",
                  "Permission-denied probes are recorded; root is never requested.",
                  "Presence of su does not prove usable root access.",
                  "Bootloader properties are indicators; fastboot state is not checked.",
                  "Block sizes are Linux sysfs sectors of 512 bytes, not UFS LBAs."],
        "probes": {},
    }
    for name, command in PROBES.items():
        print(f"Reading {name}…", flush=True)
        report["probes"][name] = run([adb, "-s", serial, "shell", command], 12,
                                     [s for s, _ in entries])
    props = dict(line.split("=", 1) for line in
                 report["probes"]["properties"]["stdout"].splitlines() if "=" in line)
    codenames = [props.get("ro.product.device", ""),
                 props.get("ro.product.vendor.device", "")]
    report["alioth_property_match"] = "alioth" in codenames
    report["collection_complete"] = all(
        report["probes"][name]["returncode"] == 0
        for name in ("properties", "kernel", "shell_uid"))
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    print(f"Saved {output}; alioth property match: {report['alioth_property_match']}")
    return 0 if report["collection_complete"] else 2


if __name__ == "__main__":
    sys.exit(main())
