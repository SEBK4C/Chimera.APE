// CLI-facing entry points for the two pipelines and the small commands.
#pragma once

#include <string>
#include <vector>

namespace chimera {

struct Options {
  std::string dir;          // ingest corpus root
  std::string query;        // --search text
  std::string search_file;  // --search-file: query by image/audio file
  std::string sparql_text;  // sparql subcommand
  std::string db;           // --db override (else <dir>/.chimera)
  std::string model;        // --model override
  std::vector<std::string> include, exclude;
  bool paranoid = false;
  bool fix = false;
  bool json = false;
  bool verbose = false;
  int k = 24;
  int hops = 1;
  int ctx_budget = 8192;
};

int run_ingest(const Options& o);
int run_search(const Options& o);
int run_status(const Options& o);
int run_verify(const Options& o);
int run_vacuum(const Options& o);
int run_sparql(const Options& o);

// Shared: resolve the db dir (--db, else <dir>/.chimera, else ./.chimera).
std::string resolve_db_dir(const Options& o);

}  // namespace chimera
