#!/usr/bin/env bash
# H4/BlueTool bridge and UUID-guarded iOS 16.5 QCA compatibility.
set -euo pipefail
cd "$(dirname "$0")/../build/k40"
export THEOS="$PWD/../../tools/theos"
source xnu-env.sh
cd userland
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O2 -fblocks -dynamiclib \
    -install_name /redwood/lib/libalioth-bt.dylib -framework CoreFoundation -framework IOKit \
    -o obj/libalioth-bt.dylib src/inject_bluetooth.c src/bluetooth_compat_ios165.c
shasum -a 256 obj/libalioth-bt.dylib
