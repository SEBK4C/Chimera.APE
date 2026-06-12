# Chimera.APE — Decision Record

Resolutions for `prompt.md` Appendix C, based on reading the actual vendored
repos on 2026-06-13. Each decision cites the evidence that forced it.

---

## D1. TurboVec's exact API surface → we must build the server organ ourselves

**Finding.** `vendor/turbovec-cosmopolitan` is the *upstream* turbovec library
(Rust crate v0.9.0 + Python bindings), unmodified — no server, no CLI, no C
FFI, and zero Cosmopolitan work. The name of the repo is aspiration, not
state.

What it does give us is exactly the index we need:

- `IdMapIndex::new(dim, bit_width)` / `new_lazy(bit_width)` — dim fixed at
  first add; must be a multiple of 8 (3840 ✓); bit_width ∈ {2,3,4}.
- `add_with_ids(&[f32], &[u64])` — external u64 IDs, unique, cosine metric,
  vectors quantized via TurboQuant (data-oblivious, no training phase).
- `search_with_allowlist(queries, k, Option<&[u64]>) -> (scores, ids)`.
- `remove(id) -> bool` — O(1) tombstone-equivalent (swap-remove).
- `write(path)` / `load(path)` — `.tvim` format, magic `TVIM` v3.

**Decision.** Build `turbovec.ape` as a thin Rust binary in this repo
(`organs/turbovec-server/`): a single-threaded-accept loopback HTTP server
(hand-rolled or `tiny_http`; no async stack) exposing:

```
GET  /health            -> 200 "ok"
GET  /info              -> {dim, count, bit_width}
POST /upsert            -> {ids:[u64], vectors:[[f32]]}   (remove-then-add = upsert)
POST /query             -> {vectors:[[f32]], k, allowlist?:[u64]} -> {scores, ids}
POST /remove            -> {ids:[u64]}
POST /persist           -> flush .tvim to --path
```

APE build path: Rust does not target cosmocc natively. Plan A is ahgamut's
`rust-ape-example` technique (custom `x86_64-unknown-linux-cosmo` target spec
linking against the cosmocc sysroot). Plan B (fallback, documented caveat):
`x86_64-unknown-linux-musl` fully static binary per arch — loses the
run-anywhere property for this one organ until Plan A lands. The orchestrator
treats it as an opaque child either way, so swapping the build method later
costs nothing.

**chunkID → u64 mapping.** TurboVec keys are u64; chunkIDs are strings
(`{docID}:{NNNN}`). `vecKey = xxh3_64(chunkID)`. The manifest stores
`chunk(chunkID TEXT PRIMARY KEY, veckey INTEGER UNIQUE, ...)` so collisions
are detected at insert time (probability ~n²/2⁶⁴; at 10M chunks ≈ 3e-6) and
reverse lookup never depends on hashing being injective. On collision: bump a
salt byte appended to the chunkID for that one key, record the salt in the
manifest row.

## D2. QLever bulk-load mechanics → index build for initial ingest, SPARQL UPDATE thereafter

**Finding.** `vendor/qlever-cosmopolitan` already ships the Cosmopolitan
port, e2e-green as of 2026-06-12 (`cosmo/REPORT-2026-06-12.md`): APE
artifacts `qlever-index` (113 MB), `qlever-server` (117 MB),
`PrintIndexVersionMain`. Built via `cosmo/build-deps.sh` (zlib, zstd,
OpenSSL, ICU 76.1, Boost 1.83 into `~/cosmos` sysroot) → `cosmo/build.sh` →
`cosmo/package.sh` (embeds ICU data in the APE zip).

- Initial build: `qlever-index -i <basename> -F ttl -f file.ttl ...`
- Text index: `-W` (index all RDF string literals — exactly our
  `ch:text` chunks), `-S bm25`. Predicates `ql:contains-word` /
  `ql:contains-entity` with implicit `?ql_textscore_…` variable.
- Server: `qlever-server -i <basename> -p <port> -t [-a token]`;
  health = `GET /ping`; queries via `POST /` with
  `application/sparql-query`; **SPARQL 1.1 UPDATE is supported** via
  `application/sparql-update`, with `--persist-updates` to make updates
  durable.

**Verified text-query shape** (tested against the built APE, 2026-06-13 —
`ql:` is a *built-in* prefix for
`http://qlever.cs.uni-freiburg.de/builtin-functions/`; declaring the old
`<QLever-internal-function/>` form silently matches nothing). In
from-literals mode `ql:contains-entity` binds the literal itself, so the
keyword retriever (§6 step 3) is:

```sparql
PREFIX ch: <chimera://ontology#>
SELECT ?chunk ?ql_score_t_var_text WHERE {
  ?t ql:contains-word "term1 term2*" .
  ?t ql:contains-entity ?text .
  ?chunk ch:text ?text .
} ORDER BY DESC(?ql_score_t_var_text)
```

**Decision.** Initial ingest emits Turtle batches to a staging file and runs
`qlever-index` (with `-W -S bm25`) once at the end of stage 7, then boots
`qlever-server`. Subsequent ingests (supersession, new docs) go through
SPARQL UPDATE over HTTP with `--persist-updates`. `vacuum` rebuilds the
index from a CONSTRUCT dump when tombstone bloat crosses a threshold —
rebuild-from-dump is also the escape hatch if incremental update of the
*text* index proves not to cover new literals (verify during task #8; if
new `ch:text` literals are not text-searchable after UPDATE, fall back to
periodic re-index).

## D3. Default packaging → sidecar weights by default, monolith as release flavor

**Finding.** `vendor/llamafile-gemma` packages a 6.5–7.2 GB
`gemma4-server.llamafile` via `zipalign -j0` (uncompressed, mmap-aligned).
The naked server binary is ~50 MB.

**Decision.** As the prompt recommends: ship both flavors.

- **Default / dev:** `chimera.ape` embeds `qlever.ape`, `turbovec.ape`, and
  the 50 MB llamafile binary (≈300 MB total), with weights as a sidecar GGUF
  resolved via `--model PATH` or `db/runtime/model/`. `chimera.ape` knows the
  HuggingFace URL (`google/gemma-4-12B-it-qat-q4_0-gguf`, ~7.1 GB) and can
  fetch-on-first-run with consent — the only permitted non-loopback traffic,
  opt-in, never during normal operation.
- **Release flavor `chimera-full.ape`:** weights in the ZIP store too
  (`zipalign -j0`), true single file, ~7.5 GB.

## D4. HTML ingestion → in v1, minimal

Tag-stripping HTML→text is ~100 lines with no dependency and the extractor
interface (`extract(path) → utf8 + structure hints`) exists from day one.
PDF and the rest stay Phase 4.

---

## Additional decisions forced by the recon

### D5. The model is Gemma 4 12B, not Gemma 3

The working llamafile repo serves `google/gemma-4-12B-it-qat-q4_0-gguf`
(7.1 GB) on a llamafile **v0.10.7** fork. Embeddings are 3840-dim,
mean-pooled (`--pooling mean`), L2-normalized. Dual serving (chat +
embeddings from one instance) works via `--embeddings --pooling mean` plus
the fork's patch 0001 (one-seq-per-ubatch — fixes pooled-embedding
corruption for unequal-length batches under iSWA). The startup probe of
`/v1/embeddings` (§1 of the prompt) remains the source of truth for
dimensionality; nothing hardcodes 3840.

Launch args come from `package/gemma4.args`; we override host/port and pin:
`--server --embeddings --pooling mean -c 8192 -np 2`. Health = `GET /health`.

### D6. Organ child-process contract

All three organs are HTTP-over-loopback children supervised by the
orchestrator (§7). Health endpoints: llamafile `GET /health`, qlever
`GET /ping`, turbovec `GET /health` (ours, by construction). Ephemeral ports
chosen by the orchestrator (bind(0) probe, then passed via flags).

### D7. Orchestrator third-party policy

Vendored single-file/amalgamation deps only, compiled into the APE:
`sqlite3` amalgamation (manifest), `xxhash` (XXH3-128/64), BLAKE3 portable C
(`--paranoid`), a small JSON lib (nlohmann single header), and
`cpp-httplib`-style minimal HTTP client over plain sockets (loopback only, no
TLS needed). Everything builds with cosmocc; no dynamic linking anywhere.
