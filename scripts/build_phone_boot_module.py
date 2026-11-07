#!/usr/bin/env python3
"""Package a device-local KernelSU action for the verified on-phone iOS install."""
import hashlib
import json
from pathlib import Path
import zipfile


def main():
    root = Path(__file__).resolve().parents[1]
    store = root / 'runtime/store'
    manifest = json.loads((store / 'manifest.json').read_text())
    backup = root / 'backups/k40-before-ios-20261101'
    stock = json.loads((backup / 'manifest.json').read_text())
    checks = []
    boot_prefix = None
    for name, device in [('ios', '/dev/block/mapper/ios'),
                         ('exaid', '/dev/block/by-name/exaid'),
                         ('boot_b', '/dev/block/by-name/boot_b')]:
        with (store / f'{name}.img').open('rb') as src:
            digest = hashlib.file_digest(src, 'sha256').hexdigest()
        if digest != manifest['images'][name]:
            raise SystemExit(f'{name}.img differs from the store manifest')
        if name == 'boot_b' and manifest.get('boot_prefix_sha256'):
            boot_prefix = manifest['boot_prefix_sha256']
        else:
            checks.append(f'{digest}  {device}\n')
    checks.append(f"{stock['partitions']['boot_a']['sha256']}  /dev/block/by-name/boot_a\n")
    target = store / 'k40-ios-boot.zip'
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
        for source in sorted((root / 'scripts/phone_boot_module').iterdir()):
            archive.write(source, source.name)
        archive.writestr('expected.sha256', ''.join(checks))
        if boot_prefix:
            archive.writestr('boot-prefix.sha256', boot_prefix + '\n')
        if manifest.get('app_data'):
            data=manifest['app_data']
            archive.writestr('cust-prefix.sha256', str(data['immutable_cust_prefix_bytes']//(1<<20))+' '+data['immutable_cust_prefix_sha256']+'\n')
        archive.writestr('skip_mount', '')
    print(target)


if __name__ == '__main__':
    main()
