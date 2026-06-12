# source me: organ paths for dev runs (until zip-store extraction lands)
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CHIMERA_LLAMAFILE="$R/vendor/llamafile-gemma/bin/llamafile"
export CHIMERA_MODEL="$R/vendor/llamafile-gemma/models/gemma-4-12b-it-qat-q4_0.gguf"
export CHIMERA_QLEVER_SERVER="$R/vendor/qlever-cosmopolitan/build-cosmo/qlever-server"
export CHIMERA_QLEVER_INDEX="$R/vendor/qlever-cosmopolitan/build-cosmo/qlever-index"
export CHIMERA_TURBOVEC="$R/organs/turbovec-server/cosmo/turbovec-server.fat.com"
