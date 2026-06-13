// §5 — the ingest pipeline, stages 0–8.
//
// v1 processes documents sequentially through embed→extract→write; the
// stages are batched per document rather than wired through concurrent
// queues. Correctness knobs (idempotency, stage watermarks, kill -9 resume)
// are all here; the bounded-queue concurrency of §5 is a Phase-2 throughput
// upgrade that doesn't change any on-disk format.

#include <sys/file.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

#include "chunk.h"
#include "extract.h"
#include "hash.h"
#include "manifest.h"
#include "organs.h"
#include "pipeline.h"
#include "ttl.h"
#include "util.h"
#include "walk.h"

namespace fs = std::filesystem;

namespace chimera {

std::string resolve_db_dir(const Options& o) {
  if (!o.db.empty()) return o.db;
  if (!o.dir.empty()) return fs::absolute(o.dir).string() + "/.chimera";
  return fs::absolute(".chimera").string();
}

namespace {

int acquire_lock_or_die(const std::string& db_dir) {
  std::string lock_path = db_dir + "/lock";
  // O_CLOEXEC: without it the organ children inherit the lock fd and a
  // crashed orchestrator leaves the db locked by its own surviving organs.
  int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    std::fprintf(stderr, "chimera: database is locked (%s)\n", lock_path.c_str());
    std::exit(1);
  }
  return fd;
}

struct DocWork {
  std::string doc_id;
  std::string rel_path, abs_path;
  std::string mime;
  std::string modality = "text";  // image/audio docs derive text via the model
  int64_t size = 0, mtime = 0;
  std::string superseded_old;  // old docID this one replaces, if any
  std::vector<Chunk> chunks;   // for media docs, filled after derivation
};

constexpr int kEmbedBatch = 16;

// Derivation prompts (user-specified design): images transcribe-or-describe,
// audio transcribes. The derived text is what the text pipeline indexes; the
// raw media vector is stored separately for same-modality search (the
// modality gap makes raw cross-modal cosine useless — see
// vendor/llamafile-gemma/docs/mm-embedding.md).
const char* kImageDerivePrompt =
    "If this image contains readable text, transcribe it verbatim and "
    "completely. Otherwise, describe the scene concisely but thoroughly: "
    "name visible objects, people, places, text fragments, and any "
    "identifiable context. Output only the transcription or description.";
const char* kAudioDerivePrompt =
    "Transcribe this audio verbatim and completely. If it is not speech, "
    "describe the sound concisely (speakers, music, ambience, events). "
    "Output only the transcription or description.";

}  // namespace

int run_ingest(const Options& o) {
  if (o.dir.empty()) {
    std::fprintf(stderr, "chimera ingest: missing <dir>\n");
    return 2;
  }
  std::string root = fs::absolute(o.dir).string();
  if (!fs::is_directory(root)) {
    std::fprintf(stderr, "chimera: not a directory: %s\n", root.c_str());
    return 1;
  }

  // ---- Stage 0: lock & layout ----------------------------------------------
  std::string db_dir = resolve_db_dir(o);
  fs::create_directories(db_dir + "/staging");
  fs::create_directories(db_dir + "/logs");
  acquire_lock_or_die(db_dir);

  Manifest m;
  m.open(db_dir + "/manifest.db");

  auto t0 = std::chrono::steady_clock::now();
  int64_t n_skipped = 0, n_superseded = 0;
  std::vector<DocWork> work;

  // ---- Stages 1–3: walk, identity, chunk ------------------------------------
  WalkOptions wo;
  wo.include = o.include;
  wo.exclude = o.exclude;
  // A path is only safely skippable if its document actually finished the
  // pipeline — otherwise a doc that failed mid-stage (or died with the
  // process) would be path-fast-skipped forever.
  auto fully_ingested = [&](const std::string& doc_id) {
    auto st = m.stage_of(doc_id);
    return st && *st >= static_cast<int>(Stage::kCommitted);
  };
  // In-run dedup: the manifest only learns a docID once processing starts,
  // so identical files seen during one walk must dedup against the work
  // list itself or every copy embeds+extracts separately.
  std::set<std::string> enqueued;
  walk(root, wo, [&](const WalkEntry& e) {
    auto known = m.path_info(e.rel_path);
    if (known && known->mtime == e.mtime && known->size == e.size &&
        fully_ingested(known->doc_id)) {
      ++n_skipped;
      return;
    }
    auto id = triple_tap(e.abs_path, o.paranoid);
    if (!id) {
      std::fprintf(stderr, "  unreadable: %s\n", e.rel_path.c_str());
      return;
    }
    if (known && known->doc_id == *id && fully_ingested(*id)) {
      m.upsert_path(e.rel_path, e.mtime, e.size, *id);  // touched, unchanged
      ++n_skipped;
      return;
    }
    if (enqueued.count(*id) || (m.has_doc(*id) && fully_ingested(*id))) {
      // Dedup across paths (§2.3 job 1): one Document, many locatedAt.
      m.upsert_path(e.rel_path, e.mtime, e.size, *id);
      std::ofstream ttl(db_dir + "/staging/pending.ttl", std::ios::app);
      ttl << "<chimera://doc/" << *id << "> <chimera://ontology#locatedAt> \""
          << ttl_escape(e.rel_path) << "\" .\n";
      ++n_skipped;
      return;
    }

    DocWork w;
    w.doc_id = *id;
    w.rel_path = e.rel_path;
    w.abs_path = e.abs_path;
    w.mime = e.mime;
    w.modality = e.modality;
    w.size = e.size;
    w.mtime = e.mtime;
    if (known && known->doc_id != *id) {
      w.superseded_old = known->doc_id;
      ++n_superseded;
    }

    if (w.modality == "text") {
      auto content = read_file(e.abs_path);
      if (!content) return;
      std::string text = e.mime == "text/html" ? strip_html(*content) : *content;
      w.chunks = chunk_text(text, e.mime);
    }
    // Media chunks are derived once the model is up (needs the organ).
    enqueued.insert(*id);
    work.push_back(std::move(w));
  });

  // Fast no-op (§2.3 job 2): nothing new → never start an organ.
  if (work.empty()) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    std::printf("ingest: 0 new, %lld skipped, 0 superseded — unchanged corpus in %.2fs\n",
                static_cast<long long>(n_skipped), ms / 1000.0);
    return 0;
  }

  // ---- Organs up (llama + turbovec now; qlever at stage 7) ------------------
  RuntimePaths paths = RuntimePaths::resolve(db_dir, o.model);
  if (auto miss = paths.missing()) {
    std::fprintf(stderr, "chimera: missing runtime piece: %s\n", miss->c_str());
    return 1;
  }
  Organs organs(paths, db_dir, o.gpu);
  std::fprintf(stderr, "starting model server (first load can take minutes)...\n");
  LlamaClient* llama = organs.llama();
  if (!llama) {
    std::fprintf(stderr, "chimera: %s\n", organs.last_error().c_str());
    return 1;
  }
  TurboVecClient* tvec = organs.turbovec();
  if (!tvec) {
    std::fprintf(stderr, "chimera: %s\n", organs.last_error().c_str());
    return 1;
  }

  // Probe embedding dimensionality (§1: never hardcode).
  auto probe = llama->embed({"chimera dimension probe"});
  if (!probe || probe->empty()) {
    std::fprintf(stderr, "chimera: embedding probe failed\n");
    return 1;
  }
  int dim = static_cast<int>((*probe)[0].size());
  m.set_meta("embedding_dim", std::to_string(dim));
  if (auto info = tvec->info(); info && info->first > 0 && info->first != dim) {
    std::fprintf(stderr,
                 "chimera: vector store dim %d != model dim %d — refusing "
                 "(different model? pass the original via --model)\n",
                 info->first, dim);
    return 1;
  }
  if (o.verbose) std::fprintf(stderr, "embedding dim: %d\n", dim);

  // ---- Stages 4–6 per document ----------------------------------------------
  std::string run_ttl_path =
      db_dir + "/staging/run-" + std::to_string(::getpid()) + ".ttl";
  std::ofstream ttl(run_ttl_path, std::ios::app);
  ttl << ttl_prologue();

  int64_t n_new = 0, n_chunks = 0, n_failed_extractions = 0;

  for (DocWork& w : work) {
    // Resume guard: a doc whose watermark is already past graph write
    // (from a previous killed run) only needs its manifest commit.
    auto st = m.stage_of(w.doc_id);
    if (st && *st >= static_cast<int>(Stage::kCommitted)) continue;

    m.begin();
    m.insert_doc(w.doc_id, w.size, w.mime);
    m.upsert_path(w.rel_path, w.mtime, w.size, w.doc_id);
    if (!w.superseded_old.empty()) m.supersede(w.superseded_old, w.doc_id);
    m.commit();

    // Stage 3b — media derivation + raw-media vector (multimodal docs).
    std::vector<float> media_vec;
    if (w.modality != "text") {
      auto bytes = read_file(w.abs_path);
      if (!bytes) continue;
      std::string b64 = base64_encode(*bytes);
      const char* prompt = w.modality == "image" ? kImageDerivePrompt : kAudioDerivePrompt;
      auto derived = llama->chat_media(prompt, b64, w.mime, 0.1, 2048);
      if (!derived || derived->empty()) {
        std::fprintf(stderr, "  media derivation failed: %s (will retry next run)\n",
                     w.rel_path.c_str());
        continue;
      }
      w.chunks = chunk_text(*derived, "text/plain");
      auto mv = llama->embed_media(b64);
      if (mv) {
        media_vec = std::move(*mv);
      } else {
        // GPU backend refuses media embeddings (501); same-modality vector
        // search degrades gracefully — derived text still indexes fully.
        std::fprintf(stderr,
                     "  note: raw media embedding unavailable for %s "
                     "(GPU backend?); text bridge only\n",
                     w.rel_path.c_str());
      }
      if (o.verbose)
        std::fprintf(stderr, "  %s [%s]: derived %zu bytes of text\n",
                     w.rel_path.c_str(), w.modality.c_str(),
                     derived->size());
    }
    if (w.chunks.empty()) continue;  // nothing extractable

    // Stage 4 — embed (batched).
    std::vector<std::vector<float>> vecs;
    vecs.reserve(w.chunks.size());
    bool embed_ok = true;
    for (size_t i = 0; i < w.chunks.size() && embed_ok; i += kEmbedBatch) {
      std::vector<std::string> batch;
      for (size_t j = i; j < w.chunks.size() && j < i + kEmbedBatch; ++j)
        batch.push_back(w.chunks[j].text);
      auto got = llama->embed(batch);
      if (!got) {
        embed_ok = false;
        break;
      }
      for (auto& v : *got) vecs.push_back(std::move(v));
    }
    if (!embed_ok) {
      std::fprintf(stderr, "  embed failed: %s (will retry next run)\n",
                   w.rel_path.c_str());
      continue;  // watermark stays low; next run retries this doc
    }
    m.set_stage(w.doc_id, Stage::kEmbedded);

    // Stage 5 — extract (per chunk; never stall on one weird chunk).
    std::vector<std::optional<Extraction>> extractions(w.chunks.size());
    for (size_t i = 0; i < w.chunks.size(); ++i) {
      extractions[i] = extract_chunk(*llama, w.chunks[i].text);
      if (!extractions[i]) ++n_failed_extractions;
    }
    m.set_stage(w.doc_id, Stage::kExtracted);

    // Stage 6 — vectors. Text vectors for every chunk, plus (media docs)
    // the raw media vector under the reserved ordinal-0 chunkID.
    std::vector<uint64_t> keys;
    for (const auto& ck : w.chunks) keys.push_back(vec_key(chunk_id(w.doc_id, ck.ordinal)));
    if (!media_vec.empty()) {
      keys.push_back(vec_key(chunk_id(w.doc_id, 0)));
      vecs.push_back(media_vec);
    }
    if (!tvec->upsert(keys, vecs)) {
      std::fprintf(stderr, "  vector upsert failed: %s\n", w.rel_path.c_str());
      continue;
    }
    if (!media_vec.empty()) {
      ChunkRow media_row{chunk_id(w.doc_id, 0), w.doc_id, 0,
                         vec_key(chunk_id(w.doc_id, 0)), 0, w.size, w.modality};
      m.insert_chunk(media_row);
    }
    // Supersession: tombstone the old doc's vectors (§2.3 job 2).
    if (!w.superseded_old.empty()) {
      std::vector<uint64_t> old_keys;
      for (const auto& c : m.chunks_of(w.superseded_old)) old_keys.push_back(c.vec_key);
      if (!old_keys.empty()) tvec->remove(old_keys);
    }
    m.set_stage(w.doc_id, Stage::kVectorsWritten);

    // Stage 7 (emit) — Turtle for this document.
    DocTriples dt;
    dt.doc_id = w.doc_id;
    dt.paths = {w.rel_path};
    dt.bytes = w.size;
    dt.mime = w.mime;
    dt.modality = w.modality;
    dt.ingested_at = iso8601_now();
    dt.chunk_count = static_cast<int>(w.chunks.size());
    ttl << emit_doc(dt);
    if (!w.superseded_old.empty())
      ttl << "<chimera://doc/" << w.superseded_old
          << "> <chimera://ontology#supersededBy> <chimera://doc/" << w.doc_id
          << "> .\n";

    for (size_t i = 0; i < w.chunks.size(); ++i) {
      const Chunk& ck = w.chunks[i];
      std::string cid = chunk_id(w.doc_id, ck.ordinal);
      ChunkRow row{cid, w.doc_id, ck.ordinal, vec_key(cid), ck.byte_start, ck.byte_end};
      m.insert_chunk(row);

      ChunkTriples ct;
      ct.chunk_id = cid;
      ct.doc_id = w.doc_id;
      ct.ordinal = ck.ordinal;
      ct.byte_start = ck.byte_start;
      ct.byte_end = ck.byte_end;
      ct.text = ck.text;
      ct.has_next = ck.ordinal < static_cast<int>(w.chunks.size());
      if (extractions[i]) {
        ct.summary = extractions[i]->summary;
        ct.tags = extractions[i]->tags;
        ct.entities = extractions[i]->entities;
        ct.relations = extractions[i]->relations;
      } else {
        ct.extraction_failed = true;
      }
      ttl << emit_chunk(ct);
      ++n_chunks;
    }
    ++n_new;
    if (o.verbose)
      std::fprintf(stderr, "  %s: %zu chunks\n", w.rel_path.c_str(), w.chunks.size());
  }
  ttl.flush();
  if (!tvec->persist()) std::fprintf(stderr, "warning: vector persist failed\n");

  // ---- Stage 7 (load) — graph write -----------------------------------------
  // Initial corpus: bulk index build (text index included, -W -S bm25).
  // Incremental runs: SPARQL UPDATE against the running server. The from-
  // literals text index does not (verified risk, see DECISIONS.md D2) grow
  // through UPDATE; we mark it stale and `vacuum` rebuilds it.
  bool graph_ok = false;
  if (!organs.qlever_index_exists()) {
    // Concatenate every staged run file (durable across crashes; duplicate
    // triples are harmless set-semantics re-asserts).
    std::string all = db_dir + "/staging/_bulk.ttl";
    {
      std::ofstream out(all, std::ios::trunc);
      out << ttl_prologue();
      for (const auto& f : fs::directory_iterator(db_dir + "/staging"))
        if (f.is_regular_file() && f.path().extension() == ".ttl" &&
            f.path().filename() != "_bulk.ttl") {
          // NB: `out << in.rdbuf()` sets failbit on an EMPTY source, which
          // then silently drops every later file — and a kill -9 leaves an
          // empty run-<pid>.ttl that sorts first. Read explicitly instead.
          std::ifstream in(f.path(), std::ios::binary);
          std::string content((std::istreambuf_iterator<char>(in)), {});
          out << content;
        }
    }
    // Guard against building from a Turtle file that lost its triples (e.g.
    // the empty-source failbit bug above): if we wrote n_chunks chunks this
    // run, the bulk file must contain at least that many chunk triples.
    std::string err;
    int64_t ttl_chunks = 0;
    {
      std::ifstream in(all);
      std::string line;
      while (std::getline(in, line))
        if (line.find("a ch:Chunk ") != std::string::npos) ++ttl_chunks;
    }
    if (n_chunks > 0 && ttl_chunks < n_chunks) {
      std::fprintf(stderr,
                   "chimera: staged graph has %lld chunk triples but %lld were "
                   "written this run — refusing to build a truncated index\n",
                   static_cast<long long>(ttl_chunks), static_cast<long long>(n_chunks));
    } else if (!organs.build_qlever_index(all, &err)) {
      std::fprintf(stderr, "chimera: graph index build failed: %s\n", err.c_str());
    } else {
      graph_ok = true;
    }
  } else {
    QleverClient* q = organs.qlever();
    if (!q) {
      std::fprintf(stderr, "chimera: %s\n", organs.last_error().c_str());
    } else {
      // Replay every staged-but-unloaded Turtle file (this run's plus any
      // left behind by a killed run) as INSERT DATA. Our emitter uses only
      // the ch:/xsd: prefixes, declared on the UPDATE.
      std::string triples;
      for (const auto& f : fs::directory_iterator(db_dir + "/staging"))
        if (f.is_regular_file() && f.path().extension() == ".ttl") {
          std::ifstream in(f.path());
          triples.append(std::istreambuf_iterator<char>(in), {});
        }
      // Strip @prefix lines (SPARQL UPDATE uses PREFIX, not @prefix).
      std::string body;
      std::istringstream lines(triples);
      std::string line;
      while (std::getline(lines, line))
        if (line.rfind("@prefix", 0) != 0) body += line + "\n";
      std::string update =
          "PREFIX ch: <chimera://ontology#> "
          "PREFIX xsd: <http://www.w3.org/2001/XMLSchema#> "
          "INSERT DATA {\n" + body + "}";
      if (!q->update(update)) {
        std::fprintf(stderr, "chimera: SPARQL UPDATE failed (graph not updated)\n");
      } else {
        graph_ok = true;
        m.set_meta("text_index_stale", "1");  // cleared by vacuum's rebuild
      }
    }
  }

  // ---- Stage 8 — commit & report --------------------------------------------
  if (graph_ok) {
    for (const DocWork& w : work) {
      auto st = m.stage_of(w.doc_id);
      if (st && *st >= static_cast<int>(Stage::kVectorsWritten))
        m.set_stage(w.doc_id, Stage::kCommitted);
    }
    // Mark loaded staging files so the next bulk build doesn't double-load.
    fs::create_directories(db_dir + "/staging/loaded");
    for (const auto& f : fs::directory_iterator(db_dir + "/staging"))
      if (f.is_regular_file() && f.path().extension() == ".ttl")
        fs::rename(f.path(), db_dir + "/staging/loaded/" + f.path().filename().string());
  }

  organs.stop_all();

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
  std::printf(
      "ingest: %lld new, %lld skipped, %lld superseded — %lld chunks, "
      "%lld failed extractions, graph %s, in %.1fs\n",
      static_cast<long long>(n_new), static_cast<long long>(n_skipped),
      static_cast<long long>(n_superseded), static_cast<long long>(n_chunks),
      static_cast<long long>(n_failed_extractions),
      graph_ok ? "ok" : "FAILED", ms / 1000.0);
  return graph_ok ? 0 : 1;
}

}  // namespace chimera
