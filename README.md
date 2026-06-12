# Chimera.APE

> Three queries, one binary, zero regrets. Runs on anything, answers to no one.

One Actually Portable Executable that fuses three creatures into one body:

| Organ | Beast | Job |
|---|---|---|
| [llamafile](https://github.com/SEBK4C/Llamafile-gemma-4-12B-it-qat-q4_0-gguf-Inferance-And-embeddings) + Gemma 4 12B | lion | embeddings **and** chat from one server |
| [QLever](https://github.com/SEBK4C/qlever-cosmopolitan) | goat | SPARQL knowledge graph + BM25 text index |
| [TurboVec](https://github.com/SEBK4C/turbovec-cosmopolitan) | snake | quantized ANN vector search |

Point it at a directory and it digests everything inside into a hybrid
graph-vector database; ask it a question and it answers with synthesized,
cited, checksum-verified provenance.

The full specification is [prompt.md](prompt.md); resolved design decisions
are in [docs/DECISIONS.md](docs/DECISIONS.md). Read §2 of the prompt twice —
the chunkID spine is the architectural soul of the project.

## Quick start (dev)

```sh
# toolchain: ~/cosmocc (https://cosmo.zip), plus cmake + rust for the organs
cmake -S . -B build-native && cmake --build build-native -j
./build-native/test_spine

# organ binaries (see vendor/ READMEs for their builds), then:
source scripts/dev-env.sh
./build-native/chimera ingest ~/my-notes
./build-native/chimera --search "what did we decide about the billing rewrite?"
```

Cosmopolitan build (the actual APE):

```sh
PATH="$PATH:$HOME/cosmocc/bin" cmake -S . -B build-cosmo \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/cosmocc.cmake -DCMAKE_MAKE_PROGRAM=$(command -v make)
PATH="$PATH:$HOME/cosmocc/bin" cmake --build build-cosmo -j
```

## Layout

```
src/                  the orchestrator (C++17, thin by decree)
organs/turbovec-server/  the turbovec.ape organ (Rust; cosmo/ has the APE build)
third_party/          vendored single-file deps (sqlite, xxhash, blake3, json)
toolchains/           cosmocc CMake toolchain
vendor/               the three organ repos (cloned, not committed)
docs/DECISIONS.md     Appendix-C resolutions with evidence
```

## Status

- [x] Spine: triple-tap identity, manifest, walker, chunker, Turtle emitter
- [x] Organs build: qlever APEs, llamafile + Gemma 4 weights, turbovec.ape (fat x86_64+aarch64)
- [x] Supervisor + organ HTTP clients
- [x] Ingest pipeline (sequential v1; bounded-queue concurrency is Phase 2)
- [x] Search pipeline: ANN + BM25 → RRF → graph crawl → synthesis → ✓ verification
- [x] Multimodal: image/audio ingestion (transcribe-or-describe + raw media
      vectors) and `--search-file` query-by-example — see [docs/USAGE.md](docs/USAGE.md)
- [x] Zip-store payload packaging + first-run extraction (`scripts/package.sh`)
- [ ] §11 acceptance: Linux x86_64 sweep in progress; macOS/Windows pending
