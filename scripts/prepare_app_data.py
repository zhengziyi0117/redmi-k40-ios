#!/usr/bin/env python3
"""Prepare this K40's backed-up cust tail; leave the original ext4 prefix intact.

This creates a host image only. It never changes GPT or writes the phone.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import tarfile
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
MIB = 1 << 20
BACKUP_SHA = 'b5d07b1f202635a07c891777aca1f50ed00e443846aea55ba69b065c9e5f7ce1'
IPA_SHA = 'a52531ab3b5f441a54051b18cf59446ded3d06d98c2345c9c9dc4dd54c2f142c'
TROLL_SHA = '144dc741d1b74d582c861e6ed504ddced3745724e216e3de79ab7d37b27d0551'

def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def safe_name(name):
    p = PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts:
        raise ValueError(f'Unsafe archive path: {name}')
    return p

def main():
    out = ROOT / 'runtime/app-test'
    apps = out / 'image/Applications'
    apps.mkdir(parents=True, exist_ok=True)
    backup = ROOT / 'backups/20261106/before-app-data/cust.img'
    ipa = ROOT / 'runtime/research/20261106-bilibili-app/BiliBiliMApp_8.89.0.v3.1.0.ipa'
    troll = ROOT / 'runtime/research/20261106-app-installer/TrollStore.tar'
    for path, expected in [(backup, BACKUP_SHA), (ipa, IPA_SHA), (troll, TROLL_SHA)]:
        if digest(path) != expected:
            raise ValueError(f'Unexpected SHA-256: {path}')
    with backup.open('rb') as f:
        sb = f.read(4096)
    blocks = struct.unpack_from('<I', sb, 1028)[0]
    log_block = struct.unpack_from('<I', sb, 1048)[0]
    if backup.stat().st_size != 1024*MIB or sb[1080:1082] != b'\x53\xef' or log_block > 6:
        raise ValueError('Unexpected cust geometry or ext4 superblock')
    if not blocks or blocks * (1024 << log_block) > 8*MIB or struct.unpack_from('<I', sb, 1360)[0]:
        raise ValueError('Original ext4 filesystem overlaps the reserved tail')
    with zipfile.ZipFile(ipa) as z:
        for member in z.infolist():
            p = safe_name(member.filename)
            if len(p.parts) < 2 or p.parts[0] != 'Payload':
                continue
            dest = apps.joinpath(*p.parts[1:])
            if member.is_dir():
                dest.mkdir(parents=True, exist_ok=True)
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                data = z.read(member)
                dest.write_bytes(data)
                mode = (member.external_attr >> 16) & 0o777
                dest.chmod(mode or (0o755 if data[:4] in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe') else 0o644))
    with tarfile.open(troll) as t:
        for member in t:
            p = safe_name(member.name)
            if not p.parts or p.parts[0] != 'TrollStore.app' or p.name.startswith('._'):
                continue
            dest = apps.joinpath(*p.parts)
            if member.isdir():
                dest.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                dest.parent.mkdir(parents=True, exist_ok=True)
                with t.extractfile(member) as source, dest.open('wb') as target:
                    shutil.copyfileobj(source, target)
                dest.chmod(member.mode & 0o777)
            else:
                raise ValueError(f'Unexpected tar entry type: {member.name}')
    image = out / 'apps.apfs'
    subprocess.run(['python3', str(ROOT / 'build/k40/tools/apfs/mkapfs.py'), '-z',
                    '-o', str(image), f'Apps={apps.parent}'], check=True)
    if image.stat().st_size > 496*MIB:
        raise ValueError('Application image exceeds its bounded region')
    target = out / 'cust-app-data.img'
    shutil.copyfile(backup, target)
    descriptor = struct.pack('<8sII5Q', b'K40DATA1', 1, 64, 1024*MIB, 16*MIB, 496*MIB, 512*MIB, 512*MIB)
    descriptor += struct.pack('<II', zlib.crc32(descriptor), 0)
    with target.open('r+b') as f:
        f.seek(8*MIB)
        f.write(descriptor.ljust(4096, b'\0'))
        f.seek(16*MIB)
        with image.open('rb') as source:
            shutil.copyfileobj(source, f)
        # Clear only the explicit data store; never the original filesystem.
        f.seek(512*MIB)
        for _ in range(512):
            f.write(bytes(MIB))
    with backup.open('rb') as a, target.open('rb') as b:
        if a.read(8*MIB) != b.read(8*MIB):
            raise ValueError('Original cust prefix changed')
    result = {'cust_sha256': digest(target), 'backup_sha256': BACKUP_SHA,
              'apps_sha256': digest(image), 'apps_bytes': image.stat().st_size,
              'preserved_prefix_bytes': 8*MIB, 'data_offset': 512*MIB,
              'data_bytes': 512*MIB, 'archive_slot_bytes': 256*MIB,
              'gpt_changed': False}
    (out / 'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
