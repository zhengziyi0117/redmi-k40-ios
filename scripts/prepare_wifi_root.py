#!/usr/bin/env python3
"""Keep the known-good boot programs and add this K40's QCA6390 firmware."""
from pathlib import Path
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'build/k40/tools/apfs'))
import apfs

out = root / 'runtime/wifi'
tree = out / 'root'
tree.mkdir(parents=True, exist_ok=True)
container = apfs.Container(str(root / 'runtime/boot/root.apfs'))
volume = apfs.pick_volume(container, None)
manifest = []
for path, name, fid, kind in apfs.walk(volume, apfs.ROOT_DIR_INO_NUM):
    dest = tree / path.lstrip('/')
    inode = volume.inode(fid)
    target = None
    if kind == 4:
        dest.mkdir(exist_ok=True)
    elif kind == 8:
        dest.write_bytes(inode.contents())
        os.chmod(dest, inode.mode & 0o7777)
    elif kind == 10:
        target = inode.symlink_target_bytes().rstrip(b'\0').decode()
        if not dest.is_symlink(): dest.symlink_to(target)
    else:
        raise SystemExit(f'Unsupported APFS entry: {path} {kind}')
    manifest.append(f'{inode.mode:o}\t{inode.owner}\t{inode.group}\t{path}' +
                    ('\t' + target if target is not None else ''))
container.close()
(out / 'root.manifest').write_text('\n'.join(manifest) + '\n')
firmware = root / 'backups/k40-before-ios-20261101/firmware/wifi/qca6390'
(tree / 'fw').mkdir(exist_ok=True)
records = {}
for source, name in [('amss20.bin', 'amss20.bin'), ('m3.bin', 'm3.bin'),
                     ('regdb.bin', 'regdb.bin'), ('bd_k11a.elf', 'board.bin')]:
    shutil.copyfile(firmware / source, tree / 'fw' / name)
    records[name] = {'source': source, 'sha256': hashlib.sha256((firmware / source).read_bytes()).hexdigest()}
(out / 'firmware.json').write_text(json.dumps(records, indent=2) + '\n')
# The packaged desktop root leaves networking services out. Add only those
# needed for Settings, DHCP, DNS and the Wi-Fi keychain, using stock iOS jobs.
subprocess.run(['python3', str(root / 'build/k40/userland/mkjobs.py'),
    str(root / 'build/k40/ios165/rootfs'), str(tree / 'LaunchDaemons'),
    'com.apple.configd', 'com.apple.mDNSResponder', 'com.apple.securityd',
    'com.apple.mobile.keybagd'], check=True)
probe = {
    'Label': 'com.redwood.wifi-check', 'RunAtLoad': True,
    'ProgramArguments': ['/redwood/bin/net_test', 'waitaddr', 'en0', '600',
                         'resolve', 'example.com', 'tcp', 'example.com', '80'],
    'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console',
}
(tree / 'LaunchDaemons/com.redwood.wifi-check.plist').write_bytes(plistlib.dumps(probe))
subprocess.run(['python3', str(root / 'build/k40/tools/apfs/mkapfs.py'),
    '-z', '--plain', 'fw', '-o', str(out / 'root.apfs'),
    f'redwood={tree},manifest={out / "root.manifest"}'], check=True)
print('Known-good boot programs preserved; device-local firmware added to RAM root only.')
