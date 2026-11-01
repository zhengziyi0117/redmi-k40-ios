#!/usr/bin/env bash
# Rebuild the adapted kernel using the already prepared local toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."
export THEOS="$PWD/tools/theos"
athdir="$PWD/build/k40/xnu-deps/build/ath11k"
objdir="$PWD/build/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837"
mkdir -p "$objdir/bsd/DEVELOPMENT"
python3 upstream/ios-apollo/tools/mk-wlfw-header.py "$athdir/src/ath11k" \
    "$objdir/bsd/DEVELOPMENT/alioth_wlan_wlfw_ath11k.h" --chip 'qca6390 hw2.0'
stamp=$(mktemp)
trap 'rm -f "$stamp"' EXIT
touch "$stamp"
status=0
DEVICE=alioth bash build/k40/xnu-build.sh -j12 -k > logs/wifi-kernel-rebuild.log 2>&1 || status=$?
# The upstream export step rejects 217 optional DriverKit names with this SDK.
# Do not accept that status unless a new kernel was linked and no compiler or
# linker diagnostics occurred. Never accidentally boot a previous build.
if [ ! "$objdir/mach.development.bcm2837" -nt "$stamp" ] || \
    rg -q 'error:|Undefined symbols|undefined symbol|ld: .*error' logs/wifi-kernel-rebuild.log; then
    tail -n 80 logs/wifi-kernel-rebuild.log
    exit 1
fi
if [ "$status" != 0 ] && ! rg -q 'exported name not in import list' logs/wifi-kernel-rebuild.log; then
    tail -n 80 logs/wifi-kernel-rebuild.log
    exit "$status"
fi
sha256sum "$objdir/mach.development.bcm2837"
echo 'New Wi-Fi kernel linked; export-step diagnostics, if any, are in logs/wifi-kernel-rebuild.log.'
