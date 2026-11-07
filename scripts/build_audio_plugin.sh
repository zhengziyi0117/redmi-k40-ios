#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../build/k40"
export THEOS="$PWD/../../tools/theos"
source xnu-env.sh
cd userland
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O2 \
    -bundle -framework CoreFoundation -o obj/RedwoodAudio src/audio_plugin.c
shasum -a 256 obj/RedwoodAudio
