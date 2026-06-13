#!/bin/sh
# Bake the organ binaries into chimera's APE zip store → dist/chimera.ape.
# Weights stay sidecar by default (DECISIONS.md D3); pass --full to embed
# them too (produces the ~7.5 GB chimera-full.ape flavor).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZIPALIGN="$ROOT/vendor/llamafile-gemma/bin/zipalign"
CHIMERA="$ROOT/build-cosmo/chimera"
OUT="$ROOT/dist"

[ -x "$ZIPALIGN" ] || { echo "zipalign missing — build llamafile-gemma first" >&2; exit 1; }
[ -f "$CHIMERA" ] || { echo "build-cosmo/chimera missing — cosmocc build first" >&2; exit 1; }

mkdir -p "$OUT"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/organs"

cp "$ROOT/organs/turbovec-server/cosmo/turbovec-server.fat.com" "$STAGE/organs/turbovec-server"
cp "$ROOT/vendor/qlever-cosmopolitan/build-cosmo/qlever-server"  "$STAGE/organs/qlever-server"
cp "$ROOT/vendor/qlever-cosmopolitan/build-cosmo/qlever-index"   "$STAGE/organs/qlever-index"
cp "$ROOT/vendor/llamafile-gemma/bin/llamafile"                  "$STAGE/organs/llamafile"

cp "$CHIMERA" "$OUT/chimera.ape"
# -j0: store uncompressed so children can be copied out (and mmap'd) fast.
(cd "$STAGE" && "$ZIPALIGN" -j0 "$OUT/chimera.ape" organs/*)

if [ "${1:-}" = "--full" ]; then
  cp "$OUT/chimera.ape" "$OUT/chimera-full.ape"
  # Embed weights under CANONICAL names so the orchestrator finds them by a
  # fixed /zip path (no directory listing — which zipos doesn't expose on
  # macOS). zipalign -j0 junks the dir prefix, keeping the basename, so the
  # symlinks land as /zip/model.gguf and /zip/mmproj.gguf.
  MW="$ROOT/vendor/llamafile-gemma/models"
  ln -sf "$MW/gemma-4-12b-it-qat-q4_0.gguf" "$STAGE/model.gguf"
  ln -sf "$MW/mmproj-gemma-4-12b-it-qat-q4_0.gguf" "$STAGE/mmproj.gguf"
  (cd "$STAGE" && "$ZIPALIGN" -j0 "$OUT/chimera-full.ape" model.gguf mmproj.gguf)
  echo "built: $OUT/chimera-full.ape ($(du -h "$OUT/chimera-full.ape" | cut -f1))"
fi

echo "built: $OUT/chimera.ape ($(du -h "$OUT/chimera.ape" | cut -f1))"
