#include "manifest.h"

#include <stdexcept>

#include "sqlite3.h"

namespace chimera {

namespace {

[[noreturn]] void fail(sqlite3* db, const std::string& what) {
  throw std::runtime_error(what + ": " + (db ? sqlite3_errmsg(db) : "no db"));
}

// Tiny RAII statement wrapper; the manifest is low-QPS bookkeeping, so we
// prepare per call instead of caching statements.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK)
      fail(db, std::string("prepare ") + sql);
  }
  ~Stmt() { sqlite3_finalize(st_); }
  Stmt& bind(int i, const std::string& v) {
    sqlite3_bind_text(st_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Stmt& bind(int i, int64_t v) {
    sqlite3_bind_int64(st_, i, v);
    return *this;
  }
  Stmt& bind_u64(int i, uint64_t v) {
    sqlite3_bind_int64(st_, i, static_cast<int64_t>(v));  // bit-preserving
    return *this;
  }
  bool row() {
    int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    fail(db_, "step");
  }
  void exec() {
    if (row()) fail(db_, "unexpected row");
  }
  std::string col_text(int i) {
    const unsigned char* t = sqlite3_column_text(st_, i);
    return t ? reinterpret_cast<const char*>(t) : "";
  }
  int64_t col_i64(int i) { return sqlite3_column_int64(st_, i); }

 private:
  sqlite3* db_;
  sqlite3_stmt* st_ = nullptr;
};

const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS doc(
  docid TEXT PRIMARY KEY,
  bytes INTEGER NOT NULL,
  mime  TEXT NOT NULL,
  stage INTEGER NOT NULL DEFAULT 0,
  ingested_at TEXT NOT NULL,
  superseded_by TEXT,
  vacuumed INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS path(
  relpath TEXT PRIMARY KEY,
  mtime INTEGER NOT NULL,
  size  INTEGER NOT NULL,
  docid TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS path_doc ON path(docid);
CREATE TABLE IF NOT EXISTS chunk(
  chunkid TEXT PRIMARY KEY,
  docid TEXT NOT NULL,
  ordinal INTEGER NOT NULL,
  veckey INTEGER NOT NULL UNIQUE,
  byte_start INTEGER NOT NULL,
  byte_end INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS chunk_doc ON chunk(docid);
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
)sql";

}  // namespace

Manifest::~Manifest() { close(); }

void Manifest::open(const std::string& db_path) {
  if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) fail(db_, "open " + db_path);
  char* err = nullptr;
  // WAL keeps readers (status) unblocked during ingest writes.
  if (sqlite3_exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;",
                   nullptr, nullptr, &err) != SQLITE_OK ||
      sqlite3_exec(db_, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "schema";
    sqlite3_free(err);
    throw std::runtime_error("manifest init: " + msg);
  }
}

void Manifest::close() {
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
}

std::optional<PathInfo> Manifest::path_info(const std::string& rel_path) {
  Stmt s(db_, "SELECT docid, mtime, size FROM path WHERE relpath=?1");
  s.bind(1, rel_path);
  if (!s.row()) return std::nullopt;
  return PathInfo{s.col_text(0), s.col_i64(1), s.col_i64(2)};
}

void Manifest::upsert_path(const std::string& rel_path, int64_t mtime, int64_t size,
                           const std::string& doc_id) {
  Stmt s(db_,
         "INSERT INTO path(relpath,mtime,size,docid) VALUES(?1,?2,?3,?4) "
         "ON CONFLICT(relpath) DO UPDATE SET mtime=?2,size=?3,docid=?4");
  s.bind(1, rel_path).bind(2, mtime).bind(3, size).bind(4, doc_id);
  s.exec();
}

std::vector<std::string> Manifest::paths_of(const std::string& doc_id) {
  Stmt s(db_, "SELECT relpath FROM path WHERE docid=?1 ORDER BY relpath");
  s.bind(1, doc_id);
  std::vector<std::string> out;
  while (s.row()) out.push_back(s.col_text(0));
  return out;
}

bool Manifest::has_doc(const std::string& doc_id) {
  Stmt s(db_, "SELECT 1 FROM doc WHERE docid=?1");
  s.bind(1, doc_id);
  return s.row();
}

void Manifest::insert_doc(const std::string& doc_id, int64_t bytes, const std::string& mime) {
  Stmt s(db_,
         "INSERT OR IGNORE INTO doc(docid,bytes,mime,stage,ingested_at) "
         "VALUES(?1,?2,?3,0,strftime('%Y-%m-%dT%H:%M:%SZ','now'))");
  s.bind(1, doc_id).bind(2, bytes).bind(3, mime);
  s.exec();
}

void Manifest::set_stage(const std::string& doc_id, Stage st) {
  Stmt s(db_, "UPDATE doc SET stage=?2 WHERE docid=?1");
  s.bind(1, doc_id).bind(2, static_cast<int64_t>(st));
  s.exec();
}

std::optional<int> Manifest::stage_of(const std::string& doc_id) {
  Stmt s(db_, "SELECT stage FROM doc WHERE docid=?1");
  s.bind(1, doc_id);
  if (!s.row()) return std::nullopt;
  return static_cast<int>(s.col_i64(0));
}

void Manifest::supersede(const std::string& old_id, const std::string& new_id) {
  Stmt s(db_, "UPDATE doc SET superseded_by=?2 WHERE docid=?1");
  s.bind(1, old_id).bind(2, new_id);
  s.exec();
}

std::vector<std::string> Manifest::superseded_docs() {
  Stmt s(db_, "SELECT docid FROM doc WHERE superseded_by IS NOT NULL AND vacuumed=0");
  std::vector<std::string> out;
  while (s.row()) out.push_back(s.col_text(0));
  return out;
}

void Manifest::forget_doc(const std::string& doc_id) {
  { Stmt s(db_, "DELETE FROM chunk WHERE docid=?1"); s.bind(1, doc_id); s.exec(); }
  { Stmt s(db_, "DELETE FROM path  WHERE docid=?1"); s.bind(1, doc_id); s.exec(); }
  { Stmt s(db_, "UPDATE doc SET vacuumed=1 WHERE docid=?1"); s.bind(1, doc_id); s.exec(); }
}

void Manifest::insert_chunk(const ChunkRow& c) {
  Stmt s(db_,
         "INSERT OR REPLACE INTO chunk(chunkid,docid,ordinal,veckey,byte_start,byte_end) "
         "VALUES(?1,?2,?3,?4,?5,?6)");
  s.bind(1, c.chunk_id).bind(2, c.doc_id).bind(3, static_cast<int64_t>(c.ordinal));
  s.bind_u64(4, c.vec_key);
  s.bind(5, c.byte_start).bind(6, c.byte_end);
  s.exec();
}

std::vector<ChunkRow> Manifest::chunks_of(const std::string& doc_id) {
  Stmt s(db_,
         "SELECT chunkid,docid,ordinal,veckey,byte_start,byte_end FROM chunk "
         "WHERE docid=?1 ORDER BY ordinal");
  s.bind(1, doc_id);
  std::vector<ChunkRow> out;
  while (s.row()) {
    ChunkRow c;
    c.chunk_id = s.col_text(0);
    c.doc_id = s.col_text(1);
    c.ordinal = static_cast<int>(s.col_i64(2));
    c.vec_key = static_cast<uint64_t>(s.col_i64(3));
    c.byte_start = s.col_i64(4);
    c.byte_end = s.col_i64(5);
    out.push_back(std::move(c));
  }
  return out;
}

std::optional<std::string> Manifest::chunk_by_vec_key(uint64_t key) {
  Stmt s(db_, "SELECT chunkid FROM chunk WHERE veckey=?1");
  s.bind_u64(1, key);
  if (!s.row()) return std::nullopt;
  return s.col_text(0);
}

void Manifest::set_meta(const std::string& key, const std::string& value) {
  Stmt s(db_, "INSERT OR REPLACE INTO meta(key,value) VALUES(?1,?2)");
  s.bind(1, key).bind(2, value);
  s.exec();
}

std::optional<std::string> Manifest::get_meta(const std::string& key) {
  Stmt s(db_, "SELECT value FROM meta WHERE key=?1");
  s.bind(1, key);
  if (!s.row()) return std::nullopt;
  return s.col_text(0);
}

Scoreboard Manifest::scoreboard() {
  Scoreboard sc;
  {
    Stmt s(db_, "SELECT COUNT(*) FROM doc WHERE superseded_by IS NULL");
    if (s.row()) sc.docs = s.col_i64(0);
  }
  {
    Stmt s(db_, "SELECT COUNT(*) FROM chunk");
    if (s.row()) sc.chunks = s.col_i64(0);
  }
  {
    Stmt s(db_, "SELECT COUNT(*) FROM doc WHERE superseded_by IS NOT NULL AND vacuumed=0");
    if (s.row()) sc.superseded = s.col_i64(0);
  }
  return sc;
}

void Manifest::begin() {
  char* e = nullptr;
  if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &e) != SQLITE_OK) fail(db_, "begin");
}
void Manifest::commit() {
  char* e = nullptr;
  if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &e) != SQLITE_OK) fail(db_, "commit");
}
void Manifest::rollback() {
  sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
}

}  // namespace chimera
