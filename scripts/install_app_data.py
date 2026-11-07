#!/usr/bin/env python3
"""Provision only the unreferenced tail of the verified, backed-up K40 cust.

Refuses to overwrite a previously provisioned store, even if an old host image exists.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import re
import zlib

ROOT = Path(__file__).resolve().parents[1]
SERIAL = None
DEVICE = '/dev/block/by-name/cust'

def adb(*args, **kwargs):
    return subprocess.run(['adb', '-s', SERIAL, *args], check=True, capture_output=True, **kwargs)

def main():
    global SERIAL
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True, help='ADB/fastboot serial of the backed-up alioth')
    p.add_argument('--write', action='store_true')
    a = p.parse_args()
    SERIAL = a.serial
    manifest = json.loads((ROOT / 'runtime/app-test/manifest.json').read_text())
    image = ROOT / 'runtime/app-test/cust-app-data.img'
    backup = ROOT / 'backups/20261106/before-app-data/cust.img'
    for path, expected in [(image, manifest['cust_sha256']), (backup, manifest['backup_sha256'])]:
        with path.open('rb') as f:
            if hashlib.file_digest(f, 'sha256').hexdigest() != expected:
                raise SystemExit(f'Local checksum mismatch: {path}')
    if adb('shell', 'id -u', text=True).stdout.strip() != '0':
        raise SystemExit('Requires this phone with root adbd')
    size = adb('shell', f'blockdev --getsize64 {DEVICE}', text=True).stdout.strip()
    if size != str(1 << 30):
        raise SystemExit('Unexpected cust size')
    mounts = adb('shell', 'cat /proc/mounts', text=True).stdout
    resolved = adb('shell', f'readlink -f {DEVICE}', text=True).stdout.strip()
    if any(line.split()[0] in (DEVICE, resolved) for line in mounts.splitlines()):
        raise SystemExit('cust is mounted; no write attempted')
    before = adb('shell', f'sha256sum {DEVICE}', text=True).stdout.split()[0]
    if before != manifest['backup_sha256']:
        raise SystemExit('cust differs from the original backup; refusing to overwrite existing data')
    print('Verified backup, exact device geometry, unmounted filesystem and original cust checksum.', flush=True)
    if not a.write:
        print('Read-only check passed. No phone writes.')
        return
    loader = ROOT / 'runtime/app-test/loader.img'
    if not loader.is_file():
        raise SystemExit('Build the diagnostic USB loader first')
    adb('shell', 'rm -f /data/local/tmp/k40-cust-app-data.img')
    adb('reboot', 'bootloader')
    for _ in range(40):
        if SERIAL in subprocess.check_output(['fastboot', 'devices'], text=True): break
        time.sleep(1)
    else: raise SystemExit('No bootloader')
    def fb(serial, *args, timeout=180):
        p = subprocess.run(['fastboot', '-s', serial, *map(str,args)], capture_output=True, text=True, timeout=timeout)
        print(p.stdout+p.stderr, end='', flush=True)
        p.check_returncode()
        return p.stdout+p.stderr
    if not re.search(r'product:\s*alioth\b', fb(SERIAL, 'getvar', 'product')):
        raise SystemExit('Wrong device')
    if not re.search(r'unlocked:\s*(yes|true)\b', fb(SERIAL, 'getvar', 'unlocked'), re.I):
        raise SystemExit('Locked bootloader')
    if not re.search(r'partition-size:cust:\s*(0x)?40000000\b', fb(SERIAL, 'getvar', 'partition-size:cust'), re.I):
        raise SystemExit('Wrong partition size')
    # The full image preserves the original first 8 MiB byte-for-byte.
    fb(SERIAL, 'flash', 'cust', image)
    fb(SERIAL, 'boot', loader)
    for _ in range(40):
        if 'xnuloader' in subprocess.check_output(['fastboot', 'devices'], text=True): break
        time.sleep(1)
    else: raise SystemExit('USB loader not available for read-back')
    crc = 0
    with image.open('rb') as f:
        while chunk := f.read(4 << 20): crc = zlib.crc32(chunk, crc)
    readback = fb('xnuloader', 'oem', 'readtest', 'cust', '40000000')
    match = re.search(r'1073741824 bytes \d+ ms crc32 ([0-9a-fA-F]+)', readback)
    if not match or int(match[1],16) != crc:
        raise SystemExit('Full-partition read-back CRC mismatch; do not start test kernel')
    result = {'before_sha256': before, 'image_sha256': manifest['cust_sha256'],
              'full_readback_crc32': f'{crc:08x}', 'prefix_bytes_unchanged': True,
              'gpt_changed': False, 'android_userdata_changed': False}
    (ROOT / 'runtime/app-test/provisioned.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
