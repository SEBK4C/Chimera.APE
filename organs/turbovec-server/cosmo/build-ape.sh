#!/bin/bash
# Build turbovec-server as an Actually Portable Executable (APE).
#
# Technique: ahgamut/rust-ape-example (custom Rust target spec + cosmocc as
# linker + cargo -Zbuild-std), adapted for the prebuilt ~/cosmocc toolchain
# and rustc nightly >= 1.98 (2026-06). See README-cosmo.md in this directory.
#
# Requirements:
#   - ~/cosmocc (cosmocc 14.1.0 prebuilt toolchain), override with $COSMO
#   - rustup nightly toolchain with the rust-src component
#
# Outputs (under cosmo/):
#   turbovec-server.com      x86_64-only APE
#   turbovec-server.fat.com  fat APE (x86_64 + aarch64)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CRATE="$(dirname "$HERE")"
export COSMO="${COSMO:-$HOME/cosmocc}"

CARGO_FLAGS=(-Zjson-target-spec -Zbuild-std=panic_abort,std --release)

cd "$CRATE"

echo "== building x86_64-unknown-linux-cosmo =="
ARCH=x86_64 cargo +nightly build "${CARGO_FLAGS[@]}" \
    --target=./cosmo/x86_64-unknown-linux-cosmo.json

echo "== building aarch64-unknown-linux-cosmo =="
ARCH=aarch64 cargo +nightly build "${CARGO_FLAGS[@]}" \
    --target=./cosmo/aarch64-unknown-linux-cosmo.json

X86="$CRATE/target/x86_64-unknown-linux-cosmo/release/turbovec-server.com.dbg"
A64="$CRATE/target/aarch64-unknown-linux-cosmo/release/turbovec-server.com.dbg"

echo "== apelink: x86_64-only APE =="
"$COSMO/bin/apelink" \
    -l "$COSMO/bin/ape-x86_64.elf" \
    -o "$HERE/turbovec-server.com" \
    "$X86"

echo "== apelink: fat APE (x86_64 + aarch64) =="
"$COSMO/bin/apelink" \
    -l "$COSMO/bin/ape-x86_64.elf" \
    -l "$COSMO/bin/ape-aarch64.elf" \
    -M "$COSMO/bin/ape-m1.c" \
    -o "$HERE/turbovec-server.fat.com" \
    "$X86" "$A64"

ls -la "$HERE"/turbovec-server*.com
file "$HERE"/turbovec-server*.com
