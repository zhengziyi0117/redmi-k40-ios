#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for name in init var_store; do
    cp "port/src/hackios-xnu-userland-main/userland/src/$name.c" "build/k40/userland/src/$name.c"
done
cd build/k40
export THEOS="$PWD/../../tools/theos"
source xnu-env.sh
cd userland
flags=(-arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra)
xnu-clang "${flags[@]}" -Ilibmini -c libmini/mini.c -o obj/mini.o
xnu-clang "${flags[@]}" -c libmini/start.S -o obj/start.o
xnu-clang "${flags[@]}" -DALIOTH_DATA_STORE=1 -Ilibmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 \
    -o obj/var_store_data src/var_store.c obj/mini.o obj/start.o
xnu-clang "${flags[@]}" -Ilibmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 \
    -o obj/init_apps src/init.c obj/mini.o obj/start.o
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 -fblocks \
    -framework Foundation -framework MobileCoreServices -o obj/app_probe \
    ../../../port/src/hackios-xnu-userland-main/userland/src/app_probe.m
"$THEOS/toolchain/linux/iphone/bin/ldid" \
    -S../../../port/src/hackios-xnu-userland-main/userland/src/app_probe.entitlements obj/app_probe
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 -fblocks \
    -framework Foundation -framework Photos -o obj/photos_probe \
    ../../../port/src/hackios-xnu-userland-main/userland/src/photos_probe.m
"$THEOS/toolchain/linux/iphone/bin/ldid" \
    -S../../../port/src/hackios-xnu-userland-main/userland/src/photos_probe.entitlements obj/photos_probe
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 \
    -framework Foundation -o obj/ipa_install_probe \
    ../../../port/src/hackios-xnu-userland-main/userland/src/ipa_install_probe.m
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 \
    -framework Foundation -o obj/installer_setup \
    ../../../port/src/hackios-xnu-userland-main/userland/src/installer_setup.m
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O2 -dynamiclib \
    -o obj/libalioth-installer.dylib ../../../port/src/hackios-xnu-userland-main/userland/src/inject_installer.c
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O2 -dynamiclib \
    -framework Foundation -o obj/libalioth-app-trust.dylib \
    ../../../port/src/hackios-xnu-userland-main/userland/src/inject_app_trust.m
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 \
    -framework Foundation -framework CoreText -framework AVFoundation -framework VideoToolbox \
    -framework CoreMedia -framework CoreVideo -o obj/native_render_probe \
    ../../../port/src/hackios-xnu-userland-main/userland/src/native_render_probe.m
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O2 -fblocks -dynamiclib \
    -framework Foundation -framework CoreText -framework UIKit -framework QuartzCore -framework CoreGraphics \
    -framework AVFoundation -framework CoreMedia -framework CoreVideo \
    -o obj/libalioth-bili.dylib ../../../port/src/hackios-xnu-userland-main/userland/src/inject_bili_playback.m \
    ../../../port/src/hackios-xnu-userland-main/userland/src/bili_video_canvas.m
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 \
    -o obj/storage_quota_probe ../../../port/src/hackios-xnu-userland-main/userland/src/storage_quota_probe.c
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1 \
    -o obj/storage_usage_probe ../../../port/src/hackios-xnu-userland-main/userland/src/storage_usage_probe.c
