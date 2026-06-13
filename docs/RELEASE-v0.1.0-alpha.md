# Chimera.APE v0.1.0-alpha

> Three queries, one binary, zero regrets. Runs on anything, answers to no one.

First alpha of Chimera.APE — one Actually Portable Executable that fuses a
**llamafile + Gemma 4 12B** (embeddings *and* chat), **QLever** (SPARQL graph
+ BM25 text index), and **TurboVec** (quantized ANN) into a single offline
hybrid graph-vector knowledge engine. Point it at a directory of files —
text, code, **images, audio** — and ask questions; answers come back with
synthesized, cited, checksum-verified provenance. Nothing leaves your machine.

## What you need

1. **`chimera.ape`** (~315 MB) — orchestrator with all three organ binaries
   embedded (llamafile, qlever-server, qlever-index, turbovec-server).
   Download from this release.
2. **Gemma 4 12B QAT q4_0 weights** + **multimodal projector** (sidecar,
   Apache 2.0, ~7.3 GB total) from HuggingFace:
   ```sh
   HF=https://huggingface.co/google/gemma-4-12B-it-qat-q4_0-gguf/resolve/main
   curl -LO $HF/gemma-4-12b-it-qat-q4_0.gguf          # ~7.1 GB
   curl -LO $HF/mmproj-gemma-4-12b-it-qat-q4_0.gguf   # ~175 MB (images/audio)
   ```
   Put both in the same folder; chimera finds the `mmproj-` file beside the
   model automatically. (Text-only works without the projector.)
3. Any x86_64/aarch64 Linux, macOS, or Windows box. CPU-only is supported but
   slow (~7 tok/s — minutes per document at ingest); a GPU makes it
   interactive.

## Quick start

```sh
chmod +x chimera.ape

# Ingest a directory (first run unpacks the embedded organs into .chimera/runtime/):
./chimera.ape ingest ~/notes --model ./gemma-4-12b-it-qat-q4_0.gguf

# Ask:
./chimera.ape --search "what did we decide about the billing rewrite?" \
    --db ~/notes/.chimera --model ./gemma-4-12b-it-qat-q4_0.gguf
```

```
Maria Chen leads Project Phoenix [1]. It is a rewrite of the billing system [1].

Sources:
  [1] phoenix.md#1  ✓ verified
```

`✓ verified` = the cited file is byte-identical to what was ingested;
`⚠ drifted` / `⚠ missing` tell you when it isn't. Citations are promises the
checksum keeps.

## Images and audio

PNG/JPEG/WAV/MP3 are first-class documents. At ingest the model transcribes
any legible text or describes the scene/sound, indexes that derived text, and
stores the raw media embedding for query-by-example:

```sh
./chimera.ape --search "the budget figure on the banner" --db ... --model ...   # text → media
./chimera.ape --search-file query.png --db ... --model ...                      # media → everything
```

## Other commands

```sh
./chimera.ape status  --db DIR/.chimera        # counts, dims, index staleness
./chimera.ape verify  --db ... [--paranoid]    # re-checksum the corpus
./chimera.ape vacuum  --db ...                 # purge superseded data, rebuild text index
./chimera.ape sparql  "SELECT ..." --db ...    # raw SPARQL into the graph
```

## Acceptance (Linux x86_64, CPU)

Run via `scripts/acceptance.sh` against the §11 criteria of the spec:

| Criterion | Result |
|---|---|
| APE format, statically linked | ✓ |
| Ingest mixed corpus; re-run 100% skipped < 2 s / 1000 docs | ✓ |
| Middle-edit supersession; `--paranoid` catches sampled-hash blind spots | ✓ |
| Search returns ≥1 `✓ verified` citation; deleted source → `⚠ missing` | ✓ |
| `vacuum` shrinks stores; `sparql` walks the live graph | ✓ |
| `kill -9` mid-ingest then resume: no duplicates | ✓ |

## Known alpha limitations

- **Sequential ingest.** CPU extraction dominates wall-clock (~2–6 min/chunk
  at 7 tok/s). The §5 bounded-queue concurrency is designed, not yet wired.
- **Incremental text index.** Ingests after the first don't extend the BM25
  text index (vector + graph search unaffected); `status` flags it, `vacuum`
  rebuilds it.
- **Platform.** The orchestrator, QLever, and llamafile are true fat APEs;
  `turbovec-server` is Rust-in-APE with Linux ABI assumptions, so Linux
  x86_64 is the tested platform for this alpha — other OSes are
  expected-but-unverified.
- **Vision OCR** of dense rendered text has a known upstream pipeline bug;
  photos/scenes describe well, screenshots may transcribe imperfectly.
- **Multimodal embeddings** are CPU-only (the server refuses them on GPU);
  media search degrades to the text bridge there, which is the primary path.

Full design rationale: `docs/DECISIONS.md`. Usage detail: `docs/USAGE.md`.
