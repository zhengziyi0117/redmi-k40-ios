#!/usr/bin/env python3
"""Reconstruct the explicitly selected Apollo driver files from its patch series."""
import argparse
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--through', type=int, default=126)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = root / 'upstream/ios-apollo'
selected = {'sweet_rpmh.c', 'sweet_pcie.c', 'sweet_pcie.h', 'sweet_pcie_phy.h',
            'sweet_mhi.c', 'sweet_mhi.h', 'sweet_wifi.c', 'sweet_qmi.c', 'sweet_wlfw.c',
            'sweet_lx.c', 'sweet_sta.c', 'sweet_net.c', 'sweet_apple80211.c',
            'hakios.qmi.h', 'hakios.qrtr.h', 'hakios.lx.h', 'hakios.say.h',
            'hakios.sta.h', 'hakios.net.h'}
a.out.mkdir(parents=True, exist_ok=True)
for patch in sorted((source / 'xnu/patches').glob('*.patch')):
    if int(patch.name.split('-')[0]) > a.through:
        continue
    parts = re.split(r'(?=^--- (?:a/|/dev/null))', patch.read_bytes().decode('latin1'), flags=re.M)
    for part in parts:
        m = re.search(r'^\+\+\+ b/(\S+)', part, flags=re.M)
        if not m or Path(m[1]).name not in selected:
            continue
        target = a.out / m[1]
        target.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(['patch', '-p1', '--batch', '--fuzz=0'], cwd=a.out,
                                input=part.encode('latin1'), capture_output=True)
        if result.returncode:
            raise SystemExit(f'{patch.name}: {m[1]}\n{result.stdout}\n{result.stderr}')
        print(patch.name, m[1])
(a.out / 'SOURCE.txt').write_text(subprocess.check_output(
    ['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True))
