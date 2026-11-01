#!/bin/bash
# Build the XNU loader from the overlay (port/) into build/k40/xnuloader/loader.efi.
# xnuloader/build.sh wants lld-link; without it, Rust's bundled LLD (rust-lld -flavor link) is used.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
src=$root/port/src/hackios-xnu-kernel-main/xnuloader/loader.c
cp "$src" "$root/upstream/ios-alioth/src/hackios-xnu-kernel-main/xnuloader/loader.c"
cp "$src" "$root/build/k40/xnuloader/loader.c"
if ! command -v lld-link >/dev/null; then
	lld=$(ls ~/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
	[ -n "$lld" ] || { echo "neither lld-link nor rust-lld found" >&2; exit 1; }
	shim=$(mktemp -d)
	trap 'rm -rf "$shim"' EXIT
	printf '#!/bin/sh\nexec %s -flavor link "$@"\n' "$lld" > "$shim/lld-link"
	chmod +x "$shim/lld-link"
	PATH=$shim:$PATH
fi
bash "$root/build/k40/xnuloader/build.sh"
