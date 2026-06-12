// Small shared helpers. Everything here is header-only and dependency-free.
#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace chimera {

inline std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline std::string iso8601_now() {
  using namespace std::chrono;
  auto t = system_clock::to_time_t(system_clock::now());
  char buf[32];
  std::strftime(buf, sizeof buf, "%FT%TZ", std::gmtime(&t));
  return buf;
}

inline std::string to_hex(const uint8_t* data, size_t n) {
  static const char* k = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(k[data[i] >> 4]);
    out.push_back(k[data[i] & 0xf]);
  }
  return out;
}

// NFC is overkill for v1 slugs (we fold to ASCII anyway); §4 pipeline:
// lowercase → trim → collapse whitespace → kebab-case. Non-alnum runs become
// single hyphens; leading/trailing hyphens dropped.
inline std::string slugify(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool pending_dash = false;
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      if (pending_dash && !out.empty()) out.push_back('-');
      pending_dash = false;
      out.push_back(static_cast<char>(std::tolower(c)));
    } else {
      pending_dash = true;  // any separator / punctuation / non-ascii byte
    }
  }
  return out;
}

inline std::string chunk_id(const std::string& doc_id, int ordinal) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d", ordinal);
  return doc_id + ":" + buf;
}

}  // namespace chimera
