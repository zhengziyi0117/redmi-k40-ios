#!/usr/bin/env python3
"""Adapt the pinned Apollo QCA6390 Linux shim to this tree's Redwood API names."""
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[1]
source = root / 'upstream/ios-apollo/wifi/lx'
destinations = [root / 'build/k40/wifi/lx',
    root / 'upstream/ios-alioth/src/hackios-xnu-kernel-main/wifi/lx']
for dest in destinations:
    for file in source.rglob('*'):
        if not file.is_file(): continue
        target = dest / file.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        data = file.read_text().replace('hakios.', 'redwood_').replace('hakios_', 'redwood_').replace('HAKIOS_', 'REDWOOD_')
        target.write_text(data)
    shutil.copyfile(source.parent / 'LICENSE.HackiOS', dest.parent / 'LICENSE.HackiOS')
print('QCA6390 compatibility layer staged; Linux and FreeBSD sources remain pinned by mk-ath11k.sh.')
