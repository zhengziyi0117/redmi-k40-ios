#!/usr/bin/env python3
"""Keep iOS on the phone so that it starts without a computer (docs/ON-PHONE-BOOT.md).

  build      lay the images out in runtime/store (the phone is not touched)
  check      what the phone has now: slot, both slots' firmware, the partitions to be used
  partition  create the logical partition "ios" in super's free space (through fastbootd)
  write      write the images and read each back
  uninstall  boot_b back to the backup, exaid back to zeros, "ios" deleted

The root image (system.apfs) goes in three pieces: "ios", "exaid" (unused: all zeros on this
phone) and the end of boot_b. boot_b also holds the loader (its boot image), the manifest the
loader reads (loader.c, struct store_manifest) and the kernel payload.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import zlib

root = Path(__file__).resolve().parents[1]
out = root / 'runtime/store'
MiB = 1 << 20
IOS_BYTES, EXAID_BYTES, BOOT_BYTES = 1896 * MiB, 384 * MiB, 192 * MiB
MANIFEST_AT, PAYLOAD_AT, PIECE_AT = 4 * MiB, 8 * MiB, 60 * MiB
PREFS_AT = 160 * MiB
BOOT_PART = 'boot_b'
SYSTEM = root / 'runtime/boot/system.apfs'
KERNEL = root / 'build/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837'
ROOT_IMAGE = root / 'runtime/wifi/root.apfs'
APP_DATA = False
BILI_COMPAT = False
BACKUP = root / 'backups/k40-before-ios-20261101'
# run_wifi_probe.py --steps 12 --seconds 0, without epoch=: the kernel then takes the time from
# the firmware's clock (boot-epoch), not from when the image was made
CMDLINE = ('loader_return_after=604800 sb_delay=8 touchscreen=1 buttons=1 cpufreq=1 bootcpu=7 cpus=8 '
           'adreno_soc=a650 adreno=21 adreno_keep=1 adreno_watch=10 aliothwifi=12 aliothwifidelay=8 wifi=1 wifi_quiet=1 '
           'aliothflip=1 aliothscan=1 fb_beat=0 fb_queue=1 touch_poll_us=2000 fb_hz=120 '
           'alioth_audio=5 alioth_sound_gain=0 alioth_sound_below=288 aliothbt=6 aliothbtdelay=45 '
           'ufs=2 ufs_write=2 var_store=1')
FIRMWARE = ('abl aop bluetooth cmnlib cmnlib64 devcfg dsp dtbo featenabler hyp imagefv keymaster mdtp mdtpsecapp '
            'modem multiimgoem multiimgqti qupfw tz uefisecapp vbmeta vbmeta_system vendor_boot vm-linux xbl xbl_config').split()
IMAGES = (('ios', '/dev/block/mapper/ios'), ('exaid', '/dev/block/by-name/exaid'), (BOOT_PART, f'/dev/block/by-name/{BOOT_PART}'))


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw).stdout


def adb(script, timeout=600):
    return run('adb', 'shell', script, timeout=timeout)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(16 * MiB):
            h.update(chunk)
    return h.hexdigest()


def verify_feature_root():
    """Reject a root image missing the components required by the stored boot arguments."""
    sys.path.insert(0, str(root / 'build/k40/tools/apfs'))
    import apfs
    container = apfs.Container(str(ROOT_IMAGE))
    try:
        volume = apfs.pick_volume(container, None)
        required = ['fw-audio/adsp.mdt', 'fw-bt/htbtfw20.tlv', 'fw-bt/htnv20.bin']
        required += [f'LaunchDaemons/com.apple.{name}.plist' for name in
                     ('mediaserverd', 'mediaremoted', 'nanoregistryd', 'bluetoothd', 'BlueTool')]
        if APP_DATA:
            required += [f'LaunchDaemons/com.apple.{name}.plist' for name in ('assetsd','FileProvider','mobile.installd','fontservicesd','mediaparserd','TextInput.kbd')]
            required += ['bin/var_store_prefs','LaunchDaemons/com.redwood.installer-setup.plist']
        if BILI_COMPAT:
            required += ['lib/libalioth-bili.dylib']
        for path in required:
            if not volume.lookup(path):
                sys.exit(f'Root lacks {path}; rebuild with --display-bridge PATH --audio --bluetooth')
        binaries = {'lib/libredwood-gl.dylib': 'libredwood-gl.dylib',
                    'lib/libalioth-bt.dylib': 'libalioth-bt.dylib',
                    'lib/RedwoodAudio.driver/RedwoodAudio': 'RedwoodAudio',
                    'bin/var_store': 'var_store_data' if APP_DATA else 'var_store_alioth'}
        if APP_DATA:
            binaries.update({'sbin/init':'init_apps','bin/installer_setup':'installer_setup',
                             'lib/libalioth-installer.dylib':'libalioth-installer.dylib'})
            binaries['lib/libalioth-app-trust.dylib']='libalioth-app-trust.dylib'
        if BILI_COMPAT:
            binaries['lib/libalioth-bili.dylib']='libalioth-bili.dylib'
        for path, name in binaries.items():
            entry = volume.lookup(path)
            expected = root / 'build/k40/userland/obj' / name
            if not entry or not expected.is_file() or volume.inode(entry[0]).contents() != expected.read_bytes():
                sys.exit(f'Root has no matching {name}; rebuild its component and the feature root')
    finally:
        container.close()


def clock_offset():
    """Seconds from the PMIC's clock (rtc0, which Android may not set) to the time, as Android
    keeps it: the loader adds them to the firmware's reading of that clock. 0 without a phone."""
    r = subprocess.run(['adb', 'shell', 'echo $(date +%s) $(cat /sys/class/rtc/rtc0/since_epoch)'],
                       capture_output=True, text=True, timeout=30)
    if r.returncode != 0 or len(r.stdout.split()) != 2:
        print('no phone in Android: the clock offset is left at 0 (iOS then starts in 1970)')
        return 0
    now, rtc = map(int, r.stdout.split())
    print(f'clock: Android {now}, rtc0 {rtc}: offset {now - rtc}')
    return now - rtc


def build():
    verify_feature_root()
    out.mkdir(parents=True, exist_ok=True)
    total = SYSTEM.stat().st_size
    rest = total - IOS_BYTES - EXAID_BYTES
    assert 0 < rest <= PREFS_AT - PIECE_AT, f'system.apfs is {total} bytes: it overlaps the settings store'
    # Rebuilding an image must keep the newest credentials/settings, including both archive slots.
    if adb('getprop ro.product.device').strip() != 'alioth':
        sys.exit('Build requires this alioth in Android to preserve its settings and read the RTC offset')
    app_data = None
    if APP_DATA:
        app_data = json.loads((root / 'runtime/app-test/manifest.json').read_text())
        marker = subprocess.run(['adb','exec-out','dd if=/dev/block/by-name/cust bs=4096 skip=2048 count=1 2>/dev/null'],
                                capture_output=True,check=True,timeout=30).stdout
        if (len(marker)!=4096 or marker[:8]!=b'K40DATA1' or
            zlib.crc32(marker[:56])!=struct.unpack_from('<I',marker,56)[0] or
            struct.unpack_from('<II5Q',marker,8)!=(1,64,1024*MiB,16*MiB,496*MiB,512*MiB,512*MiB)):
            sys.exit('Missing or invalid bounded application/data store; nothing built')
        got=adb(f"dd if=/dev/block/by-name/cust bs=4096 skip=4096 count={app_data['apps_bytes']//4096} 2>/dev/null | sha256sum").split()[0]
        if got!=app_data['apps_sha256']:
            sys.exit('Application image differs from the checked build; nothing built')
        app_data['immutable_cust_prefix_bytes']=512*MiB
        app_data['immutable_cust_prefix_sha256']=phone_sha('/dev/block/by-name/cust',512*MiB)
    saved = subprocess.run(['adb', 'exec-out', f'dd if=/dev/block/by-name/boot_b bs={MiB} skip=160 count=32 2>/dev/null'],
                           capture_output=True, check=True, timeout=60).stdout
    assert len(saved) == BOOT_BYTES - PREFS_AT, 'Could not preserve all 32 MiB of settings; nothing built'
    (out / 'prefs-before-build.bin').write_bytes(saved)
    payload = out / 'payload.bin'
    run(sys.executable, str(root / 'build/k40/xnuloader/mkpayload.py'), '-o', str(payload),
        f'kernel={KERNEL}', f'ramdisk={ROOT_IMAGE}', '--cmdline', CMDLINE)
    loader = out / 'loader-boot.img'
    run(sys.executable, str(root / 'scripts/mk_loader_boot.py'), '--loader', str(root / 'build/k40/xnuloader/loader.efi'),
        '--store', f'{BOOT_PART} {MANIFEST_AT:x} usb', '-o', str(loader))
    run(sys.executable, str(root / 'scripts/mk_loader_boot.py'), '--loader', str(root / 'build/k40/xnuloader/loader.efi'),
        '--usb', '-o', str(out / 'loader-usb.img'))
    p, l = payload.read_bytes(), loader.read_bytes()
    assert len(p) <= PIECE_AT - PAYLOAD_AT and len(l) <= MANIFEST_AT

    pieces = (('ios', 0, IOS_BYTES), ('exaid', 0, EXAID_BYTES), (BOOT_PART, PIECE_AT, rest))
    crcs, image_sha = [], hashlib.sha256()
    with open(SYSTEM, 'rb') as src:
        for name, _, size in pieces:
            crc, left = 0, size
            dest = open(out / f'{name}.img', 'wb') if name != BOOT_PART else None
            tail = bytearray()
            while left:
                chunk = src.read(min(left, 16 * MiB))
                crc = zlib.crc32(chunk, crc)
                image_sha.update(chunk)
                left -= len(chunk)
                if dest:
                    dest.write(chunk)
                else:
                    tail += chunk
            if dest:
                dest.close()
            crcs.append(crc)
    boot = bytearray(BOOT_BYTES)
    boot[:len(l)] = l
    boot[PAYLOAD_AT:PAYLOAD_AT + len(p)] = p
    boot[PIECE_AT:PIECE_AT + rest] = tail
    boot[PREFS_AT:] = saved
    clock = clock_offset()
    manifest = struct.pack('<8sIIQII40sQ', b'XNUSTORE', 2, len(pieces), total, len(p), zlib.crc32(p),
                           BOOT_PART.encode(), PAYLOAD_AT)
    for (name, offset, size), crc in zip(pieces, crcs):
        manifest += struct.pack('<40sQQII', name.encode(), offset, size, crc, 0)
    manifest += bytes(64) * (8 - len(pieces))
    manifest += struct.pack('<q', clock)
    manifest += struct.pack('<I', zlib.crc32(manifest))
    assert len(manifest) == 604
    boot[MANIFEST_AT:MANIFEST_AT + len(manifest)] = manifest
    (out / f'{BOOT_PART}.img').write_bytes(boot)
    info = {
        'system_sha256': image_sha.hexdigest(), 'system_bytes': total,
        'kernel_sha256': sha256(KERNEL), 'root_sha256': sha256(ROOT_IMAGE), 'loader_sha256': sha256(loader),
        'cmdline': CMDLINE, 'clock_offset': clock, 'pieces': [dict(partition=n, offset=o, bytes=s, crc32=f'{c:08x}') for (n, o, s), c in zip(pieces, crcs)],
        'images': {n: sha256(out / f'{n}.img') for n, _ in IMAGES},
        'boot_prefix_sha256': hashlib.sha256(boot[:PREFS_AT]).hexdigest(),
        'prefs_offset': PREFS_AT, 'prefs_bytes': BOOT_BYTES - PREFS_AT,
        'app_data': app_data,
        'built_at_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
    }
    (out / 'manifest.json').write_text(json.dumps(info, indent=2) + '\n')
    print(json.dumps(info, indent=2))


def phone_sha(dev, size=None):
    count = f' bs={MiB} count={size // MiB}' if size else ''
    return adb(f'dd if={dev}{count} 2>/dev/null | sha256sum' if size else f'sha256sum {dev}').split()[0]


def check():
    """Returns what write() may overwrite: {name: 'ours'|'stock'|...}."""
    ok = True
    if adb('id -u').strip() != '0' or adb('getprop ro.product.device').strip() != 'alioth':
        sys.exit('not a rooted alioth')
    slot = adb('getprop ro.boot.slot_suffix').strip()
    print('Android slot:', slot)
    ok &= slot == '_a'
    lp = adb('lpdump')
    print('update state:', re.search(r'Update state: (\S+)', lp).group(1))
    ok &= 'Update state: none' in lp
    hashes = adb('cd /dev/block/by-name; for p in ' + ' '.join(FIRMWARE) +
                 '; do echo $p $(sha256sum ${p}_a | cut -c1-64) $(sha256sum ${p}_b | cut -c1-64); done')
    differ = [l.split()[0] for l in hashes.splitlines() if len(set(l.split()[1:])) != 1]
    print('firmware: both slots the same' if not differ else f'firmware DIFFERS between slots: {differ}')
    ok &= not differ
    build_info = json.loads((out / 'manifest.json').read_text()) if (out / 'manifest.json').exists() else {}
    built = build_info.get('images', {})
    stock_boot = json.loads((BACKUP / 'manifest.json').read_text())['partitions'][BOOT_PART]['sha256']
    state = {}
    has_ios = adb('ls /dev/block/mapper/ios 2>/dev/null || true').strip() != ''
    for name, dev in IMAGES:
        if name == 'ios' and not has_ios:
            state[name] = 'missing (run partition)'
            print(f'{name}: {state[name]}')
            continue
        h = phone_sha(dev)
        if h == built.get(name):
            state[name] = 'ours'
        elif name == BOOT_PART and build_info.get('boot_prefix_sha256') == phone_sha(dev, PREFS_AT):
            state[name] = 'ours'
        elif name == BOOT_PART and h == stock_boot:
            state[name] = 'stock (as backed up)'
        elif name == BOOT_PART and adb(f'dd if={dev} bs={MiB} skip={MANIFEST_AT // MiB} count=1 2>/dev/null | head -c 8') == 'XNUSTORE':
            state[name] = 'ours, older'
        elif name == 'exaid' and adb(f'dd if={dev} bs={MiB} 2>/dev/null | tr -d "\\000" | wc -c').strip() == '0':
            state[name] = 'all zeros (unused)'
        elif name == 'ios':
            state[name] = 'unknown contents (ours, older)'
        else:
            state[name] = f'UNKNOWN contents {h[:16]}'
            ok = False
        print(f'{name}: {state[name]}')
    if has_ios:
        size = int(adb('blockdev --getsize64 /dev/block/mapper/ios').strip())
        print('ios size:', size)
        ok &= size == IOS_BYTES
    if not ok:
        sys.exit('check failed: nothing written')
    return state


def wait_for(cmd, pattern, seconds=180):
    for _ in range(seconds // 3):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if re.search(pattern, r.stdout + r.stderr, re.M):
            return
        time.sleep(3)
    sys.exit(f'timed out waiting for {pattern}')


def partition():
    if adb('ls /dev/block/mapper/ios 2>/dev/null || true').strip():
        print('"ios" exists already')
        return
    layout = re.findall(r'super: (\d+) \.\. (\d+):', adb('lpdump'))
    free = 17825792 - max(int(e) for _, e in layout)
    print(f'super: {free * 512} bytes free after the last partition')
    assert free * 512 >= IOS_BYTES + MiB
    run('adb', 'reboot', 'fastboot')
    wait_for(['fastboot', 'getvar', 'is-userspace'], r'is-userspace: yes')
    print(run('fastboot', 'create-logical-partition', 'ios', str(IOS_BYTES)))
    run('fastboot', 'reboot')
    wait_for(['adb', 'shell', 'ls /dev/block/mapper/ios'], r'/dev/block/mapper/ios$', 240)
    print(adb('lpdump | grep -A6 "Name: ios"'))


def fastboot(serial, *args, timeout=900):
    subprocess.run(['fastboot', '-s', serial, *map(str, args)], check=True, timeout=timeout)


def to_bootloader():
    serial = run('adb', 'get-serialno').strip()
    run('adb', 'reboot', 'bootloader')
    wait_for(['fastboot', 'devices'], rf'^{serial}\s+fastboot')
    return serial


def back_to_android(serial):
    wait_for(['adb', 'devices'], rf'^{serial}\s+device', 240)
    wait_for(['adb', 'shell', 'ls /dev/block/mapper/ios'], r'/dev/block/mapper/ios$', 240)


def write():
    # Android's kernel here (baseband_guard) refuses writes to these partitions: exaid and boot_b
    # are flashed by the bootloader, and "ios", a logical partition it cannot flash (and fastbootd
    # cannot open), by the loader itself, booted into RAM: "oem storewrite" in loader.c.
    state = check()
    built = json.loads((out / 'manifest.json').read_text())['images']
    assert 'missing' not in state['ios']
    todo = [name for name, _ in IMAGES if state[name] != 'ours']
    if not todo:
        print('all written already')
        return
    serial = to_bootloader()
    for name in ('exaid', BOOT_PART):
        if name in todo:
            fastboot(serial, 'flash', name, out / f'{name}.img')
    if 'ios' in todo:
        fastboot(serial, 'boot', out / 'loader-usb.img')
        wait_for(['fastboot', 'devices'], r'^xnuloader\s+fastboot')
        fastboot('xnuloader', 'oem', 'ramdisk')
        fastboot('xnuloader', 'stage', out / 'ios.img')
        fastboot('xnuloader', 'oem', 'storewrite', 'ios', '0')
        fastboot('xnuloader', 'reboot')
    else:
        fastboot(serial, 'reboot')
    back_to_android(serial)
    for name, dev in IMAGES:
        got = phone_sha(dev)
        print(f'{name}: {"as built" if got == built[name] else "DIFFERENT: " + got}')
        if got != built[name]:
            sys.exit('stopped')


def uninstall():
    zeros = out / 'zeros.img'
    with open(zeros, 'wb') as f:
        f.truncate(EXAID_BYTES)
    has_ios = adb('ls /dev/block/mapper/ios 2>/dev/null || true').strip() != ''
    serial = to_bootloader()
    fastboot(serial, 'flash', BOOT_PART, BACKUP / f'{BOOT_PART}.img')
    fastboot(serial, 'flash', 'exaid', zeros)
    zeros.unlink()
    if has_ios:
        fastboot(serial, 'reboot', 'fastboot')
        wait_for(['fastboot', 'getvar', 'is-userspace'], r'is-userspace: yes')
        fastboot(serial, 'delete-logical-partition', 'ios')
    fastboot(serial, 'reboot')
    wait_for(['adb', 'devices'], rf'^{serial}\s+device', 240)
    print(BOOT_PART, 'as backed up:', phone_sha(f'/dev/block/by-name/{BOOT_PART}') ==
          json.loads((BACKUP / 'manifest.json').read_text())['partitions'][BOOT_PART]['sha256'])


def main():
    global APP_DATA, BILI_COMPAT, CMDLINE
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('step', choices=['build', 'check', 'partition', 'write', 'uninstall'])
    p.add_argument('--app-data',action='store_true',help='build with the already-provisioned cust-tail app/data store; never overwrite it')
    p.add_argument('--bili-software-decode',action='store_true',help='enable the UUID-guarded native Bilibili compatibility adapter (requires --app-data)')
    p.add_argument('--bili-sample-canvas',action='store_true',help='enable the sample-buffer CPU presenter after device validation (requires --bili-software-decode)')
    p.add_argument('--bili-overlay-canvas',action='store_true',help='experimental pinned IJK overlay CPU presenter (requires --bili-software-decode)')
    args=p.parse_args()
    APP_DATA=args.app_data
    BILI_COMPAT=args.bili_software_decode
    if BILI_COMPAT and not APP_DATA:
        p.error('--bili-software-decode requires --app-data')
    if args.bili_sample_canvas and not BILI_COMPAT:
        p.error('--bili-sample-canvas requires --bili-software-decode')
    if args.bili_overlay_canvas and not BILI_COMPAT:
        p.error('--bili-overlay-canvas requires --bili-software-decode')
    if APP_DATA:
        CMDLINE=CMDLINE.replace('ufs_write=2','ufs_write=3')+' alioth_apps=1 var_save_ms=60000'
    if BILI_COMPAT:
        CMDLINE+=' alioth_bili_sw=1'
    if args.bili_sample_canvas:
        CMDLINE+=' alioth_bili_sample=1'
    if args.bili_overlay_canvas:
        CMDLINE+=' alioth_bili_canvas=1'
    {'build': build, 'check': check, 'partition': partition, 'write': write, 'uninstall': uninstall}[args.step]()


if __name__ == '__main__':
    main()
