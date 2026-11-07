#!/usr/bin/env bash
# Build the small, existing var_store archive tool for Alioth's reserved boot_b tail.
set -euo pipefail
cd "$(dirname "$0")/../build/k40"
export THEOS="$PWD/../../tools/theos"
source xnu-env.sh
cd userland
flags=(-arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra)
xnu-clang "${flags[@]}" -Ilibmini -c libmini/mini.c -o obj/mini.o
xnu-clang "${flags[@]}" -c libmini/start.S -o obj/start.o
xnu-clang "${flags[@]}" -DALIOTH_PREFS_STORE=1 -Ilibmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 \
    -o obj/var_store_alioth src/var_store.c obj/mini.o obj/start.o
