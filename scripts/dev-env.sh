# source me: organ paths for dev runs (until zip-store extraction lands)
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CHIMERA_LLAMAFILE="$R/vendor/llamafile-gemma/bin/llamafile"
export CHIMERA_MODEL="$R/vendor/llamafile-gemma/models/gemma-4-12b-it-qat-q4_0.gguf"
export CHIMERA_MMPROJ="$R/vendor/llamafile-gemma/models/mmproj-gemma-4-12b-it-qat-q4_0.gguf"
export CHIMERA_QLEVER_SERVER="$R/vendor/qlever-cosmopolitan/build-cosmo/qlever-server"
export CHIMERA_QLEVER_INDEX="$R/vendor/qlever-cosmopolitan/build-cosmo/qlever-index"
# musl build on Linux dev boxes: the cosmo APE build busy-spins its rayon
# pool (cosmo thread-parking issue; capped via RAYON_NUM_THREADS in the
# supervisor, but musl behaves perfectly here).
export CHIMERA_TURBOVEC="$R/organs/turbovec-server/target/x86_64-unknown-linux-musl/release/turbovec-server"
