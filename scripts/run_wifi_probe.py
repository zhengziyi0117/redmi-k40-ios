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
# 0: a week, for using the phone; the run then ends with five fingers held on the screen,
# power off or restart in iOS, or a forced restart (power held ~10 s).
p.add_argument('--seconds', type=int, default=120)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--display-flip', type=int, choices=(0, 1), default=0,
               help='enable the experimental Alioth hardware page flip (default: off)')
p.add_argument('--display-probe', action='store_true',
               help='pause between display init steps to capture the failing register access')
p.add_argument('--gpu-scale', action='store_true',
               help='offer padded scanout IOSurfaces to the matching GPU scaling bridge')
p.add_argument('--audio-step', type=int, choices=range(1, 12),
               help='bring up the K40 audio DSP through this diagnostic step')
p.add_argument('--sound-level', type=int, choices=range(1, 6),
               help='K40 audio: DSP, TDM, test stream, guarded tone, or iOS playback')
p.add_argument('--bt-step', type=int, choices=range(1, 7),
               help='K40 QCA6390 Bluetooth: read, clocks, loopback, ROM, scan, or iOS H4')
p.add_argument('--bt-console-log', action='store_true',
               help='mirror bluetoothd power/transport logs through the UUID-guarded hook')
p.add_argument('--refresh-hz', type=int, choices=(60, 90, 120), default=120)
p.add_argument('--prefs-store', action='store_true',
               help='use the installed loader to restore/save settings in boot_b[160:192 MiB]')
p.add_argument('--app-data', action='store_true',
               help='use cust-tail apps and the 512 MiB double-buffered data store (new USB loader)')
p.add_argument('--bili-software-decode',action='store_true',help='test the pinned native Bilibili client with software IJK decoding')
p.add_argument('--bili-sample-canvas',action='store_true',help='diagnostic: present native NV12 sample buffers through a CPU-backed CALayer')
p.add_argument('--bili-overlay-canvas',action='store_true',help='diagnostic: present the pinned IJK renderer overlay on a CPU-backed CALayer')
a = p.parse_args()
assert a.seconds == 0 or 30 <= a.seconds <= 1800
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
# A week stands for no limit. The loader would feed the hypervisor's watchdog only for as long
# as a run says it takes, but on this phone that watchdog is not there to arm (its calls return
# "not supported"), so the limit only says when the run ends.
seconds = a.seconds or 7 * 24 * 3600
cmdline = (f'loader_return_after={seconds} epoch={int(time.time())} sb_delay=8 '
    f'touchscreen=1 buttons=1 cpufreq=1 bootcpu=7 cpus=8 '
    f'adreno_soc=a650 adreno=21 adreno_keep=1 adreno_watch=10 '
    f'aliothwifi={a.steps} aliothwifidelay=8 aliothflip={a.display_flip}')
if a.steps == 12:
    cmdline += ' wifi=1 wifi_quiet=1'
if a.display_flip:
    # Hardware latching replaces the software beat. Keep one queued swap and sample touch at 2 ms.
    cmdline += f' fb_beat=0 fb_queue=1 touch_poll_us=2000 fb_hz={a.refresh_hz}'
if a.display_probe:
    cmdline += ' aliothflip_probe=1'
if a.gpu_scale:
    if not a.display_flip:
        raise SystemExit('--gpu-scale requires --display-flip 1')
    cmdline += ' aliothscan=1'
if a.audio_step:
    cmdline += f' alioth_adsp_step={a.audio_step} alioth_adsp_pause=0 alioth_adsp_keep=1'
if a.sound_level:
    cmdline += f' alioth_audio={a.sound_level} alioth_sound_gain=0 alioth_sound_below=288'
if a.bt_step:
    cmdline += f' aliothbt={a.bt_step} aliothbtdelay=45'
if a.bt_console_log:
    if a.bt_step != 6:
        raise SystemExit('--bt-console-log requires --bt-step 6')
    cmdline += ' aliothbtlog=1'
if a.app_data:
    cmdline += ' ufs=2 ufs_write=3 var_store=1 alioth_apps=1 var_save_ms=60000'
if a.bili_software_decode:
    cmdline += ' alioth_bili_sw=1 alioth_bili_log=1'
if a.bili_sample_canvas:
    if not a.bili_software_decode: raise SystemExit('--bili-sample-canvas requires --bili-software-decode')
    cmdline += ' alioth_bili_sample=1'
if a.bili_overlay_canvas:
    if not a.bili_software_decode: raise SystemExit('--bili-overlay-canvas requires --bili-software-decode')
    cmdline += ' alioth_bili_canvas=1'
if a.prefs_store and not a.app_data:
    cmdline += ' ufs=2 ufs_write=2 var_store=1'
payload = a.out.resolve() / 'payload.bin'
subprocess.run(['python3', str(root / 'build/k40/xnuloader/mkpayload.py'),
    '-o', str(payload), 'kernel=' + str(kernel),
    'ramdisk=' + str(root / ('runtime/wifi/root.apfs' if a.steps >= 4 else 'runtime/boot/root.apfs')),
    '--cmdline', cmdline], check=True)
(a.out / 'manifest.json').write_text(json.dumps({
    'kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest(),
    'root_sha256': hashlib.sha256((root / ('runtime/wifi/root.apfs' if a.steps >= 4 else 'runtime/boot/root.apfs')).read_bytes()).hexdigest(),
    'payload_sha256': hashlib.sha256(payload.read_bytes()).hexdigest(),
    'cmdline': cmdline, 'system': 'resident RAM system from the first iOS boot',
    'writes': ('cust[512:1024 MiB] data archive; loader may append logdump diagnostics' if a.app_data else
               'boot_b[160:192 MiB] settings archive; loader may append logdump diagnostics'
               if a.prefs_store else 'RAM only; loader may append logdump diagnostics')
}, indent=2) + '\n')

# A kernel stopped while it brings the GPU up leaves it half started, and the next kernel
# hangs on it ("an earlier kernel left the GPU block running"), as happened when this ran
# right after start_ios.sh: let the running one get as far as its accelerated renderer.
if 'result: running' in fb('getvar', 'result'):
    live = a.out.resolve() / 'before-stop.bin'
    for i in range(90):
        if (subprocess.run(['fastboot', '-s', 'xnuloader', 'oem', 'log'], capture_output=True, timeout=30).returncode == 0
                and subprocess.run(['fastboot', '-s', 'xnuloader', 'get_staged', str(live)],
                                   capture_output=True, timeout=30).returncode == 0
                ):
            log = live.read_bytes()
            if b'== XNULOADER run (store) ==' in log:
                raise SystemExit('Storage boots reset when stopped. Use the USB diagnostic loader for kernel iteration.')
            if b'accelerated renderer' in log or b'adreno: THE GPU STAYS ON for the system' in log:
                break
        time.sleep(1)
    else:
        print('The running kernel never reported its renderer; stopping it anyway', flush=True)
fb('oem', 'stop')
for i in range(90 if a.app_data else 30):
    status = fb('getvar', 'result')
    if 'result: running' not in status:
        break
    time.sleep(1)
else:
    raise SystemExit('Kernel did not stop; no download attempted')
fb('stage', payload)
fb('oem', 'run')
print('Wi-Fi diagnostic started; use capture_ios.py to inspect its real log.', flush=True)
