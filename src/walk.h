// §5 Stage 1 — corpus walker: .chimeraignore + sane defaults, UTF-8 text only.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace chimera {

struct WalkEntry {
  std::string rel_path;   // relative to the walk root, '/'-separated
  std::string abs_path;
  int64_t size = 0;
  int64_t mtime = 0;      // unix seconds
  std::string mime;       // best-effort from extension ("text/plain" fallback)
};

struct WalkOptions {
  std::vector<std::string> include;  // glob patterns; empty = all
  std::vector<std::string> exclude;  // glob patterns, additive to defaults
  std::string db_dir_name = ".chimera";
};

// Walks root depth-first, applying, in order:
//   1. default skips: .git, node_modules, the db dir, hidden dirs
//   2. .chimeraignore at the root (gitignore-lite: '#' comments, '*'/'?'
//      globs, trailing-'/' = directory pattern, leading '!' unsupported in v1)
//   3. --exclude then --include globs
//   4. binary sniff: extension blocklist, then NUL byte / invalid-UTF-8 check
//      on the first 8 KiB
// Calls cb for every surviving file. Deterministic (sorted) order.
void walk(const std::string& root, const WalkOptions& opts,
          const std::function<void(const WalkEntry&)>& cb);

// Exposed for tests.
bool glob_match(const std::string& pattern, const std::string& path);
bool looks_binary(const char* data, size_t n);
std::string mime_of(const std::string& path);

}  // namespace chimera
