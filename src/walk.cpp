#include "walk.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace chimera {

namespace {

const char* kSkipDirs[] = {".git", "node_modules", "__pycache__", ".hg", ".svn",
                           "target", "build", ".venv", "venv"};

// Extensions we refuse without sniffing (well-known binary formats).
const char* kBinaryExt[] = {
    "png", "jpg", "jpeg", "gif", "webp", "ico", "bmp", "tiff", "pdf",  "zip",
    "gz",  "bz2", "xz",   "zst", "tar",  "7z",  "rar", "exe", "dll",  "so",
    "dylib", "a", "o",    "bin", "dat",  "db",  "sqlite", "wasm", "mp3", "mp4",
    "wav", "ogg", "flac", "avi", "mkv",  "mov", "ttf", "otf", "woff", "woff2",
    "gguf", "ape", "tvim", "pyc", "class", "jar"};

const std::map<std::string, std::string> kMime = {
    {"md", "text/markdown"},   {"markdown", "text/markdown"},
    {"html", "text/html"},     {"htm", "text/html"},
    {"csv", "text/csv"},       {"json", "application/json"},
    {"txt", "text/plain"},     {"py", "text/x-python"},
    {"c", "text/x-c"},         {"h", "text/x-c"},
    {"cpp", "text/x-c++"},     {"hpp", "text/x-c++"},
    {"cc", "text/x-c++"},      {"rs", "text/x-rust"},
    {"go", "text/x-go"},       {"js", "text/x-javascript"},
    {"ts", "text/x-typescript"}, {"java", "text/x-java"},
    {"sh", "text/x-shellscript"}, {"toml", "text/x-toml"},
    {"yaml", "text/x-yaml"},   {"yml", "text/x-yaml"},
    {"xml", "text/xml"},       {"sql", "text/x-sql"},
    {"rb", "text/x-ruby"},     {"tex", "text/x-tex"}};

std::string ext_of(const std::string& path) {
  auto dot = path.find_last_of('.');
  auto slash = path.find_last_of('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
  std::string e = path.substr(dot + 1);
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
  return e;
}

}  // namespace

std::string mime_of(const std::string& path) {
  auto it = kMime.find(ext_of(path));
  return it != kMime.end() ? it->second : "text/plain";
}

namespace {
// Recursive two-pointer matcher: '*' = any run not crossing '/', '**' = any
// run including '/', '?' = one char. Patterns and paths are short.
bool glob_core(const std::string& pat, size_t pi, const std::string& s, size_t si) {
  while (pi < pat.size()) {
    char pc = pat[pi];
    if (pc == '*') {
      bool dbl = pi + 1 < pat.size() && pat[pi + 1] == '*';
      size_t next = pi + (dbl ? 2 : 1);
      for (size_t k = si; k <= s.size(); ++k) {
        if (glob_core(pat, next, s, k)) return true;
        if (k < s.size() && !dbl && s[k] == '/') return false;
      }
      return false;
    }
    if (si >= s.size()) return false;
    if (pc != '?' && pc != s[si]) return false;
    ++pi, ++si;
  }
  return si == s.size();
}
}  // namespace

bool glob_match(const std::string& pattern, const std::string& path) {
  if (glob_core(pattern, 0, path, 0)) return true;
  // A bare-name pattern (no '/') also matches any single path component,
  // mirroring gitignore: "*.log" ignores logs at every depth.
  if (pattern.find('/') == std::string::npos) {
    size_t start = 0;
    while (start <= path.size()) {
      size_t end = path.find('/', start);
      size_t len = (end == std::string::npos ? path.size() : end) - start;
      if (glob_core(pattern, 0, path.substr(start, len), 0)) return true;
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  return false;
}

bool looks_binary(const char* data, size_t n) {
  if (memchr(data, '\0', n) != nullptr) return true;
  // UTF-8 validity scan; reject if >0 invalid sequences.
  size_t i = 0;
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(data[i]);
    size_t need = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xe ? 2 : (c >> 3) == 0x1e ? 3 : SIZE_MAX;
    if (need == SIZE_MAX) return true;
    if (i + need >= n && need > 0) break;  // truncated tail at sniff boundary: fine
    for (size_t k = 1; k <= need; ++k)
      if ((static_cast<unsigned char>(data[i + k]) >> 6) != 0x2) return true;
    i += need + 1;
  }
  return false;
}

namespace {

struct IgnoreRules {
  std::vector<std::string> patterns;       // file patterns
  std::vector<std::string> dir_patterns;   // trailing-'/' patterns

  bool ignored(const std::string& rel, bool is_dir) const {
    for (const auto& p : patterns)
      if (glob_match(p, rel)) return true;
    if (is_dir)
      for (const auto& p : dir_patterns)
        if (glob_match(p, rel)) return true;
    return false;
  }
};

IgnoreRules load_ignore(const std::string& root) {
  IgnoreRules r;
  std::ifstream in(root + "/.chimeraignore");
  std::string line;
  while (std::getline(in, line)) {
    // strip comments and whitespace
    auto hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    size_t b = 0;
    while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
    line.erase(0, b);
    if (line.empty()) continue;
    if (line.back() == '/') {
      line.pop_back();
      r.dir_patterns.push_back(line);
    } else {
      r.patterns.push_back(line);
    }
  }
  return r;
}

bool is_binary_ext(const std::string& path) {
  std::string e = ext_of(path);
  for (const char* b : kBinaryExt)
    if (e == b) return true;
  return false;
}

}  // namespace

void walk(const std::string& root, const WalkOptions& opts,
          const std::function<void(const WalkEntry&)>& cb) {
  IgnoreRules ignore = load_ignore(root);
  fs::path rootp(root);

  std::vector<fs::path> stack{rootp};
  while (!stack.empty()) {
    fs::path dir = stack.back();
    stack.pop_back();

    std::vector<fs::directory_entry> entries;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
      entries.push_back(*it);
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.path() < b.path(); });
    // LIFO stack + sorted ascending = subdirs visited in reverse; push
    // reversed so traversal stays lexicographic.
    std::vector<fs::path> subdirs;

    for (const auto& de : entries) {
      std::error_code sec;
      std::string rel = fs::relative(de.path(), rootp, sec).generic_string();
      std::string name = de.path().filename().string();

      if (de.is_directory(sec)) {
        bool skip = false;
        if (!name.empty() && name[0] == '.') skip = true;  // hidden dirs (incl. db dir)
        for (const char* d : kSkipDirs)
          if (name == d) skip = true;
        if (name == opts.db_dir_name) skip = true;
        if (ignore.ignored(rel, /*is_dir=*/true)) skip = true;
        if (!skip) subdirs.push_back(de.path());
        continue;
      }
      if (!de.is_regular_file(sec)) continue;
      if (!name.empty() && name[0] == '.' && name != ".chimeraignore") continue;
      if (name == ".chimeraignore") continue;
      if (ignore.ignored(rel, false)) continue;

      for (const auto& p : opts.exclude)
        if (glob_match(p, rel)) goto next_entry;
      if (!opts.include.empty()) {
        bool hit = false;
        for (const auto& p : opts.include)
          if (glob_match(p, rel)) hit = true;
        if (!hit) continue;
      }

      if (is_binary_ext(rel)) continue;
      {
        std::ifstream f(de.path(), std::ios::binary);
        char buf[8192];
        f.read(buf, sizeof buf);
        std::streamsize got = f.gcount();
        if (got > 0 && looks_binary(buf, static_cast<size_t>(got))) continue;
      }

      {
        WalkEntry e;
        e.rel_path = rel;
        e.abs_path = de.path().string();
        e.size = static_cast<int64_t>(de.file_size(sec));
        struct stat st {};
        if (::stat(e.abs_path.c_str(), &st) == 0) e.mtime = st.st_mtime;
        e.mime = mime_of(rel);
        cb(e);
      }
    next_entry:;
    }

    for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(*it);
  }
}

}  // namespace chimera
