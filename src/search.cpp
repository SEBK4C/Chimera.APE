// §6 — the search pipeline: embed → ANN + keyword → RRF → crawl → budget →
// synthesize → verify. Plus the small commands (status/verify/vacuum/sparql).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

#include "nlohmann/json.hpp"

#include "extract.h"
#include "hash.h"
#include "manifest.h"
#include "organs.h"
#include "pipeline.h"
#include "ttl.h"
#include "util.h"
#include "walk.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace chimera {

namespace {

// ---- SPARQL result helpers --------------------------------------------------

// Rows of {var → value} from SPARQL-results JSON.
std::vector<std::map<std::string, std::string>> rows_of(const std::string& body) {
  std::vector<std::map<std::string, std::string>> out;
  try {
    json j = json::parse(body);
    for (const auto& b : j.at("results").at("bindings")) {
      std::map<std::string, std::string> row;
      for (auto it = b.begin(); it != b.end(); ++it)
        row[it.key()] = it.value().value("value", "");
      out.push_back(std::move(row));
    }
  } catch (const std::exception&) {
  }
  return out;
}

std::string chunk_id_from_iri(const std::string& iri) {
  const std::string p = "chimera://chunk/";
  auto at = iri.find(p);
  return at == std::string::npos ? "" : iri.substr(at + p.size());
}

std::string values_block(const std::vector<std::string>& chunk_ids) {
  std::string v = "VALUES ?chunk { ";
  for (const auto& c : chunk_ids) v += "<chimera://chunk/" + c + "> ";
  v += "}";
  return v;
}

// ---- keyword query ------------------------------------------------------------

const std::set<std::string> kStop = {
    "the", "and", "for", "are", "but", "not", "with", "this", "that", "from",
    "have", "has", "was", "were", "will", "would", "could", "should", "what",
    "when", "where", "which", "who", "how", "why", "does", "did", "their",
    "there", "about", "into", "than", "then", "them", "they", "its", "his",
    "her", "you", "your", "can", "all", "any", "one", "out", "use", "used"};

std::vector<std::string> salient_terms(const std::string& q, size_t max_terms = 6) {
  std::vector<std::string> terms;
  std::string cur;
  auto flush = [&] {
    if (cur.size() >= 3 && !kStop.count(cur)) terms.push_back(cur);
    cur.clear();
  };
  for (unsigned char c : q) {
    if (std::isalnum(c)) cur.push_back(static_cast<char>(std::tolower(c)));
    else flush();
  }
  flush();
  std::sort(terms.begin(), terms.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
  terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
  if (terms.size() > max_terms) terms.resize(max_terms);
  return terms;
}

// ---- context assembly ---------------------------------------------------------

struct Candidate {
  std::string chunk_id;
  double fused = 0;        // RRF score
  double cosine = 0;       // from ANN, 0 if keyword-only
  int graph_distance = 0;  // 0 = retrieved, 1 = expansion
  std::string doc_id, path, summary, text;
  std::vector<std::string> entities;
};

constexpr int kBytesPerToken = 4;

}  // namespace

int run_search(const Options& o) {
  std::string db_dir = resolve_db_dir(o);
  if (!fs::exists(db_dir + "/manifest.db")) {
    std::fprintf(stderr, "chimera: no database at %s (ingest first)\n", db_dir.c_str());
    return 1;
  }
  Manifest m;
  m.open(db_dir + "/manifest.db");

  RuntimePaths paths = RuntimePaths::resolve(db_dir, o.model);
  if (auto miss = paths.missing()) {
    std::fprintf(stderr, "chimera: missing runtime piece: %s\n", miss->c_str());
    return 1;
  }
  Organs organs(paths, db_dir);
  std::fprintf(stderr, "starting organs...\n");
  LlamaClient* llama = organs.llama();
  TurboVecClient* tvec = llama ? organs.turbovec() : nullptr;
  QleverClient* q = tvec ? organs.qlever() : nullptr;
  if (!llama || !tvec || !q) {
    std::fprintf(stderr, "chimera: %s\n", organs.last_error().c_str());
    return 1;
  }
  if (m.get_meta("text_index_stale"))
    std::fprintf(stderr,
                 "note: keyword index is stale (incremental ingests since "
                 "last build); run `chimera vacuum` to rebuild\n");

  // ---- 0b. media query? Derive text + same-modality vector hits --------------
  // The modality gap (docs/DECISIONS.md, mm-embedding.md) means a raw image/
  // audio vector only ranks meaningfully against vectors of the SAME
  // modality; the derived transcription/description bridges into text space.
  std::string query_text = o.query;
  std::vector<std::pair<std::string, double>> media_ranked;  // chunkID, cosine
  if (!o.search_file.empty()) {
    auto bytes = read_file(o.search_file);
    if (!bytes) {
      std::fprintf(stderr, "chimera: cannot read %s\n", o.search_file.c_str());
      return 1;
    }
    std::string modality = modality_of(o.search_file);
    if (modality == "text") {
      query_text = *bytes;  // plain text file as query
    } else {
      std::string b64 = base64_encode(*bytes);
      std::string mime = mime_of(o.search_file);
      const char* prompt = modality == "image"
          ? "If this image contains readable text, transcribe it verbatim. "
            "Otherwise describe the scene concisely. Output only that."
          : "Transcribe this audio verbatim. If not speech, describe the "
            "sound concisely. Output only that.";
      std::fprintf(stderr, "deriving query text from %s...\n", modality.c_str());
      auto derived = llama->chat_media(prompt, b64, mime, 0.1, 2048);
      if (derived && !derived->empty()) {
        query_text = o.query.empty() ? *derived : o.query + "\n" + *derived;
      } else if (o.query.empty()) {
        std::fprintf(stderr, "chimera: media derivation failed and no --search text given\n");
        return 1;
      }
      auto allow = m.vec_keys_of_modality(modality);
      if (!allow.empty()) {
        if (auto mv = llama->embed_media(b64)) {
          if (auto hits = tvec->query(*mv, o.k, allow))
            for (const auto& h : *hits)
              if (auto cid = m.chunk_by_vec_key(h.vec_key))
                media_ranked.push_back({*cid, h.score});
        }
      }
    }
  }

  // ---- 1. embed the query ----------------------------------------------------
  auto qe = llama->embed({query_text});
  if (!qe) {
    std::fprintf(stderr, "chimera: query embedding failed\n");
    return 1;
  }

  // ---- 2. ANN ------------------------------------------------------------------
  // Text-modality vectors only: media vectors live in their own similarity
  // regime and would pollute a text query's neighborhood.
  auto text_allow = m.vec_keys_of_modality("text");
  auto ann = tvec->query((*qe)[0], o.k, text_allow);
  std::vector<std::pair<std::string, double>> vec_ranked;  // chunkID, cosine
  if (ann)
    for (const auto& h : *ann)
      if (auto cid = m.chunk_by_vec_key(h.vec_key)) vec_ranked.push_back({*cid, h.score});

  // ---- 3. keyword ---------------------------------------------------------------
  std::map<std::string, double> kw_scores;  // chunkID → best BM25
  for (const auto& term : salient_terms(query_text)) {
    std::string sparql =
        "PREFIX ch: <chimera://ontology#> "
        "SELECT ?chunk ?ql_score_t_var_text WHERE { "
        "?t ql:contains-word \"" + term + "*\" . "
        "?t ql:contains-entity ?text . "
        "?chunk ch:text ?text . } "
        "ORDER BY DESC(?ql_score_t_var_text) LIMIT " + std::to_string(o.k);
    auto res = q->query(sparql);
    if (!res) continue;
    for (const auto& row : rows_of(*res)) {
      std::string cid = chunk_id_from_iri(row.count("chunk") ? row.at("chunk") : "");
      if (cid.empty()) continue;
      double s = row.count("ql_score_t_var_text") ? std::atof(row.at("ql_score_t_var_text").c_str()) : 0;
      auto it = kw_scores.find(cid);
      if (it == kw_scores.end() || s > it->second) kw_scores[cid] = s;
    }
  }
  std::vector<std::pair<std::string, double>> kw_ranked(kw_scores.begin(), kw_scores.end());
  std::sort(kw_ranked.begin(), kw_ranked.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  // ---- 4. RRF fuse (k=60) --------------------------------------------------------
  constexpr double kRrf = 60.0;
  std::map<std::string, Candidate> cands;
  for (size_t i = 0; i < vec_ranked.size(); ++i) {
    auto& c = cands[vec_ranked[i].first];
    c.chunk_id = vec_ranked[i].first;
    c.fused += 1.0 / (kRrf + static_cast<double>(i) + 1);
    c.cosine = vec_ranked[i].second;
  }
  for (size_t i = 0; i < kw_ranked.size(); ++i) {
    auto& c = cands[kw_ranked[i].first];
    c.chunk_id = kw_ranked[i].first;
    c.fused += 1.0 / (kRrf + static_cast<double>(i) + 1);
  }
  // Same-modality media hits: an ordinal-0 hit stands for its document;
  // route it to the doc's first text chunk (which carries the derived text).
  for (size_t i = 0; i < media_ranked.size(); ++i) {
    std::string cid = media_ranked[i].first;
    auto colon = cid.rfind(':');
    if (colon != std::string::npos) cid = cid.substr(0, colon) + ":0001";
    auto& c = cands[cid];
    c.chunk_id = cid;
    c.fused += 1.0 / (kRrf + static_cast<double>(i) + 1);
    if (c.cosine == 0) c.cosine = media_ranked[i].second;
  }
  std::vector<Candidate> top;
  for (auto& [id, c] : cands) top.push_back(std::move(c));
  std::sort(top.begin(), top.end(),
            [](const auto& a, const auto& b) { return a.fused > b.fused; });
  constexpr size_t kTopN = 12;
  if (top.size() > kTopN) top.resize(kTopN);
  if (top.empty()) {
    if (o.json) std::puts(R"({"answer":null,"reason":"no candidates"})");
    else std::puts("No matching content found.");
    return 0;
  }

  // ---- 5. crawl -------------------------------------------------------------------
  std::vector<std::string> ids;
  for (const auto& c : top) ids.push_back(c.chunk_id);

  // 5a. cards for candidates.
  auto fetch_cards = [&](const std::vector<std::string>& chunk_ids)
      -> std::map<std::string, Candidate> {
    std::map<std::string, Candidate> out;
    if (chunk_ids.empty()) return out;
    std::string sparql =
        "PREFIX ch: <chimera://ontology#> "
        "SELECT ?chunk ?doc ?path ?text ?summary WHERE { " +
        values_block(chunk_ids) +
        " ?chunk ch:partOf ?doc ; ch:text ?text . ?doc ch:locatedAt ?path . "
        "OPTIONAL { ?chunk ch:summary ?summary } }";
    auto res = q->query(sparql);
    if (!res) return out;
    for (const auto& row : rows_of(*res)) {
      std::string cid = chunk_id_from_iri(row.at("chunk"));
      Candidate& c = out[cid];
      if (!c.chunk_id.empty()) continue;  // first path wins
      c.chunk_id = cid;
      const std::string dp = "chimera://doc/";
      c.doc_id = row.at("doc").substr(row.at("doc").find(dp) + dp.size());
      c.path = row.at("path");
      c.text = row.at("text");
      if (row.count("summary")) c.summary = row.at("summary");
    }
    return out;
  };

  auto cards = fetch_cards(ids);
  for (auto& c : top)
    if (cards.count(c.chunk_id)) {
      auto& k = cards[c.chunk_id];
      c.doc_id = k.doc_id;
      c.path = k.path;
      c.text = k.text;
      c.summary = k.summary;
    }

  // 5b. expansion set (hops=1): siblings + co-mentions + relation receipts.
  std::set<std::string> expansion;
  std::vector<std::string> receipts;  // human-readable relation lines
  if (o.hops > 0) {
    // Siblings via ch:nextChunk (±1).
    auto res = q->query(
        "PREFIX ch: <chimera://ontology#> SELECT ?chunk ?next ?prev WHERE { " +
        values_block(ids) +
        " OPTIONAL { ?chunk ch:nextChunk ?next } OPTIONAL { ?prev ch:nextChunk ?chunk } }");
    if (res)
      for (const auto& row : rows_of(*res)) {
        for (const char* k : {"next", "prev"})
          if (row.count(k)) {
            std::string cid = chunk_id_from_iri(row.at(k));
            if (!cid.empty() && !cands.count(cid)) expansion.insert(cid);
          }
      }

    // Stop-entity rule: hubs mentioned by >5% of all chunks never expand.
    int64_t total_chunks = m.scoreboard().chunks;
    double hub_cut = std::max(2.0, 0.05 * static_cast<double>(total_chunks));
    res = q->query(
        "PREFIX ch: <chimera://ontology#> SELECT ?ent (COUNT(?c) AS ?n) WHERE { "
        "?c ch:mentions ?ent } GROUP BY ?ent");
    std::set<std::string> hubs;
    if (res)
      for (const auto& row : rows_of(*res))
        if (std::atof(row.at("n").c_str()) > hub_cut) hubs.insert(row.at("ent"));

    // Co-mentioning chunks through non-hub entities.
    res = q->query(
        "PREFIX ch: <chimera://ontology#> SELECT ?ent ?other WHERE { " +
        values_block(ids) +
        " ?chunk ch:mentions ?ent . ?other ch:mentions ?ent . "
        "FILTER(?other != ?chunk) } LIMIT 200");
    if (res)
      for (const auto& row : rows_of(*res)) {
        if (hubs.count(row.at("ent"))) continue;
        std::string cid = chunk_id_from_iri(row.at("other"));
        if (!cid.empty() && !cands.count(cid)) expansion.insert(cid);
      }

    // Relation receipts evidenced by candidates.
    res = q->query(
        "PREFIX ch: <chimera://ontology#> SELECT ?s ?p ?obj ?conf WHERE { " +
        values_block(ids) +
        " ?r a ch:Relation ; ch:evidencedBy ?chunk ; ch:subject ?s ; "
        "ch:predicate ?p ; ch:object ?obj ; ch:confidence ?conf }");
    if (res)
      for (const auto& row : rows_of(*res)) {
        auto tail = [](const std::string& iri) {
          auto sl = iri.find_last_of('/');
          return sl == std::string::npos ? iri : iri.substr(sl + 1);
        };
        receipts.push_back(tail(row.at("s")) + " —" + tail(row.at("p")) + "→ " +
                           tail(row.at("obj")) + " (conf " + row.at("conf") + ")");
      }
  }

  std::vector<std::string> exp_ids(expansion.begin(), expansion.end());
  auto exp_cards = fetch_cards(exp_ids);

  // ---- 6. budget ------------------------------------------------------------------
  int64_t budget_bytes = static_cast<int64_t>(o.ctx_budget) * kBytesPerToken;
  struct Source {
    std::string chunk_id, path, text;
    int ordinal;
    std::string doc_id;
  };
  std::vector<Source> sources;
  auto try_add = [&](const Candidate& c) {
    if (budget_bytes - static_cast<int64_t>(c.text.size()) < 0) return;
    budget_bytes -= static_cast<int64_t>(c.text.size());
    int ordinal = 0;
    auto colon = c.chunk_id.find(':');
    if (colon != std::string::npos) ordinal = std::atoi(c.chunk_id.c_str() + colon + 1);
    sources.push_back({c.chunk_id, c.path, c.text, ordinal, c.doc_id});
  };
  for (const auto& c : top) try_add(c);                       // fused rank first
  for (const auto& [id, c] : exp_cards) try_add(c);           // then expansion

  // ---- 7. synthesize ----------------------------------------------------------------
  std::ostringstream ctx;
  ctx << "QUESTION: " << query_text << "\n\nCONTEXT:\n";
  for (size_t i = 0; i < sources.size(); ++i)
    ctx << "[" << i + 1 << "] (" << sources[i].path << "#" << sources[i].ordinal
        << ")\n" << sources[i].text << "\n\n";
  if (!receipts.empty()) {
    ctx << "KNOWN RELATIONS (from the knowledge graph):\n";
    for (const auto& r : receipts) ctx << "- " << r << "\n";
  }
  auto answer = llama->chat(kSynthesisSystemPrompt, ctx.str(), 0.7, 2048);
  if (!answer || answer->empty()) {
    std::fprintf(stderr, "chimera: synthesis failed\n");
    return 1;
  }

  // ---- 8. verify cited sources --------------------------------------------------------
  // Cited = [n] markers present in the answer; fall back to all sources if
  // the model cited nothing (it was told to).
  std::set<size_t> cited;
  for (size_t i = 0; i + 2 < answer->size(); ++i)
    if ((*answer)[i] == '[') {
      int n = std::atoi(answer->c_str() + i + 1);
      if (n >= 1 && static_cast<size_t>(n) <= sources.size()) cited.insert(n - 1);
    }
  if (cited.empty())
    for (size_t i = 0; i < sources.size(); ++i) cited.insert(i);

  std::string corpus_root = fs::path(db_dir).parent_path().string();
  struct Verdict {
    std::string mark, path;
    int ordinal;
  };
  std::vector<Verdict> verdicts;
  for (size_t i : cited) {
    const Source& s = sources[i];
    std::string abs = corpus_root + "/" + s.path;
    std::string mark;
    if (!fs::exists(abs)) {
      mark = "⚠ missing";
    } else {
      auto tap = triple_tap(abs);
      mark = (tap && *tap == s.doc_id) ? "✓ verified"
                                       : "⚠ drifted since ingest (re-ingest advised)";
    }
    verdicts.push_back({mark, s.path, s.ordinal});
  }

  organs.stop_all();

  // ---- output -----------------------------------------------------------------------
  if (o.json) {
    json out;
    out["query"] = o.query;
    out["answer"] = *answer;
    out["candidates"] = json::array();
    for (const auto& c : top)
      out["candidates"].push_back({{"chunk", c.chunk_id},
                                   {"fused", c.fused},
                                   {"cosine", c.cosine},
                                   {"path", c.path}});
    out["sources"] = json::array();
    for (size_t i = 0; i < sources.size(); ++i)
      out["sources"].push_back({{"n", i + 1},
                                {"chunk", sources[i].chunk_id},
                                {"path", sources[i].path},
                                {"ordinal", sources[i].ordinal}});
    out["verification"] = json::array();
    for (size_t vi = 0; vi < verdicts.size(); ++vi) {
      auto it = cited.begin();
      std::advance(it, vi);
      out["verification"].push_back({{"n", *it + 1},
                                     {"path", verdicts[vi].path},
                                     {"status", verdicts[vi].mark}});
    }
    out["relations"] = receipts;
    std::puts(out.dump(2).c_str());
  } else {
    std::printf("\n%s\n\nSources:\n", answer->c_str());
    for (size_t i = 0; i < sources.size(); ++i) {
      std::string mark = "(uncited)";
      size_t vi = 0;
      for (size_t ci : cited) {
        if (ci == i) mark = verdicts[vi].mark;
        ++vi;
      }
      std::printf("  [%zu] %s#%d  %s\n", i + 1, sources[i].path.c_str(),
                  sources[i].ordinal, mark.c_str());
    }
  }
  return 0;
}

// ---- small commands ------------------------------------------------------------

int run_status(const Options& o) {
  std::string db_dir = resolve_db_dir(o);
  if (!fs::exists(db_dir + "/manifest.db")) {
    std::fprintf(stderr, "chimera: no database at %s\n", db_dir.c_str());
    return 1;
  }
  Manifest m;
  m.open(db_dir + "/manifest.db");
  auto sc = m.scoreboard();
  std::printf("db:         %s\n", db_dir.c_str());
  std::printf("documents:  %lld\n", static_cast<long long>(sc.docs));
  std::printf("chunks:     %lld\n", static_cast<long long>(sc.chunks));
  std::printf("superseded: %lld (pending vacuum)\n", static_cast<long long>(sc.superseded));
  if (auto dim = m.get_meta("embedding_dim"))
    std::printf("embedding:  %s dims\n", dim->c_str());
  if (m.get_meta("text_index_stale"))
    std::printf("note:       keyword index stale; vacuum to rebuild\n");
  return 0;
}

int run_verify(const Options& o) {
  std::string db_dir = resolve_db_dir(o);
  Manifest m;
  m.open(db_dir + "/manifest.db");
  std::string corpus_root = fs::path(db_dir).parent_path().string();

  // Verify every live path in the manifest. (Path table is the source of
  // truth for "what should exist where".)
  int ok = 0, drifted = 0, missing = 0;
  std::vector<std::string> drifted_paths;
  // Reuse walk over manifest contents via doc list: simplest exhaustive scan.
  // We iterate paths through superseded-free docs.
  // Manifest lacks an all-paths accessor; go through scoreboard docs via SQL
  // would be cleaner — use chunks_of's doc set instead.
  // Pragmatic: open a second connection? Add accessor later; for now use
  // paths_of over docs found via superseded_docs() complement is awkward —
  // walk the corpus instead and compare.
  WalkOptions wo;
  walk(corpus_root, wo, [&](const WalkEntry& e) {
    auto known = m.path_info(e.rel_path);
    if (!known) return;  // never ingested; not a verification target
    auto tap = triple_tap(e.abs_path, o.paranoid);
    if (!tap) {
      ++missing;
      std::printf("⚠ unreadable  %s\n", e.rel_path.c_str());
    } else if (*tap == known->doc_id) {
      ++ok;
    } else {
      ++drifted;
      drifted_paths.push_back(e.rel_path);
      std::printf("⚠ drifted     %s\n", e.rel_path.c_str());
    }
  });
  std::printf("verify: %d ok, %d drifted, %d unreadable\n", ok, drifted, missing);
  if (o.fix && drifted > 0) {
    std::printf("--fix: re-ingest drifted documents with `chimera ingest <dir>` "
                "(drift re-detection is automatic)\n");
    Options ingest_opts = o;
    ingest_opts.dir = corpus_root;
    return run_ingest(ingest_opts);
  }
  return drifted == 0 && missing == 0 ? 0 : 1;
}

int run_vacuum(const Options& o) {
  std::string db_dir = resolve_db_dir(o);
  Manifest m;
  m.open(db_dir + "/manifest.db");

  RuntimePaths paths = RuntimePaths::resolve(db_dir, o.model);
  Organs organs(paths, db_dir);

  // 1. Drop superseded docs' vectors (tombstoned already at supersession;
  //    re-remove is a no-op) and manifest rows.
  auto dead = m.superseded_docs();
  TurboVecClient* tvec = nullptr;
  if (!dead.empty()) {
    tvec = organs.turbovec();
    if (!tvec) {
      std::fprintf(stderr, "chimera: %s\n", organs.last_error().c_str());
      return 1;
    }
  }
  int64_t purged_chunks = 0;
  for (const auto& d : dead) {
    std::vector<uint64_t> keys;
    for (const auto& c : m.chunks_of(d)) keys.push_back(c.vec_key);
    if (tvec && !keys.empty()) tvec->remove(keys);
    purged_chunks += static_cast<int64_t>(keys.size());
    m.forget_doc(d);
  }
  if (tvec) tvec->persist();

  // 2. Rebuild the graph index from live triples only: dump everything
  //    reachable from non-superseded documents via CONSTRUCT, then re-index.
  //    v1 simplification: re-emit Turtle from loaded staging files minus
  //    superseded docs is fragile; instead CONSTRUCT the full live graph.
  if (organs.qlever_index_exists()) {
    QleverClient* q = organs.qlever();
    if (!q) {
      std::fprintf(stderr, "chimera: %s\n", organs.last_error().c_str());
      return 1;
    }
    // Two-step CONSTRUCT: everything except subtrees of superseded docs.
    std::string cons =
        "PREFIX ch: <chimera://ontology#> CONSTRUCT { ?s ?p ?o } WHERE { "
        "?s ?p ?o . "
        "FILTER NOT EXISTS { ?s ch:supersededBy ?x } "
        "FILTER NOT EXISTS { ?s ch:partOf ?d . ?d ch:supersededBy ?x2 } "
        "FILTER NOT EXISTS { ?s ch:evidencedBy ?c . ?c ch:partOf ?d2 . ?d2 ch:supersededBy ?x3 } }";
    auto turtle = q->query(cons, /*timeout_ms=*/600000);
    if (!turtle) {
      std::fprintf(stderr, "chimera: vacuum CONSTRUCT failed\n");
      return 1;
    }
    organs.stop_qlever();
    std::string dump = db_dir + "/staging/_vacuum.ttl";
    {
      std::ofstream out(dump, std::ios::trunc);
      out << *turtle;
    }
    // Move the old index aside, rebuild fresh (also de-stales the text index).
    fs::remove_all(db_dir + "/qlever.old");
    if (fs::exists(db_dir + "/qlever")) fs::rename(db_dir + "/qlever", db_dir + "/qlever.old");
    std::string err;
    if (!organs.build_qlever_index(dump, &err)) {
      std::fprintf(stderr, "chimera: vacuum rebuild failed: %s\n", err.c_str());
      fs::remove_all(db_dir + "/qlever");
      fs::rename(db_dir + "/qlever.old", db_dir + "/qlever");
      return 1;
    }
    fs::remove_all(db_dir + "/qlever.old");
    // The rebuilt index subsumes all loaded staging history.
    fs::remove_all(db_dir + "/staging/loaded");
    fs::create_directories(db_dir + "/staging/loaded");
    m.set_meta("text_index_stale", "");
  }
  organs.stop_all();
  std::printf("vacuum: purged %zu superseded documents (%lld chunks)\n",
              dead.size(), static_cast<long long>(purged_chunks));
  return 0;
}

int run_sparql(const Options& o) {
  std::string db_dir = resolve_db_dir(o);
  RuntimePaths paths = RuntimePaths::resolve(db_dir, o.model);
  Organs organs(paths, db_dir);
  QleverClient* q = organs.qlever();
  if (!q) {
    std::fprintf(stderr, "chimera: %s\n", organs.last_error().c_str());
    return 1;
  }
  auto res = q->query(o.sparql_text, /*timeout_ms=*/600000);
  if (!res) {
    std::fprintf(stderr, "chimera: query failed\n");
    return 1;
  }
  std::puts(res->c_str());
  organs.stop_all();
  return 0;
}

}  // namespace chimera
