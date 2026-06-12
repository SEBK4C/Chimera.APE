# CHIMERA.APE — Build Prompt v1

You are building **Chimera.ape**: a single Actually Portable Executable that fuses three creatures into one body — a **llamafile** (Gemma 3 12B) for embeddings *and* inference, **QLever** for a SPARQL knowledge graph with integrated text search, and **TurboVec** for quantized approximate-nearest-neighbor vector search. One binary. It runs on Linux, macOS, Windows, and BSDs unmodified. Point it at a directory and it digests everything inside into a hybrid graph-vector database; ask it a question and it answers with synthesized, cited, checksum-verified provenance.

The rest of this document is deadly serious. Read §2 twice — it is the architectural soul of the project.

---

## 1. Non-negotiable constraints

- **Toolchain:** Cosmopolitan Libc (`cosmocc`). Output is one APE. No dynamic linking, no runtime dependencies, no network traffic except loopback between embedded components. Offline-first, always.
- **Payloads ride inside the binary.** APE files are valid ZIP archives; use the APE ZIP store to carry `qlever.ape`, `turbovec.ape`, the llamafile, and the Gemma 3 12B weights (Q4_K_M, ~7–8 GB). On first run, extract payloads to the database's `runtime/` directory, version-stamped. Also support `--model PATH` as a sidecar override, because 8 GB executables make some filesystems cry.
- **One model for everything.** Gemma 3 12B serves both embeddings (mean-pooled, L2-normalized) and chat completions through the llamafile's OpenAI-compatible endpoints (`/v1/embeddings`, `/v1/chat/completions` — verify the exact flag set and endpoint shapes against the bundled llamafile version; you may need `--embedding` enabled). Query and corpus share one vector space because they share one brain. At startup, **probe** the embedding endpoint to discover dimensionality (expect 3840 for Gemma 3 12B) and configure TurboVec from the probe. Never hardcode the dimension.
- **Orchestrator language:** C++ (≥17) built with `cosmocc`, matching the QLever / llama.cpp ecosystems. The orchestrator stays *thin*: it owns process supervision, the manifest, the two pipelines, and the CLI. It does not reimplement any database.
- **Determinism where it matters:** extraction calls run at temperature ≤ 0.2 with strict JSON schemas; synthesis may run warmer.

---

## 2. The Spine — one ID binds three stores

Every design decision below hangs off this section.

### 2.1 The triple-tap checksum

Document identity comes from a **sampled identity hash** built for speed on arbitrarily large files:

```
docID = hex( xxh3_128( filesize ‖ head 64 KiB ‖ middle 64 KiB ‖ tail 64 KiB ) )
```

Files smaller than 192 KiB are hashed whole. Cost is O(1) with respect to file size — three seeks and a hash.

**Honesty note for the implementer:** sampled hashing can miss edits confined to unsampled regions. This is an accepted speed tradeoff. Provide `--paranoid` to substitute a full BLAKE3 hash for users who don't trust their middles.

### 2.2 The ID lattice

```
docID    = triple-tap hash (16 hex chars is plenty)
chunkID  = {docID}:{NNNN}            # zero-padded ordinal within the document
IRIs     = chimera://doc/{docID}
           chimera://chunk/{chunkID}
           chimera://entity/{slug}
           chimera://tag/{slug}
           chimera://rel/{lower_snake_verb}
```

The **same chunkID** is simultaneously: the TurboVec vector key, the QLever subject IRI, and the manifest row key. **No other join keys exist anywhere in the system.** If a piece of data cannot be reached from a docID or chunkID, it does not belong in Chimera.

### 2.3 The checksum has three jobs

1. **Ingest — identity & dedup.** Same content discovered at two paths → one `ch:Document` node carrying multiple `ch:locatedAt` triples. Ingest the bytes once.
2. **Re-ingest — supersession.** A changed file produces a new docID. The old document node gets `ch:supersededBy → new doc`; its vectors are tombstoned in TurboVec; its graph subtree survives until `vacuum` purges it. Re-running ingest on an unchanged corpus is a fast no-op — **idempotency is sacred**.
3. **Query time — citation integrity.** Before any citation is printed, re-triple-tap the cited source file. Match → `✓ verified`. Mismatch → `⚠ drifted since ingest (re-ingest advised)`. Missing → `⚠ missing`. Citations are promises; the checksum keeps them honest.

---

## 3. Division of labor — each organ does exactly one thing

| Organ | Its one job | What it stores |
|---|---|---|
| **llamafile / Gemma 3 12B** | The only component that understands language: embed chunks & queries; extract metadata as strict JSON; synthesize final answers | Nothing (stateless) |
| **TurboVec** | Answer one question: *"which chunkIDs are semantically nearest to this vector?"* | `{chunkID → quantized vector}` and nothing else |
| **QLever** | The memory palace: all structure and provenance. Documents, chunks (with full text as literals), tags, entities, relations, sequence links. Its integrated text index over chunk text doubles as the keyword/BM25-style retriever (`ql:contains-word` / `ql:contains-entity` — verify exact mechanics against the embedded build) | The entire graph + chunk text |
| **Orchestrator** | Process lifecycle, the manifest, pipelines, CLI, context budgeting | Manifest only: docID, paths, mtime, size, stage watermark, timestamps. No content. |

**Duplication policy:** chunk text lives in exactly one store (QLever — its text index needs the text anyway, which is precisely why keyword search lives there). Vectors live in exactly one store (TurboVec). The shared chunkID is the only thing that appears everywhere. This is the trinity's communion.

---

## 4. Ontology (`@prefix ch: <chimera://ontology#>`)

**Classes:** `ch:Document`, `ch:Chunk`, `ch:Entity`, `ch:Tag`, `ch:Relation`

```turtle
# Document
chimera://doc/{docID}
    a ch:Document ;
    ch:locatedAt "relative/path.md" ;        # 1..n — dedup across paths
    ch:checksum  "{docID}" ;
    ch:bytes     123456 ;
    ch:mime      "text/markdown" ;
    ch:ingestedAt "ISO-8601"^^xsd:dateTime ;
    ch:hasChunk  chimera://chunk/{docID}:0001 ;
    ch:supersededBy chimera://doc/{newID} .   # only on supersession

# Chunk
chimera://chunk/{chunkID}
    a ch:Chunk ;
    ch:partOf    chimera://doc/{docID} ;
    ch:ordinal   1 ;
    ch:byteStart 0 ; ch:byteEnd 2048 ;
    ch:text      "full chunk text as literal" ;
    ch:summary   "≤50-word model-written summary" ;
    ch:nextChunk chimera://chunk/{docID}:0002 ;
    ch:taggedWith chimera://tag/{slug} ;
    ch:mentions  chimera://entity/{slug} .

# Relation — a provenance-bearing receipt for every model-extracted claim
_:rel a ch:Relation ;
    ch:subject     chimera://entity/acme-corp ;
    ch:predicate   chimera://rel/acquired ;
    ch:object      chimera://entity/widgetco ;
    ch:evidencedBy chimera://chunk/{chunkID} ;
    ch:confidence  0.93 .

# ...AND assert the direct triple for cheap one-hop crawling:
chimera://entity/acme-corp  chimera://rel/acquired  chimera://entity/widgetco .
```

The direct triple makes graph crawling fast; the `ch:Relation` node is the receipt that lets any claim be traced back to the exact chunk (and therefore, via the spine, to a verified source file). Nothing the model asserts is allowed to float free of evidence.

**Slug canonicalization** (entities, tags): NFC → lowercase → trim → collapse whitespace → kebab-case. Embedding-based entity resolution is Phase 4 — do not build it now, do not preclude it.

---

## 5. Ingest pipeline — `chimera.ape ingest <dir>`

**Stage 0 — Lock & runtime.** Acquire an exclusive lock on the database dir (default `<dir>/.chimera`, override `--db`). First run: extract ZIP-store payloads into `db/runtime/`, version-stamped.

**Stage 1 — Walk.** Respect `.chimeraignore` (gitignore syntax) plus sane defaults: skip binaries (extension + magic-byte sniff), `.git`, `node_modules`, and the db dir itself. v1 ingests UTF-8 text: `txt`, `md`, source code, `csv`, `json`, `html` (tags stripped). Define an extractor interface now — `extract(path) → utf8 + structure hints` — so PDF and friends bolt on in Phase 4 without surgery.

**Stage 2 — Identity.** Triple-tap each file → docID. Consult the manifest: **new** (proceed), **unchanged** (skip — count it, touch nothing), **changed** (proceed + supersede old docID per §2.3).

**Stage 3 — Chunk.** Structure-aware splitting per mime type: headings and paragraph breaks for prose, function/class boundaries for code. Target 512–1024 tokens with ~15% overlap. Record byte offsets. Never split mid-sentence when avoidable. Assign ordinals → chunkIDs.

**Stage 4 — Embed.** Batch chunks to `/v1/embeddings`; L2-normalize; queue `{chunkID, vec}`.

**Stage 5 — Extract.** Per chunk, call Gemma with the extraction prompt (Appendix A) at temperature ≤ 0.2, JSON-only. Validate against the schema. On failure: one retry with the parse error appended; on second failure, write `ch:extractionFailed true` and move on. **Never stall the pipeline on one weird chunk.**

**Stage 6 — Write vectors.** Upsert into TurboVec (quantization per TurboVec defaults; expose its recall/speed knob in `--help`).

**Stage 7 — Write graph.** Emit triples as Turtle batches into QLever. Investigate the embedded build's preferred bulk path (file-based index build vs. SPARQL UPDATE); use bulk loading for initial ingest, updates thereafter.

**Stage 8 — Commit & report.** Write the manifest row; print the scoreboard: docs new / skipped / superseded, chunks, triples, vectors, wall time.

**Concurrency & resilience:** stages connected by bounded queues with backpressure; embedding batch size tuned to GPU/CPU reality; every stage idempotent keyed on chunkID; the manifest records a per-document stage watermark so a `kill -9` mid-ingest resumes cleanly with zero duplicates.

---

## 6. Search pipeline — `chimera.ape --search "Q"`

1. **Embed** Q through the same endpoint with the same normalization (shared vector space, §1).
2. **ANN:** TurboVec top-k (default `--k 24`) → `{chunkID, cosine}`.
3. **Keyword:** QLever text-index query over Q's salient terms → `{chunkID, score}`. Vector and keyword retrieval each run in the store built for them.
4. **Fuse:** Reciprocal Rank Fusion (k=60) → top-N candidates (default 12).
5. **Crawl — the graph earns its keep.** One batched SPARQL query (`VALUES`) per candidate set pulls: parent document (path, checksum), summary, tags, entities. Then expand to `--hops` (default 1): sibling chunks via `ch:nextChunk` (±1), co-mentioning chunks via shared entities, and `ch:Relation` receipts evidenced by candidates. **Stop-entity rule:** any entity mentioned in >5% of all chunks is a hub — never expand through it, or one mention of "the internet" drags in the whole corpus.
6. **Budget.** Assemble context under `--ctx-budget` tokens (default 8192): candidates first by fused rank, then expansion material by graph distance then similarity. Every included chunk carries its `path#ordinal` for citation.
7. **Synthesize.** Gemma with the synthesis prompt (Appendix B): answer *only* from context, inline `[n]` citations mapped to a source table.
8. **Verify.** Re-triple-tap every cited file; print `✓` / `⚠ drifted` / `⚠ missing` per citation (§2.3). The system would rather confess than fabricate.

**Output modes:** human (answer + Sources table with verification marks) and `--json` (full machine record: query, candidate scores, crawl trace, assembled context, answer, citations, verification results).

---

## 7. Runtime model

- **Lazy supervision.** Each subcommand starts only the organs it needs — `status` and `verify` start none; `ingest` and `--search` start all three.
- Children bind loopback ephemeral ports; the orchestrator passes ports via flags and polls health endpoints with exponential backoff until ready (llamafile exposes `/health`; confirm equivalents for QLever and TurboVec). A child crash aborts cleanly with that child's stderr tail in the error message.
- **Layout:** `.chimera/{manifest.db, turbovec/, qlever/, runtime/{bin,model}/, lock, config.toml, logs/}` — manifest in SQLite (fine under Cosmopolitan).
- **Signals:** trap INT/TERM → drain queues → flush manifest → TERM children, KILL after 5 s.

---

## 8. CLI

```
chimera.ape ingest <dir> [--db P] [--include G]... [--exclude G]... [--threads N] [--paranoid]
chimera.ape --search "Q" [--db P] [--k N] [--hops N] [--ctx-budget N] [--json]
chimera.ape verify  [--db P] [--fix]     # re-tap all docs, report drift; --fix re-ingests drifted
chimera.ape status  [--db P]             # counts, sizes, model, embedding dims, organ versions
chimera.ape vacuum  [--db P]             # purge superseded subtrees + tombstoned vectors
chimera.ape sparql "..." [--db P]        # power-user escape hatch straight into QLever

Global: --model PATH, --gpu auto|off|N, --verbose
        --version   # prints versions of all three organs, because of course it does
```

Positional sugar: `chimera.ape <dir>` ≡ `chimera.ape ingest <dir>`.

---

## 9. Internal API — build v1 against this; Phase 3 turns it into tools

```
retrieve.vector(text, k)        -> [Hit{chunkID, score}]
retrieve.keyword(text, k)       -> [Hit]
retrieve.fused(text, k)         -> [Hit]            # RRF of the two above
graph.about(chunkID)            -> ChunkCard        # doc, path, summary, tags, entities
graph.neighbors(iri, hops, caps)-> Subgraph
graph.sparql(query)             -> Rows             # read-only
content.text(chunkID)           -> str
provenance.verify(docID)        -> Verified | Drifted | Missing
llm.extract(chunkText)          -> ExtractionJSON
llm.synthesize(question, ctx)   -> AnswerJSON
```

The v1 CLI search composes these functions **linearly**. The Phase 3 agentic loop will compose them **dynamically**. Same functions, different conductor — design every signature with that in mind.

---

## 10. Phase 3 preview — the agentic loop (design for it, do not build it yet)

`--search "Q" --agentic [--steps 6]`: Gemma runs a ReAct loop over tools = `{retrieve.fused, graph.neighbors, graph.sparql, content.text}`. Loop contract: every step must either surface new evidence or terminate; hard step budget; the final answer flows through the *same* synthesis + verification path as v1, so agentic answers are exactly as honest as linear ones. Sub-query decomposition (vector probe here, graph walk there) is the whole point — §9 exists so this phase is a new conductor, not a rewrite.

---

## 11. Acceptance — v1 is done when

1. The same binary passes the test suite on Linux x86_64, macOS arm64, and Windows 11 — unmodified.
2. `file` confirms APE; nothing dynamically linked.
3. Ingesting a mixed md/code/txt corpus completes; an immediate re-run reports 100% skipped in under 2 s for a 1,000-document corpus (identity checks only).
4. Editing the *middle* of one file → only that document re-ingested; old node superseded; (and `--paranoid` catches an edit the triple-tap window missed).
5. `--search` returns a synthesized answer with ≥1 citation marked `✓ verified`; deleting a cited source then re-searching shows `⚠ missing` rather than lying.
6. `vacuum` after supersession measurably shrinks both stores; `sparql` still walks the live graph correctly.
7. `kill -9` mid-ingest, then re-run: completes with zero duplicate chunks, vectors, or triples.

---

## Appendix A — extraction prompt (Stage 5)

System message, temperature ≤ 0.2:

> You extract structured metadata from a text chunk. Respond with **JSON only** — no prose, no markdown fences. Schema:
> ```json
> {
>   "summary": "≤50 words",
>   "tags": ["3-8 kebab-case topical tags"],
>   "entities": [{"name": "string", "type": "person|org|place|concept|artifact|event"}],
>   "relations": [{"subject": "string", "predicate": "lower_snake_verb", "object": "string", "confidence": 0.0}]
> }
> ```
> Every relation's subject and object MUST appear in `entities`. Extract only what the text states or directly implies — no outside knowledge. If a field has nothing, return it empty.

Validate strictly; retry once with the parse error appended; then mark `ch:extractionFailed` and continue.

## Appendix B — synthesis prompt (search step 7)

> Answer the QUESTION using only the CONTEXT below. Cite every claim with `[n]` markers keyed to the numbered sources. If the context is insufficient, say so plainly and name what is missing — do not improvise. Be terse. No preamble.

## Appendix C — open decisions (resolve before coding the affected module)

- **TurboVec's exact API surface** — read its repository first; §3/§6 assume an upsert/query/tombstone interface.
- **QLever bulk-load mechanics** for the embedded build (initial index build vs. incremental SPARQL UPDATE).
- Default packaging: weights inside the ZIP store (true single file, huge binary) vs. sidecar `--model` (two files, sane downloads). Recommend: ship both release flavors.
- HTML ingestion in v1 or deferred to the Phase 4 extractor interface with PDF.
