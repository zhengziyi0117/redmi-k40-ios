#!/usr/bin/env python3
"""Prepare host-only USB boot inputs from the pinned Alioth payload and a local system image.

Never contacts a phone. Refuses to replace an existing output directory.
APFS consistency and file read-back checks must be run separately; see docs/INSTALL.md.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BASE_HASHES = {
    'run-payload.bin': '52f47426571f5c85209891232f8410bde89434be49366fc31869daf1eeee7efb',
    'alioth-boot.img': 'b7a413c2761f1157c06f466b135890773d604754362c7c2d38f80e6d07e860f9',
    'uefi-alioth.fd': 'bf3bcab227e910792626f98b750879996410e58389764f19abb53d51fa421a50',
}


def metadata(path):
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'sha256': digest, 'size': path.stat().st_size}


def payload_entries(data):
    if len(data) < 8 or data[:4] != b'XNUP' or struct.unpack_from('<I', data, 4)[0] != len(data) - 8:
        raise ValueError('Invalid XNUP header or declared length')
    offset, entries = 8, {}
    while offset + 24 <= len(data):
        name, size, reserved = struct.unpack_from('<16sII', data, offset)
        offset += 24
        name = name.split(b'\0', 1)[0].decode('ascii')
        if not name:
            if size or reserved or offset != len(data):
                raise ValueError('Invalid XNUP terminator')
            return entries
        end = offset + size
        padded_end = end + (-size % 16)
        if reserved or name in entries or padded_end > len(data):
            raise ValueError('Duplicate or truncated XNUP entry')
        entries[name] = data[offset:end]
        offset = padded_end
    raise ValueError('Missing XNUP terminator')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system', type=Path, default=ROOT / 'build/k40/userland/system.apfs')
    parser.add_argument('--out', type=Path, default=ROOT / 'runtime/boot')
    args = parser.parse_args()
    output = args.out.resolve()
    if output.exists():
        parser.error(f'{output} already exists; preserve existing inputs and use a new --out directory')
    base = ROOT / 'upstream/ios-alioth'
    expected = json.loads((ROOT / 'sources.json').read_text())['ios-alioth']
    revision = subprocess.check_output(['git', '-C', str(base), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != expected:
        raise ValueError('Alioth source revision differs from sources.json')
    for name, digest in BASE_HASHES.items():
        if metadata(base / 'boot' / name)['sha256'] != digest:
            raise ValueError(f'Pinned upstream input differs: {name}')
    entries = payload_entries((base / 'boot/run-payload.bin').read_bytes())
    if not all(entries.get(name) for name in ('kernel', 'ramdisk', 'cmdline')):
        raise ValueError('Baseline payload lacks kernel, RAM root or command line')
    with args.system.open('rb') as stream:
        header = stream.read(64)
    if header[32:36] != b'NXSB' or args.system.stat().st_size < 1_000_000_000:
        raise ValueError('A full iOS system APFS image is required')
    block_size, blocks = struct.unpack_from('<IQ', header, 36)
    if block_size != 4096 or blocks * block_size != args.system.stat().st_size:
        raise ValueError('APFS container geometry differs from file size')
    words = entries['cmdline'].decode().rstrip('\0').split()
    if any(word.startswith(('ufs_write=', 'root_apfs=', 'pcie=')) for word in words):
        raise ValueError('Baseline payload requests storage writes or hardware probes')
    words = [word for word in words if not word.startswith(('epoch=', 'loader_return_after='))]
    cmdline = ' '.join(['loader_return_after=1800', f'epoch={int(time.time())}', *words])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.boot-inputs-', dir=output.parent) as temp:
        stage = Path(temp) / 'boot'
        stage.mkdir()
        (stage / 'kernel-payload.bin').write_bytes(entries['kernel'])
        (stage / 'root.apfs').write_bytes(entries['ramdisk'])
        shutil.copyfile(base / 'boot/alioth-boot.img', stage / 'alioth-boot.img')
        shutil.copyfile(base / 'boot/uefi-alioth.fd', stage / 'uefi-alioth.fd')
        shutil.copyfile(args.system, stage / 'system.apfs')
        body = b''
        for name, contents in [('kernel', entries['kernel']), ('ramdisk', entries['ramdisk']),
                               ('cmdline', cmdline.encode() + b'\0')]:
            body += struct.pack('<16sII', name.encode(), len(contents), 0)
            body += contents + bytes(-len(contents) % 16)
        body += bytes(24)
        (stage / 'try-payload.bin').write_bytes(b'XNUP' + struct.pack('<I', len(body)) + body)
        payload_entries((stage / 'try-payload.bin').read_bytes())
        manifest = {name: metadata(stage / name) for name in (
            'alioth-boot.img', 'uefi-alioth.fd', 'kernel-payload.bin', 'root.apfs', 'try-payload.bin', 'system.apfs')}
        manifest.update(source_revision=revision, try_cmdline=cmdline,
                        system_validation='Header/geometry only; run tools/apfs/check-image.sh separately',
                        prepared_at_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        stage.rename(output)
    print(f'Prepared {output}; no phone commands run. Validate APFS before USB boot.')


if __name__ == '__main__':
    main()
