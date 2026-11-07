#!/usr/bin/env bash
# Rebuild only the CoreAnimation OpenGL bridge against this workspace's validated Mesa.
set -euo pipefail
cd "$(dirname "$0")/../build/k40"
export THEOS="$PWD/../../tools/theos"
source xnu-env.sh
cd userland
mesa=${DISPLAY_MESA:-../../../runtime/wifi/root/lib/libOSMesa.8.dylib}
inputs=../xnu-deps/build/display
mkdir -p "$inputs"
python3 - "$inputs" "$mesa" <<'PY'
from pathlib import Path
import re, subprocess, sys, urllib.request
inputs, mesa = Path(sys.argv[1]), Path(sys.argv[2])
registry = '6af574a14089ccfee87efe230ebcdd8742859813'
if not (inputs / 'gl.xml').exists():
    url = f'https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/{registry}/xml/gl.xml'
    (inputs / 'gl.xml').write_bytes(urllib.request.urlopen(url, timeout=60).read())
symbols = subprocess.check_output(['llvm-nm', '--extern-only', '--defined-only', str(mesa)], text=True)
(inputs / 'exported-gl.txt').write_text('\n'.join(re.findall(r'\b_(gl\w+)$', symbols, re.M)) + '\n')
PY
python3 mkeagl.py src/eagl_slots.txt "$inputs/gl.xml" "$inputs/exported-gl.txt" src/eagl.c > obj/eagl_gen.h
xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O2 -dynamiclib \
    -install_name /redwood/lib/libredwood-gl.dylib -Iobj -framework CoreFoundation \
    -framework IOKit -framework IOSurface -lobjc "$mesa" \
    -o obj/libredwood-gl.dylib src/eagl.c src/gl_osmesa.c
shasum -a 256 obj/libredwood-gl.dylib
