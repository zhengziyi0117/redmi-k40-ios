#!/usr/bin/env python3
"""Run an opt-in Wi-Fi diagnostic using the resident loader and existing RAM system."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--steps', type=int, default=2, choices=range(1, 13))
p.add_argument('--seconds', type=int, default=120)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
assert 30 <= a.seconds <= 1800
a.out.mkdir(parents=True, exist_ok=True)

def fb(*args):
    r = subprocess.run(['fastboot', '-s', 'xnuloader', *map(str, args)],
                       capture_output=True, text=True, timeout=30)
    print(r.stdout + r.stderr, end='', flush=True)
    if r.returncode:
        raise SystemExit(r.returncode)
    return r.stdout + r.stderr

if not re.search(r'product:\s*xnuloader\b', fb('getvar', 'product')):
    raise SystemExit('Not the resident XNU loader')
kernel_source = root / 'build/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837'
kernel = a.out.resolve() / 'kernel.bin'
kernel.write_bytes(kernel_source.read_bytes())
assert kernel.stat().st_size > 10000000
cmdline = (f'loader_return_after={a.seconds} epoch={int(time.time())} sb_delay=8 '
    f'touchscreen=1 buttons=1 cpufreq=1 bootcpu=7 cpus=8 '
    f'adreno_soc=a650 adreno=21 adreno_keep=1 adreno_watch=10 '
    f'aliothwifi={a.steps} aliothwifidelay=8')
if a.steps == 12:
    cmdline += ' wifi=1 wifi_quiet=1'
payload = a.out.resolve() / 'payload.bin'
subprocess.run(['python3', str(root / 'build/k40/xnuloader/mkpayload.py'),
    '-o', str(payload), 'kernel=' + str(kernel),
    'ramdisk=' + str(root / ('runtime/wifi/root.apfs' if a.steps >= 4 else 'runtime/boot/root.apfs')),
    '--cmdline', cmdline], check=True)
(a.out / 'manifest.json').write_text(json.dumps({
    'kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest(),
    'payload_sha256': hashlib.sha256(payload.read_bytes()).hexdigest(),
    'cmdline': cmdline, 'system': 'resident RAM system from the first iOS boot',
    'writes': 'RAM only; loader may append logdump diagnostics'
}, indent=2) + '\n')

fb('oem', 'stop')
for i in range(30):
    status = fb('getvar', 'result')
    if 'result: running' not in status:
        break
    time.sleep(1)
else:
    raise SystemExit('Kernel did not stop; no download attempted')
fb('stage', payload)
fb('oem', 'run')
print('Wi-Fi diagnostic started; use capture_ios.py to inspect its real log.', flush=True)
