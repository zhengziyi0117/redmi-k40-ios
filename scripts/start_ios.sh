#!/usr/bin/env bash
# Temporarily boot this workspace's K40 iOS image. Does not flash or erase a partition.
set -euo pipefail
cd "$(dirname "$0")/.."
serial=${1:?Usage: bash scripts/start_ios.sh ANDROID_SERIAL}
bootdir=$PWD/runtime/boot

# Check all host inputs before changing the phone's state.
python3 - "$bootdir" <<'PY'
import hashlib, json, struct, sys
from pathlib import Path
p = Path(sys.argv[1])
m = json.loads((p/'manifest.json').read_text())
for name in ('alioth-boot.img', 'kernel-payload.bin', 'root.apfs', 'try-payload.bin', 'system.apfs'):
    f = p/name
    if name not in m or not f.is_file():
        raise SystemExit('Missing validated input: '+name)
    h = hashlib.file_digest(f.open('rb'), 'sha256').hexdigest()
    if h != m[name]['sha256'] or f.stat().st_size != m[name]['size']:
        raise SystemExit('Input changed since validation: '+name)
with (p/'system.apfs').open('rb') as f:
    head=f.read(64)
if head[32:36] != b'NXSB' or (p/'system.apfs').stat().st_size < 1_000_000_000:
    raise SystemExit('Full APFS system image is required')
data=(p/'try-payload.bin').read_bytes()
if data[:4] != b'XNUP' or struct.unpack_from('<I', data, 4)[0] != len(data)-8:
    raise SystemExit('Invalid XNUP payload')
offset=8; entries={}
while offset+24<=len(data):
    name,size,_=struct.unpack_from('<16sII',data,offset); offset+=24
    name=name.split(b'\0')[0].decode()
    if not name: break
    if offset+size>len(data): raise SystemExit('Truncated XNUP entry')
    entries[name]=data[offset:offset+size]; offset+=size+(-size%16)
if entries.get('kernel') != (p/'kernel-payload.bin').read_bytes() or entries.get('ramdisk') != (p/'root.apfs').read_bytes():
    raise SystemExit('Payload components differ from the validated kernel/root pair')
args=entries.get('cmdline',b'').decode().rstrip('\0').split()
if 'ufs_write=1' in args or any(a.startswith(('root_apfs=', 'pcie=')) for a in args):
    raise SystemExit('First boot requires RAM root and no PCIe probe or data writes')
print('Host images validated; RAM system and matching payload are ready', flush=True)
PY

if [ "$(adb -s "$serial" get-state 2>/dev/null || true)" = device ]; then
    [ "$(adb -s "$serial" shell getprop ro.product.device | tr -d '\r')" = alioth ] || {
        echo 'Selected Android device is not alioth' >&2; exit 1;
    }
    [ "$(adb -s "$serial" shell getprop ro.boot.flash.locked | tr -d '\r')" = 0 ] || {
        echo 'Bootloader is locked or its state is unknown' >&2; exit 1;
    }
    echo 'Rebooting the verified K40 into its bootloader'
    adb -s "$serial" reboot bootloader
fi

ready=0
for i in $(seq 1 40); do
    if fastboot devices | awk -v s="$serial" '$1==s {found=1} END {exit !found}'; then
        ready=1; break
    fi
    sleep 1
done
[ "$ready" = 1 ] || { echo 'Target K40 did not enter fastboot' >&2; exit 1; }
product=$(timeout 10 fastboot -s "$serial" getvar product 2>&1)
printf '%s\n' "$product"
printf '%s\n' "$product" | grep -Eq 'product: *alioth([[:space:]]|$)' || exit 1
unlocked=$(timeout 10 fastboot -s "$serial" getvar unlocked 2>&1)
printf '%s\n' "$unlocked"
printf '%s\n' "$unlocked" | grep -Eiq 'unlocked: *(yes|true)([[:space:]]|$)' || {
    echo 'Fastboot did not confirm an unlocked bootloader' >&2; exit 1;
}

timeout 45 fastboot -s "$serial" boot "$bootdir/alioth-boot.img"
ready=0
for i in $(seq 1 40); do
    if fastboot devices | awk '$1=="xnuloader" {found=1} END {exit !found}'; then
        ready=1; break
    fi
    sleep 1
done
[ "$ready" = 1 ] || { echo 'XNU loader did not enumerate' >&2; exit 1; }
timeout 10 fastboot -s xnuloader getvar product 2>&1 | grep -Eq 'product: *xnuloader([[:space:]]|$)'
echo 'Staging the full iOS system into RAM'
timeout 15 fastboot -s xnuloader oem ramdisk
timeout 600 fastboot -s xnuloader stage "$bootdir/system.apfs"
timeout 60 fastboot -s xnuloader stage "$bootdir/try-payload.bin"
timeout 15 fastboot -s xnuloader oem run
echo 'iOS kernel started; capture with: python3 scripts/capture_ios.py --out runtime/captures/first'
