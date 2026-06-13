# Running and building Chimera.APE on a GPU (CUDA)

The whole point of moving to a GPU box: ingest extraction/embedding and search
synthesis go from ~7 tok/s (CPU) to interactive. Nothing about the database,
spine, or organs changes — only how the llamafile organ executes the model.

## Running on GPU

The orchestrator passes GPU-offload flags to the embedded llamafile via
`--gpu` (§8). Default is `auto`.

```sh
./chimera-full.ape ingest ~/corpus --gpu auto     # offload all layers (default)
./chimera-full.ape ingest ~/corpus --gpu nvidia   # force the CUDA backend
./chimera-full.ape ingest ~/corpus --gpu 24       # offload only 24 layers (small VRAM)
./chimera-full.ape ingest ~/corpus --gpu off      # force CPU
```

Mapping to llamafile flags:

| `--gpu` value | llamafile flags | meaning |
|---|---|---|
| `auto` (default) | `-ngl 999` | offload all layers; auto-detect vendor; **falls back to CPU if no GPU** |
| `off` / `disable` | `--gpu disable` | force CPU |
| integer `N` | `-ngl N` | offload N of the model's layers (partial; for VRAM-limited cards) |
| `nvidia` / `amd` / `apple` | `--gpu <vendor> -ngl 999` | pin the backend vendor |

`--gpu` applies wherever the model runs (`ingest` and `--search`/`--search-file`).

### CUDA prerequisites

llamafile compiles its CUDA module (`ggml-cuda`) on **first GPU run** using the
system toolkit, then caches it under `~/.llamafile/`. So the GPU box needs
**either**:

- the **CUDA toolkit** (`nvcc` on `PATH`) — llamafile JITs an optimized module
  (one-time, ~1–2 min, then cached), **or**
- just the **CUDA runtime/driver** — llamafile uses its prebuilt tinyBLAS path.

Check: `nvidia-smi` works and (ideally) `nvcc --version` is on `PATH`. The
first `ingest` will log the GPU it found and the offload count in
`<db>/.chimera/logs/llamafile.log` — grep for `ngl` / `CUDA` / `offloaded`.

### Multimodal embeddings on GPU — the one caveat

Gemma 4's **media embedding** path crashes the GPU backend, so the vendored
llamafile (patch 0009) refuses media on `/v1/embeddings` with HTTP 501 when
running on GPU. Consequence:

- **Text** embeddings, **chat**, and image/audio **chat** (transcribe/describe)
  all work fine on GPU.
- **Raw media vectors** (the ordinal-0 same-modality search vector) are not
  produced on GPU — chimera logs a note and the media doc still indexes fully
  via its derived text (the primary retrieval path). `--search-file` then
  relies on the text bridge rather than raw-vector similarity.

If you specifically want raw media vectors, run that ingest with `--gpu off`
(media embedding works on CPU). This is an upstream llama.cpp/Gemma-4 GPU bug,
not a chimera one; revisit when the vendored llamafile picks up a fix.

## Building on the GPU box

Same toolchain as the build host. From a clean checkout:

```sh
# 1. Toolchain
curl -L https://cosmo.zip/pub/cosmocc/cosmocc.zip -o /tmp/c.zip
mkdir -p ~/cosmocc && (cd ~/cosmocc && unzip -o /tmp/c.zip && chmod +x bin/*)
#   plus: cmake, a host cc/c++, rustup (+ x86_64-unknown-linux-musl), python3

# 2. Organs (clone the three forks under vendor/)
git clone --depth 1 https://github.com/SEBK4C/qlever-cosmopolitan vendor/qlever-cosmopolitan
git clone --depth 1 https://github.com/SEBK4C/turbovec-cosmopolitan vendor/turbovec-cosmopolitan
git clone --depth 1 https://github.com/SEBK4C/Llamafile-gemma-4-12B-it-qat-q4_0-gguf-Inferance-And-embeddings vendor/llamafile-gemma

# QLever APEs (one-time dep sysroot, then build)
(cd vendor/qlever-cosmopolitan && JOBS=$(nproc) sh cosmo/build-deps.sh && \
   JOBS=$(nproc) sh cosmo/build.sh && sh cosmo/package.sh)

# llamafile + zipalign + Gemma 4 weights (see build-state notes: nested
# submodules must be `git clean -fdx` before re-patching; patch 0001's
# kv-cache.cpp hunk is already upstream — apply only the iswa half)
(cd vendor/llamafile-gemma && make setup && JOBS=$(nproc) make build && make model)

# turbovec.ape (fat APE) — or the musl binary for a Linux-only box
(cd organs/turbovec-server && bash cosmo/build-ape.sh)   # x86_64+aarch64 fat APE
#   Linux-only alternative: cargo build --release --target x86_64-unknown-linux-musl

# 3. Orchestrator (APE)
PATH="$PATH:$HOME/cosmocc/bin" cmake -S . -B build-cosmo \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/cosmocc.cmake -DCMAKE_MAKE_PROGRAM=$(command -v make)
PATH="$PATH:$HOME/cosmocc/bin" cmake --build build-cosmo -j$(nproc)

# 4. Package (organs embedded; --full also embeds weights → chimera-full.ape)
bash scripts/package.sh --full
```

> The APE container runs on any host, but `--gpu nvidia` only does anything
> where llamafile finds CUDA. The APE itself is host-agnostic; CUDA is resolved
> at runtime by llamafile, not baked into the binary — so the same
> `chimera-full.ape` runs CPU on your laptop and GPU on the server.

## Quick GPU sanity check after building

```sh
./build-cosmo/chimera ingest ./small-corpus --gpu auto --verbose
grep -iE 'ngl|CUDA|offloaded|GPU' ./small-corpus/.chimera/logs/llamafile.log
# expect: a CUDA device line + "offloaded 49/49 layers" (or similar), and a
# tokens/s in the hundreds rather than ~7.
```

If extraction or the embedding probe (`embedding dim: 3840`) succeeds, the GPU
path is wired; compare tok/s in the llamafile log against the CPU baseline to
confirm the offload took effect.
