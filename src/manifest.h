// The manifest (§3, §7): SQLite bookkeeping owned by the orchestrator.
// Stores docID, paths, mtime, size, stage watermark, timestamps — no content.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace chimera {

// Per-document stage watermark (§5). A document at stage N has durably
// completed every stage <= N; kill -9 resume restarts it at N+1.
enum class Stage : int {
  kIdentified = 0,
  kChunked = 1,
  kEmbedded = 2,
  kExtracted = 3,
  kVectorsWritten = 4,
  kGraphWritten = 5,
  kCommitted = 6,
};

struct PathInfo {
  std::string doc_id;
  int64_t mtime = 0;
  int64_t size = 0;
};

struct ChunkRow {
  std::string chunk_id;
  std::string doc_id;
  int ordinal = 0;          // 0 is reserved for a document's raw-media vector
  uint64_t vec_key = 0;
  int64_t byte_start = 0;
  int64_t byte_end = 0;
  std::string modality = "text";  // text | image | audio (vector space tag)
};

struct Scoreboard {
  int64_t docs = 0;
  int64_t chunks = 0;
  int64_t superseded = 0;
};

class Manifest {
 public:
  Manifest() = default;
  ~Manifest();
  Manifest(const Manifest&) = delete;
  Manifest& operator=(const Manifest&) = delete;

  // Opens (creating schema if needed). Throws std::runtime_error on failure.
  void open(const std::string& db_path);
  void close();

  // -- path layer (walk fast-skip) -----------------------------------------
  std::optional<PathInfo> path_info(const std::string& rel_path);
  void upsert_path(const std::string& rel_path, int64_t mtime, int64_t size,
                   const std::string& doc_id);
  std::vector<std::string> paths_of(const std::string& doc_id);

  // -- documents -------------------------------------------------------------
  bool has_doc(const std::string& doc_id);
  void insert_doc(const std::string& doc_id, int64_t bytes, const std::string& mime);
  void set_stage(const std::string& doc_id, Stage s);
  std::optional<int> stage_of(const std::string& doc_id);
  void supersede(const std::string& old_id, const std::string& new_id);
  // Documents superseded and not yet vacuumed.
  std::vector<std::string> superseded_docs();
  void forget_doc(const std::string& doc_id);  // vacuum: drop doc+chunks rows

  // -- chunks ----------------------------------------------------------------
  void insert_chunk(const ChunkRow& c);
  std::vector<ChunkRow> chunks_of(const std::string& doc_id);
  // Reverse lookup for search results coming back from TurboVec.
  std::optional<std::string> chunk_by_vec_key(uint64_t key);
  // All vector keys in a given modality (ordinal-0 media vectors for
  // image/audio) — the allowlist for same-modality search.
  std::vector<uint64_t> vec_keys_of_modality(const std::string& modality);

  // -- meta / stats ----------------------------------------------------------
  void set_meta(const std::string& key, const std::string& value);
  std::optional<std::string> get_meta(const std::string& key);
  Scoreboard scoreboard();

  // Wrap multi-statement mutations (one document's commit) atomically.
  void begin();
  void commit();
  void rollback();

 private:
  sqlite3* db_ = nullptr;
};

}  // namespace chimera
