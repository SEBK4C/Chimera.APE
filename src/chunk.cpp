#include "chunk.h"

#include <strings.h>

#include <algorithm>
#include <cctype>

namespace chimera {

namespace {

constexpr int kBytesPerToken = 4;

bool starts_with(const std::string& s, size_t at, const char* prefix) {
  return s.compare(at, std::char_traits<char>::length(prefix), prefix) == 0;
}

// A "segment" is the atomic unit chunks are assembled from: prose paragraphs
// or code blocks delimited by structural boundaries. Splitting only ever
// happens between segments (or inside one that alone exceeds max).
struct Segment {
  size_t begin, end;  // byte span, end exclusive
};

bool is_code(const std::string& mime) {
  return mime.rfind("text/x-", 0) == 0 || mime == "application/json";
}

// Top-level definition heuristic for code: a non-indented line that opens a
// function/class/struct-ish construct.
bool code_boundary(const std::string& t, size_t line_start) {
  if (line_start >= t.size()) return false;
  char c = t[line_start];
  if (c == ' ' || c == '\t' || c == '\n' || c == '}' || c == ')') return false;
  static const char* kw[] = {"def ", "class ", "fn ", "func ", "function ", "impl ",
                             "struct ", "enum ", "trait ", "pub ", "static ",
                             "void ", "int ", "bool ", "double ", "const ",
                             "template", "namespace ", "export ", "async "};
  for (const char* k : kw)
    if (starts_with(t, line_start, k)) return true;
  return false;
}

// Split text into segments at structural boundaries.
std::vector<Segment> segment(const std::string& t, const std::string& mime) {
  std::vector<Segment> segs;
  bool code = is_code(mime);
  size_t seg_start = 0;
  size_t i = 0;
  auto flush = [&](size_t end) {
    if (end > seg_start) segs.push_back({seg_start, end});
    seg_start = end;
  };
  while (i < t.size()) {
    size_t line_start = i;
    size_t nl = t.find('\n', i);
    if (nl == std::string::npos) nl = t.size();
    i = nl + (nl < t.size() ? 1 : 0);

    if (code) {
      if (line_start > seg_start && code_boundary(t, line_start)) flush(line_start);
      continue;
    }
    // Prose: markdown heading starts a new segment...
    if (line_start > seg_start && t[line_start] == '#') flush(line_start);
    // ...and a blank line ends one (the blank line stays with the segment
    // before it, keeping offsets tiling).
    bool blank = true;
    for (size_t k = line_start; k < nl; ++k)
      if (!std::isspace(static_cast<unsigned char>(t[k]))) {
        blank = false;
        break;
      }
    if (blank && i > seg_start) flush(i);
  }
  flush(t.size());
  return segs;
}

// Find a sentence-ish boundary in [lo, hi) closest below hi; npos if none.
size_t sentence_break(const std::string& t, size_t lo, size_t hi) {
  for (size_t k = hi; k > lo + 1; --k) {
    char c = t[k - 1];
    if ((c == '.' || c == '!' || c == '?' || c == '\n') &&
        (k >= t.size() || std::isspace(static_cast<unsigned char>(t[k]))))
      return k;
  }
  return std::string::npos;
}

}  // namespace

std::vector<Chunk> chunk_text(const std::string& text, const std::string& mime,
                              const ChunkOptions& opts) {
  std::vector<Chunk> out;
  if (text.empty()) return out;

  const size_t target = static_cast<size_t>(opts.target_tokens) * kBytesPerToken;
  const size_t max = static_cast<size_t>(opts.max_tokens) * kBytesPerToken;
  const size_t overlap_bytes = static_cast<size_t>(target * opts.overlap);

  std::vector<Segment> segs = segment(text, mime);

  // Oversized single segments get hard-split at sentence boundaries (or max).
  std::vector<Segment> units;
  for (const Segment& s : segs) {
    size_t b = s.begin;
    while (s.end - b > max) {
      size_t cut = sentence_break(text, b, b + max);
      if (cut == std::string::npos || cut <= b) cut = b + max;
      units.push_back({b, cut});
      b = cut;
    }
    if (s.end > b) units.push_back({b, s.end});
  }

  // Greedy accumulation of units into chunks around the target size.
  size_t cur_begin = units.empty() ? 0 : units[0].begin;
  size_t cur_end = cur_begin;
  auto emit = [&]() {
    if (cur_end <= cur_begin) return;
    Chunk c;
    c.ordinal = static_cast<int>(out.size()) + 1;
    c.byte_start = static_cast<int64_t>(cur_begin);
    c.byte_end = static_cast<int64_t>(cur_end);
    size_t ov_start = cur_begin;
    if (!out.empty() && overlap_bytes > 0) {
      ov_start = cur_begin > overlap_bytes ? cur_begin - overlap_bytes : 0;
      // Don't start overlap mid-UTF-8-sequence.
      while (ov_start < cur_begin &&
             (static_cast<unsigned char>(text[ov_start]) & 0xc0) == 0x80)
        ++ov_start;
    }
    c.text = text.substr(ov_start, cur_end - ov_start);
    out.push_back(std::move(c));
  };

  for (const Segment& u : units) {
    size_t cur_size = cur_end - cur_begin;
    size_t u_size = u.end - u.begin;
    if (cur_size > 0 && cur_size + u_size > target && cur_size >= target / 2) {
      emit();
      cur_begin = u.begin;
    }
    cur_end = u.end;
    if (cur_end - cur_begin >= max) {
      emit();
      cur_begin = cur_end;
    }
  }
  emit();
  return out;
}

std::string strip_html(const std::string& html) {
  std::string out;
  out.reserve(html.size());
  bool in_tag = false;
  bool in_script = false;
  size_t i = 0;
  while (i < html.size()) {
    if (!in_tag && html[i] == '<') {
      // script/style bodies are dropped wholesale
      auto tag_is = [&](const char* name) {
        size_t n = std::char_traits<char>::length(name);
        return html.size() - i > n + 1 &&
               strncasecmp(html.data() + i + 1, name, n) == 0;
      };
      if (tag_is("script") || tag_is("style")) in_script = true;
      if (in_script && i + 1 < html.size() && html[i + 1] == '/') in_script = false;
      in_tag = true;
      ++i;
      continue;
    }
    if (in_tag) {
      if (html[i] == '>') {
        in_tag = false;
        out.push_back(' ');
      }
      ++i;
      continue;
    }
    if (in_script) {
      ++i;
      continue;
    }
    if (html[i] == '&') {
      struct {
        const char* ent;
        char ch;
      } static const kEnt[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                               {"&quot;", '"'}, {"&#39;", '\''}, {"&nbsp;", ' '}};
      bool hit = false;
      for (const auto& e : kEnt) {
        size_t n = std::char_traits<char>::length(e.ent);
        if (html.compare(i, n, e.ent) == 0) {
          out.push_back(e.ch);
          i += n;
          hit = true;
          break;
        }
      }
      if (hit) continue;
    }
    out.push_back(html[i]);
    ++i;
  }
  return out;
}

}  // namespace chimera
