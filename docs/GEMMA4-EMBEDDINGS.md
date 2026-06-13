# Gemma 4 12B — native multimodal embeddings (read this before touching the embed path)

> Captured from the model author's guidance (2026-06-13). Gemma 4 came out the
> week of 2026-06-08 — it is **newer than any model-training cutoff**, so this
> behavior is not in model priors. Do not "remember" how older multimodal
> embedders work; this model is different. Reference repos on the build host:
> `~/gemma-4-12b-it-qat-q4_0-llamafile/` (HF model repo) and
> `~/gemma-4-12b-it-qat-q4_0-llamafile/Llamafile-gemma-4-12B-it-qat-q4_0-gguf-Inferance-And-embeddings/`
> (build/source + patches + `docs/mm-embedding.md`).

## What the model is

Gemma 4 **12B** is a **dense, natively-multimodal** model: image, audio, and
text are all consumed by the **main weights**, not by a separate bolted-on
embedding tower. There is one model; it sees everything.

- **Audio** enters as a **16 kHz signal chopped into slices** (frames).
- **Images** enter at a **specific pixel resolution**, fed through a
  **projector** into the token stream.
- At inference the modalities are **interleaved into one token sequence** —
  text, image-projected tokens, and audio-projected tokens sit side by side and
  the model processes them together in a single forward pass. The model does
  not care which token came from which modality; it sees them all at once.

This capability is **specific to the 12B model** — no other Gemma 4 size has
it. Treat "Gemma 4 12B" as a hard precondition anywhere this path is used.

## How to embed with it — the rule

**To embed any input (text, image, audio, or any interleaving of them):
reuse the inference path, and take the end embedding state.**

1. Build the interleaved token sequence exactly as for a chat/inference forward
   pass — media through the **projector**, interleaved with any text.
2. Run the **forward pass** (the same one used for generation).
3. Take the **end embedding state** — the model's final hidden state over the
   interleaved tokens — as the embedding vector.

One mechanism covers every modality and every mix of modalities, in one shared
vector space, because they all go through the same forward pass.

## What NOT to do (and why the GPU path looked "broken")

Do **not** route media through a separate, dedicated pooled-embeddings graph
(llamafile's `/v1/embeddings` with `{"prompt_string": "<marker>",
"multimodal_data": [...]}`). On Gemma 4 12B that path is wrong twice over:

- **Wrong representation.** It mean-pools a sequence that is ~95% projected
  media tokens, so the vector clusters by *modality* rather than *content*
  (the modality-gap measurements in the llamafile-gemma repo's
  `docs/mm-embedding.md`: cross-modal same-sentence cosine 0.30–0.61, raw
  cross-modal retrieval at chance). The **end-state-of-the-interleave** path is
  the intended representation.
- **Crashes the GPU backend.** The dedicated embd-input graph segfaults on the
  GPU backend (CUDA and Metal) — reproduced directly here on 2× RTX 4090: the
  request kills the server. The **inference/interleave path runs fine on GPU**
  (chat with image/audio works on GPU at full speed), which is exactly why the
  embedding should be taken from that path, not the dedicated one.

So the GPU failure is not "media embeddings can't work on GPU." It is "the
*dedicated pooled-embeddings graph* is the wrong tool." The right tool — the
projector+interleave forward pass — already works on GPU.

## How chimera implements it (branch `GPU-mm-embed-patch`, 2026-06-13)

The end-state path is the LAST-pooling hidden state, and it is now wired:

- **llamafile patch** —
  `patches/0015-gpu-media-embeddings-last-pooling.patch` in the llamafile-gemma
  repo. `LAST` pooling no longer forces *all* tokens to be outputs
  (`output_all` in `src/llama-context.cpp`), so a media decode outputs only the
  final token — like generation, which is GPU-safe. The embedding becomes the
  model's end hidden state over the interleaved sequence. patch-0009's GPU
  refusal is relaxed for LAST pooling (mean/cls still force all-outputs and stay
  refused on GPU). Rebuild llamafile after applying.
- **chimera** — starts the embed server with `--pooling last` and a 2048 ubatch
  (`src/organs.cpp`) and always calls `embed`/`embed_media`; the GPU skip is
  gone (`src/ingest.cpp`). Text, image and audio all embed as the end state of
  the interleave pass, in one shared 3840-d space, on GPU.

Verified on 2× RTX 4090 / CUDA 12.8: a 2-image + audio + text corpus ingests to
**7 vectors** (4 text chunks + 3 raw media vectors) on `--gpu auto`, and
`--search-file <image>` returns the matching image (`✓ verified`) via raw-vector
similarity. Direct llamafile check: text/image/audio embeddings all return
HTTP 200, dim 3840, norm 1.0, server stays up.

Note: the embedding space is now **LAST-pooled** (was mean). Re-ingest existing
corpora; dimensionality (3840) is unchanged. The embedded llamafile must carry
patch 0015 (the packaged APE does, via `scripts/package.sh`).
