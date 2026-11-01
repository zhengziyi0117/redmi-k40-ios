#!/usr/bin/env python3
"""Fetch pinned source trees, install the source overlay, assemble build/k40."""
from pathlib import Path
import json
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
pins = json.loads((root / 'sources.json').read_text())
urls = {'ios-alioth': 'https://github.com/MrMoxing/ios-alioth.git',
        'ios-apollo': 'https://github.com/aTechGeekDev/ios-apollo.git'}
for name, url in urls.items():
    dest = root / 'upstream' / name
    if not dest.exists():
        dest.parent.mkdir(exist_ok=True)
        subprocess.run(['git', 'clone', '--filter=blob:none', '--no-checkout', url, str(dest)], check=True)
        subprocess.run(['git', '-C', str(dest), 'checkout', '--detach', pins[name]], check=True)
    actual = subprocess.check_output(['git', '-C', str(dest), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != pins[name]:
        raise SystemExit(f'{name}: expected {pins[name]}, found {actual}; existing checkout left in place')

base = root / 'upstream/ios-alioth'
for src in (root / 'port').rglob('*'):
    if not src.is_file():
        continue
    target = base / src.relative_to(root / 'port')
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, target)
subprocess.run(['bash', str(base / 'ubuntu/sync.sh'), str(root / 'build/k40')], check=True)
wrappers = root / 'build/k40/xnu-deps/host/bin'
for name in ['codesign', 'iig', 'install', 'plutil', 'sw_vers', 'xcrun', 'xnu-clang']:
    p = wrappers / name
    if p.is_file():
        p.chmod(p.stat().st_mode | 0o100)
print('Pinned sources and Alioth overlay assembled. Toolchain and runtime images are separate inputs.')
