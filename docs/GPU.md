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

Gemma 4 12B is a **dense, natively-multimodal** model — the right way to embed
image/audio is to reuse its inference forward pass (projector + interleave) and
take the **end hidden state**, *not* a separate pooled-embeddings graph. See
[GEMMA4-EMBEDDINGS.md](GEMMA4-EMBEDDINGS.md) — read it before changing the embed
path.

What chimera does **today** is the older, dedicated path
(`/v1/embeddings` with `{prompt_string, multimodal_data}`), which **crashes the
GPU backend** (the dedicated embd-input graph segfaults on CUDA/Metal —
reproduced here on 2× RTX 4090; the request kills the server). So on GPU the
orchestrator **skips the raw-media-vector call** (`Organs::model_on_gpu()`):

- **Text** embeddings, **chat**, and image/audio **chat** (transcribe/describe)
  all run on GPU; media docs **index fully via their derived text** (the
  primary retrieval path), so text→media search and `--search-file` work
  through the text bridge.
- The **raw same-modality media vector** isn't produced on GPU yet. For it now,
  run that ingest with `--gpu off` (the dedicated path works on CPU).

This GPU skip is a **stopgap**. The real fix is the interleave+end-state path,
which already runs fine on GPU (chat-with-media does) — see GEMMA4-EMBEDDINGS.md.

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
grep -iE 'CUDA|device_info|tg = |offloaded|ngl' ./small-corpus/.chimera/logs/llamafile.log
# expect: a `device_info:` block listing each `CUDA0/CUDA1 : NVIDIA ...` device,
# and `print_timing ... tg = N t/s` with N in the tens-to-hundreds rather than
# ~7. (This llamafile auto-fits layers to device memory, so you see the CUDA
# device lines + the throughput rather than a classic "offloaded 49/49" line.)
```

If extraction or the embedding probe (`embedding dim: 3840`) succeeds, the GPU
path is wired; compare tok/s in the llamafile log against the CPU baseline to
confirm the offload took effect.

### Verified

End-to-end on **2× NVIDIA RTX 4090** (driver 580 / CUDA 12.8), `--gpu auto`
offloads Gemma 4 12B across both cards and runs the full pipeline:

- `device_info:` lists `CUDA0` and `CUDA1`; generation runs at **~90 tok/s**
  (prompt eval ~2400 tok/s) versus ~7 tok/s on CPU.
- **Dev build** (`build-cosmo/chimera`): ingest of a 3-doc corpus in **94 s**;
  `--search` returns a `✓ verified` citation.
- **`chimera-full.ape`** (single file, no `--model`/env): self-extracts organs +
  weights, ingests in **~55 s**, `--search` answers with a `✓ verified` citation.
- **`chimera.ape`** (organs embedded, weights via `--model`): ingests on GPU in
  **~29 s** (warm model cache).
- **Multimodal corpus** (two PNGs + a WAV + a Markdown note): ingests on GPU in
  **~60 s → 4 chunks**, each media doc indexed via its derived text (banner
  image OCR'd to "PHOENIX BUDGET 480K", audio + scene described). A text query
  then answers **"480K"** citing `banner.png#1 ✓ verified` — image retrieval
  through the text bridge, end-to-end on GPU.

Raw media embeddings remain CPU-only per the §"one caveat" above (use
`--gpu off` for that ingest); text/chat/vision-describe all run on GPU.
