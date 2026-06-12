// §5 Stage 3 — structure-aware chunking with byte offsets.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace chimera {

struct Chunk {
  int ordinal = 1;        // 1-based, matching chunkID {docID}:{0001}
  int64_t byte_start = 0; // offsets into the original UTF-8 text
  int64_t byte_end = 0;   // exclusive
  std::string text;       // chunk text (= original bytes + leading overlap)
};

struct ChunkOptions {
  // Token counts are estimated at ~4 bytes/token (close enough for budget
  // purposes; the embedding server re-tokenizes anyway).
  int target_tokens = 768;   // middle of the 512–1024 band
  int max_tokens = 1024;
  double overlap = 0.15;
};

// Splits text into chunks along structural boundaries: markdown headings and
// blank lines for prose, top-level definition starts for code (mime
// text/x-*), never mid-line, and avoids mid-sentence splits where a sentence
// boundary is available. Adjacent chunks share ~15% overlap: each chunk after
// the first is prefixed with the tail of its predecessor; byte_start/byte_end
// always describe the non-overlapped span, so offsets tile the document.
std::vector<Chunk> chunk_text(const std::string& text, const std::string& mime,
                              const ChunkOptions& opts = {});

// HTML → text (v1: tag stripping + entity decode for the common five).
std::string strip_html(const std::string& html);

}  // namespace chimera
