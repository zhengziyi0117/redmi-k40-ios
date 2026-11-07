#!/usr/bin/env python3
"""Keep the known-good boot programs and add this K40's QCA6390 firmware."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
import struct
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wifid-console-log', action='store_true',
                    help="also write wifid's os_log messages to the console (diagnostic)")
parser.add_argument('--account-console-log', action='store_true',
                    help='also record stock account-service diagnostics (local logs may contain account data)')
parser.add_argument('--display-bridge', type=Path,
                    help='use a locally built GPU scanout bridge (requires aliothscan=1 in the kernel)')
parser.add_argument('--audio', action='store_true',
                    help='include this K40 audio firmware, HAL plug-in and native audio services')
parser.add_argument('--bluetooth', action='store_true',
                    help='include this K40 Bluetooth firmware and iOS 16.5 H4/BlueTool bridge')
parser.add_argument('--audio-test', action='store_true',
                    help='play a quiet three-second AudioQueue tone after boot (requires --audio)')
parser.add_argument('--app-data', action='store_true',
                    help='test native apps and the bounded cust-tail persistent data archive')
parser.add_argument('--open-app', help='diagnostic: open this bundle identifier after boot (requires --app-data)')
parser.add_argument('--ipa-test', action='store_true', help='diagnostic: download/check/install the pinned small Ibili IPA')
parser.add_argument('--photos-ui-test', action='store_true', help='diagnostic: dismiss Photos welcome/notification UI after opening it')
parser.add_argument('--photos-test', action='store_true', help='diagnostic: import a test image into an empty real Photos library')
parser.add_argument('--files-ui-test', action='store_true', help='diagnostic: switch Files to its Browse tab')
parser.add_argument('--render-test', action='store_true', help='diagnostic: test Chinese glyph coverage and native H264 decoding')
parser.add_argument('--bili-test', action='store_true', help='diagnostic: add the guarded Bilibili software-decoder adapter and open Bilibili after 90s')
parser.add_argument('--bili-adapter', action='store_true', help='include the guarded Bilibili adapter without automatic UI actions')
parser.add_argument('--bili-video-ui-test',action='store_true',help='diagnostic: dismiss a notification prompt and open the first Bilibili video')
parser.add_argument('--quota-test',action='store_true',help='diagnostic: check real sparse/tiny-file quota enforcement')
parser.add_argument('--usage-test',action='store_true',help='diagnostic: aggregate persistent RAM file usage')
args = parser.parse_args()
if args.audio_test and not args.audio:
    parser.error('--audio-test requires --audio')
if (args.open_app or args.ipa_test or args.photos_ui_test or args.photos_test or args.files_ui_test or args.render_test or args.bili_test or args.bili_adapter or args.quota_test or args.usage_test) and not args.app_data:
    parser.error('application diagnostics require --app-data')
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'build/k40/tools/apfs'))
import apfs

out = root / 'runtime/wifi'
tree = out / 'root'
if tree.exists():
    shutil.rmtree(tree)
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
if args.display_bridge:
    shutil.copyfile(args.display_bridge, tree / 'lib/libredwood-gl.dylib')
    os.chmod(tree / 'lib/libredwood-gl.dylib', 0o755)
# This target keeps credentials/settings in two checked archive slots at boot_b + 160 MiB.
# Preserve all other validated boot programs from the baseline image.
prefs_tool = root / 'build/k40/userland/obj/var_store_alioth'
if not prefs_tool.is_file():
    raise SystemExit('Build the Alioth settings saver first: bash scripts/build_prefs_store.sh')
shutil.copyfile(prefs_tool, tree / 'bin/var_store')
os.chmod(tree / 'bin/var_store', 0o755)
if args.app_data:
    shutil.copyfile(tree / 'bin/var_store', tree / 'bin/var_store_prefs')
    os.chmod(tree / 'bin/var_store_prefs', 0o755)
    shutil.copyfile(root / 'build/k40/userland/obj/libalioth-installer.dylib', tree / 'lib/libalioth-installer.dylib')
    os.chmod(tree / 'lib/libalioth-installer.dylib', 0o755)
    with (tree / 'etc/inject.conf').open('a') as inject:
        inject.write('/redwood/lib/libalioth-installer.dylib trollstorehelper\n')
    shutil.copyfile(root / 'build/k40/userland/obj/libalioth-app-trust.dylib', tree / 'lib/libalioth-app-trust.dylib')
    os.chmod(tree / 'lib/libalioth-app-trust.dylib', 0o755)
    with (tree / 'etc/inject.conf').open('a') as inject:
        inject.write('/redwood/lib/libalioth-app-trust.dylib SpringBoard\n')
    if args.bili_test or args.bili_adapter:
        shutil.copyfile(root/'build/k40/userland/obj/libalioth-bili.dylib',tree/'lib/libalioth-bili.dylib')
        os.chmod(tree/'lib/libalioth-bili.dylib',0o755)
        with (tree/'etc/inject.conf').open('a') as inject:
            inject.write('/redwood/lib/libalioth-bili.dylib bili-universal\n')
    if args.bili_test:
        shutil.copyfile(root/'build/k40/userland/obj/app_probe',tree/'bin/app_probe')
        os.chmod(tree/'bin/app_probe',0o755)
        job={'Label':'com.redwood.bili-probe','RunAtLoad':True,'UserName':'mobile',
             'ProgramArguments':['/redwood/bin/app_probe','90','tv.danmaku.bilianime'],
             'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}
        (tree/'LaunchDaemons/com.redwood.bili-probe.plist').write_bytes(plistlib.dumps(job))
        if args.bili_video_ui_test:
            job={'Label':'com.redwood.bili-video-ui','RunAtLoad':True,
                 'ProgramArguments':['/redwood/bin/rwtouch','wait','122','tap','0.32','0.59'],
                 'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}
            (tree/'LaunchDaemons/com.redwood.bili-video-ui.plist').write_bytes(plistlib.dumps(job))
    for source, target in [('var_store_data', 'bin/var_store'), ('init_apps', 'sbin/init')]:
        shutil.copyfile(root / 'build/k40/userland/obj' / source, tree / target)
        os.chmod(tree / target, 0o755)
    for name in ('TrollStore.tar', 'ldid-arm64'):
        shutil.copyfile(root / 'runtime/research/20261106-app-installer' / name, tree / 'share' / name)
    os.chmod(tree / 'share/ldid-arm64', 0o755)
    # Bootstrap the official installer into its normal app container, once.
    # Its helper handles subsequent app registrations from the persistent archive.
    shutil.copyfile(root / 'build/k40/userland/obj/installer_setup', tree / 'bin/installer_setup')
    os.chmod(tree / 'bin/installer_setup', 0o755)
    job = {'Label': 'com.redwood.installer-setup', 'RunAtLoad': True,
           'ProgramArguments': ['/redwood/bin/installer_setup'],
           'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console'}
    (tree / 'LaunchDaemons/com.redwood.installer-setup.plist').write_bytes(plistlib.dumps(job))
    shutil.copyfile(root / 'build/k40/userland/obj/photos_probe', tree / 'bin/photos_probe')
    os.chmod(tree / 'bin/photos_probe', 0o755)
    sample = out / 'K40-storage-test.png'
    if not sample.exists():
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind+data))
        rows = bytearray()
        for y in range(320):
            rows.append(0)
            for x in range(512):
                border = 24 <= x < 488 and 24 <= y < 296 and (x < 30 or x >= 482 or y < 30 or y >= 290)
                rows.extend((93, 224, 204) if border else (20+x//8, 52+y//4, 75+x//4))
        sample.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',512,320,8,2,0,0,0)) +
                           chunk(b'tEXt', b'Title\0K40 iOS storage test image') +
                           chunk(b'IDAT', zlib.compress(bytes(rows),9)) + chunk(b'IEND',b''))
    shutil.copyfile(sample, tree / 'share/K40-storage-test.png')
    job = {'Label': 'com.redwood.photos-probe', 'RunAtLoad': True, 'UserName': 'mobile',
           'ProgramArguments': ['/redwood/bin/photos_probe', '/redwood/share/K40-storage-test.png'],
           'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console'}
    (tree / 'LaunchDaemons/com.redwood.photos-probe.plist').write_bytes(plistlib.dumps(job))
    if not args.photos_test:
        (tree / 'LaunchDaemons/com.redwood.photos-probe.plist').unlink()
        (tree / 'bin/photos_probe').unlink()
        (tree / 'share/K40-storage-test.png').unlink()
    if args.ipa_test:
        shutil.copyfile(root / 'build/k40/userland/obj/ipa_install_probe', tree / 'bin/ipa_install_probe')
        os.chmod(tree / 'bin/ipa_install_probe', 0o755)
        job = {'Label': 'com.redwood.ipa-install-probe', 'RunAtLoad': True,
               'ProgramArguments': ['/redwood/bin/ipa_install_probe'],
               'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console'}
        (tree / 'LaunchDaemons/com.redwood.ipa-install-probe.plist').write_bytes(plistlib.dumps(job))
    if args.photos_ui_test:
        job = {'Label': 'com.redwood.photos-ui-probe', 'RunAtLoad': True,
               'ProgramArguments': ['/redwood/bin/rwtouch', 'wait', '72', 'tap', '0.32', '0.59',
                                    'wait', '3', 'tap', '0.5', '0.895'],
               'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console'}
        (tree / 'LaunchDaemons/com.redwood.photos-ui-probe.plist').write_bytes(plistlib.dumps(job))
    if args.files_ui_test:
        job = {'Label': 'com.redwood.files-ui-probe', 'RunAtLoad': True,
               'ProgramArguments': ['/redwood/bin/rwtouch', 'wait', '75', 'tap', '0.83', '0.93'],
               'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console'}
        (tree / 'LaunchDaemons/com.redwood.files-ui-probe.plist').write_bytes(plistlib.dumps(job))
    if args.render_test:
        shutil.copyfile(root / 'build/k40/userland/obj/native_render_probe', tree / 'bin/native_render_probe')
        os.chmod(tree / 'bin/native_render_probe', 0o755)
        shutil.copyfile(root / 'runtime/media-input/test-h264.mp4', tree / 'share/test-h264.mp4')
        job = {'Label':'com.redwood.native-render-probe','RunAtLoad':True,'UserName':'mobile',
               'ProgramArguments':['/redwood/bin/native_render_probe'],
               'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}
        (tree / 'LaunchDaemons/com.redwood.native-render-probe.plist').write_bytes(plistlib.dumps(job))
    if args.quota_test:
        shutil.copyfile(root/'build/k40/userland/obj/storage_quota_probe',tree/'bin/storage_quota_probe')
        os.chmod(tree/'bin/storage_quota_probe',0o755)
        job={'Label':'com.redwood.storage-quota-probe','RunAtLoad':True,
             'ProgramArguments':['/redwood/bin/storage_quota_probe'],
             'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}
        (tree/'LaunchDaemons/com.redwood.storage-quota-probe.plist').write_bytes(plistlib.dumps(job))
    if args.usage_test:
        shutil.copyfile(root/'build/k40/userland/obj/storage_usage_probe',tree/'bin/storage_usage_probe')
        os.chmod(tree/'bin/storage_usage_probe',0o755)
        job={'Label':'com.redwood.storage-usage-probe','RunAtLoad':True,
             'ProgramArguments':['/redwood/bin/storage_usage_probe'],
             'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}
        (tree/'LaunchDaemons/com.redwood.storage-usage-probe.plist').write_bytes(plistlib.dumps(job))
    subprocess.run(['python3',str(root/'build/k40/userland/mkjobs.py'),
                    str(root/'build/k40/ios165/rootfs'),str(tree/'LaunchDaemons'),'com.apple.fontservicesd'],check=True)
    subprocess.run(['python3', str(root / 'build/k40/userland/mkjobs.py'),
                    str(root / 'build/k40/ios165/rootfs'), str(tree / 'LaunchDaemons'),
                    'com.apple.assetsd', 'com.apple.FileProvider', 'com.apple.mobile.installd',
                    'com.apple.corespotlightservice','com.apple.searchd','com.apple.spotlight.IndexAgent',
                    'com.apple.mediaparserd','com.apple.TextInput.kbd'], check=True)
    if args.open_app:
        shutil.copyfile(root / 'build/k40/userland/obj/app_probe', tree / 'bin/app_probe')
        os.chmod(tree / 'bin/app_probe', 0o755)
        job = {'Label': 'com.redwood.app-probe', 'RunAtLoad': True, 'UserName': 'mobile',
               'ProgramArguments': ['/redwood/bin/app_probe', '60', args.open_app],
               'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console'}
        (tree / 'LaunchDaemons/com.redwood.app-probe.plist').write_bytes(plistlib.dumps(job))
# Also checkpoint once a minute so a forced restart does not lose the whole session.
prefs_job = tree / 'LaunchDaemons/com.redwood.varstore.plist'
prefs_plist = plistlib.loads(prefs_job.read_bytes())
prefs_plist['ProgramArguments'] = ['/redwood/bin/var_store', 'watch', '60']
prefs_job.write_bytes(plistlib.dumps(prefs_plist))
(out / 'root.manifest').write_text('\n'.join(manifest) + '\n')
firmware = root / 'backups/k40-before-ios-20261101/firmware/wifi/qca6390'
(tree / 'fw').mkdir(exist_ok=True)
records = {}
for source, name in [('amss20.bin', 'amss20.bin'), ('m3.bin', 'm3.bin'),
                     ('regdb.bin', 'regdb.bin'), ('bd_k11a.elf', 'board.bin')]:
    shutil.copyfile(firmware / source, tree / 'fw' / name)
    records[name] = {'source': source, 'sha256': hashlib.sha256((firmware / source).read_bytes()).hexdigest()}
(out / 'firmware.json').write_text(json.dumps(records, indent=2) + '\n')
if args.audio:
    (tree / 'fw-audio').mkdir(exist_ok=True)
    for source in (root / 'backups/k40-before-ios-20261101/firmware/audio').iterdir():
        if source.is_file():
            shutil.copyfile(source, tree / 'fw-audio' / source.name)
    shutil.copyfile(root / 'build/k40/userland/obj/RedwoodAudio', tree / 'lib/RedwoodAudio.driver/RedwoodAudio')
    os.chmod(tree / 'lib/RedwoodAudio.driver/RedwoodAudio', 0o755)
    subprocess.run(['python3', str(root / 'build/k40/userland/mkjobs.py'),
                    str(root / 'build/k40/ios165/rootfs'), str(tree / 'LaunchDaemons'),
                    'com.apple.mediaserverd', 'com.apple.mediaremoted', 'com.apple.nanoregistryd'], check=True)
if args.bluetooth:
    (tree / 'fw-bt').mkdir(exist_ok=True)
    for name in ('htbtfw20.tlv', 'htnv20.bin'):
        shutil.copyfile(root / 'backups/k40-before-ios-20261101/firmware/bluetooth' / name,
                        tree / 'fw-bt' / name)
    shutil.copyfile(root / 'build/k40/userland/obj/libalioth-bt.dylib', tree / 'lib/libalioth-bt.dylib')
    os.chmod(tree / 'lib/libalioth-bt.dylib', 0o755)
    with (tree / 'etc/inject.conf').open('a') as inject:
        inject.write('/redwood/lib/libalioth-bt.dylib bluetoothd BlueTool\n')
    subprocess.run(['python3', str(root / 'build/k40/userland/mkjobs.py'),
                    str(root / 'build/k40/ios165/rootfs'), str(tree / 'LaunchDaemons'),
                    'com.apple.bluetoothd', 'com.apple.BlueTool'], check=True)
if args.audio_test:
    probe = {
        'Label': 'com.redwood.audio-check', 'RunAtLoad': True,
        'ProgramArguments': ['/redwood/bin/sound_test', 'wait', '55', 'level', '0.1', 'tone', '3', '1000', '440', 'route'],
        'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console',
    }
    (tree / 'LaunchDaemons/com.redwood.audio-check.plist').write_bytes(plistlib.dumps(probe))
# The packaged desktop root leaves networking services out. Add only those
# needed for Settings, DHCP, DNS and the Wi-Fi keychain, using stock iOS jobs.
subprocess.run(['python3', str(root / 'build/k40/userland/mkjobs.py'),
    str(root / 'build/k40/ios165/rootfs'), str(tree / 'LaunchDaemons'),
    'com.apple.configd', 'com.apple.mDNSResponder', 'com.apple.securityd',
    'com.apple.mobile.keybagd', 'com.apple.sleepd', 'com.apple.nehelper-embedded',
    'com.apple.trustd', 'com.apple.nsurlsessiond', 'com.apple.accountsd', 'com.apple.akd',
    'com.apple.appleaccountd', 'com.apple.amsaccountsd', 'com.apple.itunesstored', 'com.apple.apsd',
    'com.apple.identityservicesd', 'com.apple.SCHelper', 'com.apple.AppSSODaemon', 'com.apple.adid',
    'com.apple.devicecheckd', 'com.apple.tccd', 'com.apple.followupd', 'com.apple.mobileactivationd',
    'com.apple.timed'], check=True)
# Activation is requested by DeviceCheck. Start it on demand while this port is under test.
job = tree / 'LaunchDaemons/com.apple.mobileactivationd.plist'
plist = plistlib.loads(job.read_bytes())
for key in ('RunAtLoad', 'KeepAlive', 'LaunchEvents'):
    plist.pop(key, None)
job.write_bytes(plistlib.dumps(plist, fmt=plistlib.FMT_BINARY))
# The desktop system image excludes this executable. Keep Apple's own bundle in the RAM root,
# extracted from this workspace's existing iOS image; no Apple binary is part of the source overlay.
identity_path = Path('System/Library/PrivateFrameworks/IDS.framework/identityservicesd.app')
identity_input = root / 'runtime/account-input' / identity_path
if not (identity_input / 'identityservicesd').is_file():
    (root / 'logs').mkdir(exist_ok=True)
    with (root / 'logs/extract-identity-service.log').open('w') as log:
        subprocess.run([str(root / 'tools/7zip/7zz'), 'x', '-y',
                        '-o' + str(root / 'runtime/account-input'),
                        str(root / 'build/k40/ios165/rootfs.dmg'), str(identity_path)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
identity_dest = tree / 'share/identityservicesd.app'
shutil.copytree(identity_input, identity_dest, dirs_exist_ok=True)
job = tree / 'LaunchDaemons/com.apple.identityservicesd.plist'
plist = plistlib.loads(job.read_bytes())
identity_program = '/redwood/share/identityservicesd.app/identityservicesd'
if 'Program' in plist:
    plist['Program'] = identity_program
if plist.get('ProgramArguments'):
    plist['ProgramArguments'][0] = identity_program
job.write_bytes(plistlib.dumps(plist, fmt=plistlib.FMT_BINARY))
if args.account_console_log:
    for name in ('akd', 'accountsd', 'appleaccountd', 'amsaccountsd', 'devicecheckd', 'mobileactivationd'):
        job = tree / f'LaunchDaemons/com.apple.{name}.plist'
        plist = plistlib.loads(job.read_bytes())
        plist.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
        job.write_bytes(plistlib.dumps(plist, fmt=plistlib.FMT_BINARY))
# SpringBoard's Sleep.framework looks up com.apple.sleepd.sleepserver again at
# once whenever the lookup fails: ~4000 failed lookups a second for the whole
# session, SpringBoard and launchd each keeping a core busy at its top clock
# (the heat and the battery drain). With the stock job the name exists. It is
# started on demand only, like mkjobs.py --rest jobs: no RunAtLoad and none of
# its launch events (alarms, Watch, CloudKit), which this port has nothing for.
job = tree / 'LaunchDaemons/com.apple.sleepd.plist'
plist = plistlib.loads(job.read_bytes())
for key in ('RunAtLoad', 'KeepAlive', 'LaunchEvents'):
    plist.pop(key, None)
job.write_bytes(plistlib.dumps(plist, fmt=plistlib.FMT_BINARY))
if args.wifid_console_log:
    # WiFiManager's join decisions go to os_log only. With OS_ACTIVITY_DT_MODE,
    # libsystem_trace also writes them to stderr, which this job sends to
    # /dev/console and so to the loader log.
    job = tree / 'LaunchDaemons/com.apple.wifid.plist'
    plist = plistlib.loads(job.read_bytes())
    plist.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    job.write_bytes(plistlib.dumps(plist, fmt=plistlib.FMT_BINARY))
# Separate jobs: a blocked DNS request or TCP connect must not hide the other layers.
probes = {
    'wifi-check': ['show', 'store', 'reach', 'example.com'],
    'wifi-ip': ['tcp', '1.1.1.1', '80'],
    'wifi-dns': ['resolve', 'example.com'],
    'wifi-tcp': ['tcp', 'example.com', '80'],
    'wifi-http': ['fetch', 'http://example.com'],
    'wifi-https': ['fetch', 'https://example.com'],
}
for name, steps in probes.items():
    probe = {
        'Label': f'com.redwood.{name}', 'RunAtLoad': True,
        'ProgramArguments': ['/redwood/bin/net_test', 'waitaddr', 'en0', '600', 'wait', '3', *steps],
        'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console',
    }
    (tree / f'LaunchDaemons/com.redwood.{name}.plist').write_bytes(plistlib.dumps(probe))
subprocess.run(['python3', str(root / 'build/k40/tools/apfs/mkapfs.py'),
    '-z', '--plain', 'fw', '-o', str(out / 'root.apfs'),
    f'redwood={tree},manifest={out / "root.manifest"}'], check=True)
print('RAM root ready: device-local Wi-Fi firmware, stock services and the Alioth settings saver.')
