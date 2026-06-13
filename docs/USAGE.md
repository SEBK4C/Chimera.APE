# Using Chimera.APE (v0.1.0-alpha)

Chimera digests a directory of files — text, code, **images, audio** — into a
hybrid graph-vector database, then answers questions about them with cited,
checksum-verified provenance. One process supervises three embedded engines
(Gemma 4 12B via llamafile, QLever, TurboVec) over loopback; nothing ever
leaves your machine.

## What you need

1. **`chimera.ape`** (~315 MB) — the orchestrator with all three organ
   binaries embedded. From the release page.
2. **Gemma 4 12B QAT q4_0 weights** (~7.1 GB, Apache 2.0) — sidecar download:

   ```sh
   curl -LO https://huggingface.co/google/gemma-4-12B-it-qat-q4_0-gguf/resolve/main/gemma-4-12b-it-qat-q4_0.gguf
   ```

3. Hardware: any x86_64/aarch64 Linux, macOS, or Windows machine.
   - **CPU-only is fully supported but slow** (~7 tok/s on a fast Xeon):
     budget *minutes per document* at ingest. Keep CPU corpora to a few
     hundred files.
   - GPU (`--gpu auto`, NVIDIA/Metal) makes ingest interactive — see
    [docs/GPU.md](GPU.md) for CUDA setup and the one media-embedding caveat.
    One caveat:
     raw media *embeddings* are CPU-only for now (the server politely
     refuses on GPU; image/audio search degrades to the text bridge, which
     is the primary path anyway).
   - RAM: ≥ 16 GB recommended (the model maps ~8 GB).

## First run

```sh
chmod +x chimera.ape

# Ingest a directory (first run also unpacks the embedded organ binaries
# into <dir>/.chimera/runtime/):
./chimera.ape ingest ~/notes --model ./gemma-4-12b-it-qat-q4_0.gguf

# Ask questions:
./chimera.ape --search "what did we decide about the billing rewrite?" \
    --db ~/notes/.chimera --model ./gemma-4-12b-it-qat-q4_0.gguf
```

The answer arrives with a sources table; every citation is re-checksummed
against the file on disk at answer time:

```
Maria Chen leads Project Phoenix [1]. It is a rewrite of the billing system [1].

Sources:
  [1] phoenix.md#1  ✓ verified
```

`✓ verified` = the cited file is byte-identical to what was ingested.
`⚠ drifted` = it changed since (re-ingest advised). `⚠ missing` = it's gone.
The system would rather confess than fabricate.

## Images and audio

PNG/JPEG and WAV/MP3 files are first-class documents. At ingest, the model
reads each one — transcribing any legible text, otherwise describing the
scene or sound — and that derived text is indexed exactly like a text file
(searchable, citable, graph-linked). The raw media embedding is stored too,
so you can search by example:

```sh
# text search finds media through their derived descriptions:
./chimera.ape --search "the budget figure from the banner image" --db ... --model ...

# or query BY a file — similar images/audio rank by raw vector similarity,
# and its transcription/description feeds the normal text search:
./chimera.ape --search-file query.png --db ... --model ...
./chimera.ape --search-file clip.wav --search "when was this chime recorded?" --db ... --model ...
```

## Keeping the database honest

```sh
./chimera.ape status  --db ~/notes/.chimera         # counts, dims, staleness
./chimera.ape verify  --db ~/notes/.chimera         # re-checksum everything
./chimera.ape verify  --db ... --paranoid           # full BLAKE3, catches edits
                                                    # the sampled hash can miss
./chimera.ape ingest ~/notes --model ...            # idempotent: re-run anytime
./chimera.ape vacuum  --db ~/notes/.chimera         # purge superseded docs,
                                                    # rebuild the text index
./chimera.ape sparql 'PREFIX ch: <chimera://ontology#>
  SELECT ?p ?o WHERE { <chimera://doc/...> ?p ?o }' --db ...   # power users
```

Re-running ingest on an unchanged corpus is a fast no-op (identity checks
only — no model start). Edited files are re-ingested and their old versions
superseded; superseded data stays queryable until `vacuum`.

## Tuning

```
--k N            ANN candidates (default 24)
--hops N         graph expansion depth (default 1)
--ctx-budget N   synthesis context tokens (default 8192)
--include G / --exclude G    ingest glob filters
--gpu auto|off|N|nvidia    GPU offload for the model — see docs/GPU.md
--threads N                accepted; sequential ingest lands post-alpha
--json           machine-readable search output (full trace)
```

## Known alpha limitations

- Ingest is sequential; CPU extraction dominates wall-clock (~2–6 min per
  chunk at 7 tok/s). The §5 bounded-queue concurrency is designed but not
  yet wired.
- Incremental ingests after the first don't extend the BM25 *text* index
  (vector + graph search unaffected); `status` tells you when, `vacuum`
  rebuilds it.
- macOS/Windows: the orchestrator, QLever, and llamafile are true APEs;
  `turbovec-server` is built from Rust with Linux ABI assumptions inside an
  APE shell — Linux is the tested platform for the alpha, other OSes are
  expected-but-unverified.
- Rendered-text legibility through the vision pipeline has a known upstream
  bug (vendor patch pending); photos and scenes describe well, dense
  screenshots may OCR imperfectly.
