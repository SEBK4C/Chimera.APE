#!/bin/bash
# Linker wrapper for the Rust x86_64/aarch64-unknown-linux-cosmo targets.
# Filters out linker args that Cosmopolitan's cc wrapper rejects, then
# delegates to the cosmocc per-arch compiler driver.
set -eu

COSMO="${COSMO:-$HOME/cosmocc}"
ARCH="${ARCH:-x86_64}"

declare -a args
args=()
for o in "$@" ; do
    case $o in
        "-lunwind") continue;;
        "-Wl,-Bdynamic") continue;;
        "-Wl,-Bstatic") continue;;
        "-Wl,--strip-all") continue;;  # cosmocc fixupobj needs the symtab
        "-Wl,--strip-debug") continue;;
    esac
    args+=("$o")
done

# Compile the libc stubs for this arch (cached) and add them to the link.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STUBS="$HERE/stubs-$ARCH.o"
if [ ! -e "$STUBS" ] || [ "$HERE/stubs.c" -nt "$STUBS" ]; then
    "$COSMO/bin/$ARCH-unknown-cosmo-cc" -c -O2 -o "$STUBS" "$HERE/stubs.c"
fi
args+=("$STUBS")

exec "$COSMO/bin/$ARCH-unknown-cosmo-cc" "${args[@]}"
