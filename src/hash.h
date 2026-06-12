// §2.1 — the triple-tap checksum: document identity from a sampled hash.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace chimera {

// docID = hex(xxh3_128(filesize ‖ head 64KiB ‖ middle 64KiB ‖ tail 64KiB)),
// truncated to 16 hex chars. Files < 192 KiB are hashed whole. O(1) in file
// size: three seeks and a hash.
//
// paranoid=true substitutes a full BLAKE3 over the entire content (same
// 16-hex-char truncation) for users who don't trust their middles (§2.1).
// Returns nullopt if the file cannot be read.
std::optional<std::string> triple_tap(const std::string& path, bool paranoid = false);

// xxh3_64 of a string — used to derive the TurboVec u64 key from a chunkID
// (see docs/DECISIONS.md D1).
uint64_t vec_key(const std::string& chunk_id);

}  // namespace chimera
