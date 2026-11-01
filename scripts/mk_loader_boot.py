#!/usr/bin/env python3
"""Repack the verified alioth boot image around another XNU loader (loader.efi).

The boot image's kernel is gzip(boot shim + alioth UEFI + "XNUP" payload) followed by the device
tree, and the payload holds loader.efi. build/k40 has no Mu-Silicium tree for alioth, and its
prebuilt shim is redwood's (another FD address), so the shim, the UEFI and the device tree are
taken from runtime/boot/alioth-boot.img as they are, and only the payload is new.
"""
import argparse
import gzip
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--loader', type=Path, required=True)
p.add_argument('--stock', type=Path, default=root / 'runtime/boot/alioth-boot.img')
p.add_argument('--usb', action='store_true', help='the loader stays and takes commands over USB')
p.add_argument('--store', help='"PARTITION OFFSET [usb]": start iOS from what install_ios_store.py wrote there')
p.add_argument('-o', '--output', type=Path, required=True)
a = p.parse_args()

img = a.stock.read_bytes()
assert img[:8] == b'ANDROID!'
kernel_size, page = struct.unpack_from('<I', img, 8)[0], struct.unpack_from('<I', img, 36)[0]
assert struct.unpack_from('<I', img, 40)[0] == 1, 'expected a version 1 boot image header'
z = zlib.decompressobj(31)
blob = z.decompress(img[page:page + kernel_size])
dtb = z.unused_data
fd = (root / 'runtime/boot/uefi-alioth.fd').read_bytes()
at = blob.find(b'XNUP')
assert dtb[:4] == b'\xd0\x0d\xfe\xed' and at == 176 + len(fd) and blob[176:at] == fd, 'unexpected stock image layout'

body = b''
def add(name, data):
    global body
    body += struct.pack('<16sII', name.encode(), len(data), 0) + data + b'\0' * (-len(data) % 16)
add('loader.efi', a.loader.read_bytes())
if a.usb:
    add('usb', b'1\0')
if a.store:
    add('store', a.store.encode() + b'\0')
body += b'\0' * 24
payload = struct.pack('<II', 0x50554E58, len(body)) + body
new = blob[:at] + payload
new += b'\0' * (-len(new) % 16)

with tempfile.TemporaryDirectory() as tmp:
    kernel, ramdisk = Path(tmp) / 'kernel', Path(tmp) / 'ramdisk'
    kernel.write_bytes(gzip.compress(new, 6) + dtb)
    ramdisk.write_bytes(b'dummy')
    subprocess.run([sys.executable, str(root / 'build/k40/prebuilt/uefi/mkbootimg.py'), '--kernel', str(kernel),
                    '--ramdisk', str(ramdisk), '--header_version', '1', '-o', str(a.output)], check=True)
print(f'{a.output}: {a.output.stat().st_size} bytes, payload {len(payload)} bytes')
