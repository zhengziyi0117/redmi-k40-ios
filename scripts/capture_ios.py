#!/usr/bin/env python3
"""Capture the resident XNU loader's framebuffer and logs over fastboot."""

import argparse
from pathlib import Path
import re
import struct
import subprocess
import zlib


def fastboot(serial, *args, timeout=30):
    proc = subprocess.run(["fastboot", "-s", serial, *map(str, args)],
                          capture_output=True, timeout=timeout)
    if proc.returncode:
        raise RuntimeError((proc.stdout + proc.stderr).decode(errors="replace"))
    return (proc.stdout + proc.stderr).decode(errors="replace")


def png_from_framebuffer(raw, width, height, stride):
    if not (0 < width <= 8192 and 0 < height <= 8192 and
            width * 4 <= stride <= 131072 and len(raw) >= height * stride):
        raise ValueError("Framebuffer dimensions or data length are invalid")
    rows = bytearray()
    for y in range(height):
        line = raw[y * stride:y * stride + width * 4]
        rgb = bytearray(width * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
        rows.append(0)
        rows.extend(rgb)

    def chunk(kind, body):
        data = kind + body
        return (struct.pack(">I", len(body)) + data +
                struct.pack(">I", zlib.crc32(data) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default="xnuloader")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    product = fastboot(args.serial, "getvar", "product")
    if not re.search(r"product:\s*xnuloader\b", product):
        raise SystemExit("Selected device is not the XNU loader")
    args.out.mkdir(parents=True, exist_ok=True)
    # On return the loader stages the full kernel log. "oem log" while idle
    # rebuilds only the loader's own log, and a screenshot overwrites staging.
    # Fetch the returned log first; for a live kernel ask for its current log.
    result = fastboot(args.serial, "getvar", "result")
    log_path = (args.out / "loader-log.bin").resolve()
    if "result: running" in result:
        fastboot(args.serial, "oem", "log")
    fastboot(args.serial, "get_staged", log_path)
    if not log_path.read_bytes().startswith(b"== XNULOADER"):
        fastboot(args.serial, "oem", "log")
        fastboot(args.serial, "get_staged", log_path)
    (args.out / "loader-log.txt").write_text(
        log_path.read_bytes().replace(b"\0", b"").decode(errors="replace"))
    geometry = fastboot(args.serial, "getvar", "screen")
    match = re.search(r"screen:\s*(\d+)\s+(\d+)\s+(\d+)", geometry)
    if not match:
        raise SystemExit("Loader returned unknown screen geometry: " + geometry)
    width, height, stride = map(int, match.groups())
    fastboot(args.serial, "oem", "screenshot")
    raw_path = (args.out / "screen.raw").resolve()
    fastboot(args.serial, "get_staged", raw_path)
    raw = raw_path.read_bytes()
    png_path = args.out / "screen.png"
    png_path.write_bytes(png_from_framebuffer(raw, width, height, stride))
    raw_path.unlink()
    print(f"Saved {png_path} ({width} x {height}) and loader-log.txt")


if __name__ == "__main__":
    main()
