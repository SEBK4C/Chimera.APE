// §3/§9 — the three organs as supervised children + typed HTTP clients.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "supervisor.h"

namespace chimera {

// Where the organ binaries and model live for this run (§7 layout).
// Resolution order per piece: explicit flag/env → db/runtime/{bin,model}/ →
// (task #10) extraction from the APE zip store into db/runtime/.
struct RuntimePaths {
  std::string llamafile;      // llamafile server binary
  std::string model;          // GGUF weights
  std::string mmproj;         // multimodal projector GGUF (optional: empty =
                              // text-only; media derivation/embedding off)
  std::string qlever_server;
  std::string qlever_index;   // index *builder* binary
  std::string turbovec;
  // Resolve from db_dir + overrides; empty fields mean "not found".
  static RuntimePaths resolve(const std::string& db_dir, const std::string& model_flag);
  std::optional<std::string> missing() const;  // first missing piece, if any
};

struct Hit {
  std::string chunk_id;  // resolved by caller via manifest for vector hits
  uint64_t vec_key = 0;
  double score = 0.0;
};

class LlamaClient {
 public:
  explicit LlamaClient(int port) : port_(port) {}
  // POST /v1/embeddings; returns one L2-normalized vector per input.
  std::optional<std::vector<std::vector<float>>> embed(const std::vector<std::string>& inputs);
  // POST /v1/chat/completions; returns message content (reasoning channel
  // dropped). temperature/max_tokens per Appendix A/B usage.
  std::optional<std::string> chat(const std::string& system, const std::string& user,
                                  double temperature, int max_tokens);
  // Chat with one attached media item (image or audio), e.g. for
  // transcription/description. mime e.g. "image/png", "audio/wav".
  std::optional<std::string> chat_media(const std::string& user,
                                        const std::string& media_b64,
                                        const std::string& mime,
                                        double temperature, int max_tokens);
  // Raw media embedding via {prompt_string: <media marker>, multimodal_data}.
  // Only available on the CPU backend (the server refuses with 501 on GPU).
  std::optional<std::vector<float>> embed_media(const std::string& media_b64);
  int port() const { return port_; }

 private:
  int port_;
  std::string media_marker_;  // cached from /props on first embed_media
};

class QleverClient {
 public:
  explicit QleverClient(int port) : port_(port) {}
  // application/sparql-query; returns raw SPARQL-results JSON text.
  std::optional<std::string> query(const std::string& sparql, int timeout_ms = 60000);
  // application/sparql-update (requires server --access-token match or -n).
  bool update(const std::string& sparql, const std::string& access_token = "");
  int port() const { return port_; }

 private:
  int port_;
};

class TurboVecClient {
 public:
  explicit TurboVecClient(int port) : port_(port) {}
  bool upsert(const std::vector<uint64_t>& ids,
              const std::vector<std::vector<float>>& vectors);
  // Single query vector → hits with vec_key + cosine score. A non-empty
  // allowlist restricts the search to those keys (same-modality search).
  std::optional<std::vector<Hit>> query(const std::vector<float>& vec, int k,
                                        const std::vector<uint64_t>& allowlist = {});
  bool remove(const std::vector<uint64_t>& ids);
  bool persist();
  // {dim (0 if unset), count}
  std::optional<std::pair<int, int64_t>> info();
  int port() const { return port_; }

 private:
  int port_;
};

// Lazy supervisor for all three organs (§7: each subcommand starts only what
// it needs). Start methods are idempotent.
class Organs {
 public:
  Organs(RuntimePaths paths, std::string db_dir)
      : paths_(std::move(paths)), db_dir_(std::move(db_dir)) {}

  // Each returns nullptr on failure with the reason in last_error().
  LlamaClient* llama();          // starts llamafile (slow: model load)
  QleverClient* qlever();        // starts qlever-server on db/qlever/chimera.*
  TurboVecClient* turbovec();    // starts turbovec-server on db/turbovec/index.tvim

  // True if a QLever index has been built (server can start).
  bool qlever_index_exists() const;
  // Run the index builder over a Turtle file (initial bulk load, §5 stage 7).
  // QLever's server must not be running against the same basename.
  bool build_qlever_index(const std::string& ttl_path, std::string* err);
  void stop_qlever();  // release index for rebuild

  void stop_all();
  const std::string& last_error() const { return error_; }

 private:
  RuntimePaths paths_;
  std::string db_dir_;
  std::string error_;
  Child llama_child_, qlever_child_, turbovec_child_;
  std::unique_ptr<LlamaClient> llama_client_;
  std::unique_ptr<QleverClient> qlever_client_;
  std::unique_ptr<TurboVecClient> turbovec_client_;
};

}  // namespace chimera
