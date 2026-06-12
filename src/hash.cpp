#include "hash.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include "xxhash/xxhash.h"

#include "blake3.h"
#include "util.h"

namespace chimera {

namespace {
constexpr size_t kTap = 64 * 1024;          // one tap
constexpr size_t kWholeLimit = 3 * kTap;    // 192 KiB

// Read exactly n bytes at offset off; returns false on short read.
bool read_at(std::FILE* f, long long off, void* buf, size_t n) {
#if defined(_WIN32)
  if (_fseeki64(f, off, SEEK_SET) != 0) return false;
#else
  if (std::fseek(f, static_cast<long>(off), SEEK_SET) != 0) return false;
#endif
  return std::fread(buf, 1, n, f) == n;
}
}  // namespace

std::optional<std::string> triple_tap(const std::string& path, bool paranoid) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return std::nullopt;
  struct Closer {
    std::FILE* f;
    ~Closer() { std::fclose(f); }
  } closer{f};

  if (std::fseek(f, 0, SEEK_END) != 0) return std::nullopt;
  long long size = std::ftell(f);
  if (size < 0) return std::nullopt;

  if (paranoid) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    std::vector<uint8_t> buf(1 << 16);
    std::fseek(f, 0, SEEK_SET);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
      blake3_hasher_update(&h, buf.data(), n);
    uint8_t out[8];
    blake3_hasher_finalize(&h, out, sizeof out);
    return to_hex(out, sizeof out);
  }

  XXH3_state_t* st = XXH3_createState();
  XXH3_128bits_reset(st);
  uint64_t sz = static_cast<uint64_t>(size);
  XXH3_128bits_update(st, &sz, sizeof sz);

  std::vector<uint8_t> buf(kTap);
  if (static_cast<size_t>(size) <= kWholeLimit) {
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> whole(static_cast<size_t>(size));
    if (size > 0 && std::fread(whole.data(), 1, whole.size(), f) != whole.size()) {
      XXH3_freeState(st);
      return std::nullopt;
    }
    XXH3_128bits_update(st, whole.data(), whole.size());
  } else {
    long long mid = size / 2 - static_cast<long long>(kTap) / 2;
    long long offs[3] = {0, mid, size - static_cast<long long>(kTap)};
    for (long long off : offs) {
      if (!read_at(f, off, buf.data(), kTap)) {
        XXH3_freeState(st);
        return std::nullopt;
      }
      XXH3_128bits_update(st, buf.data(), kTap);
    }
  }

  XXH128_hash_t h = XXH3_128bits_digest(st);
  XXH3_freeState(st);
  // 16 hex chars "is plenty" (§2.2): the high 64 bits of the 128-bit hash.
  uint8_t bytes[8];
  for (int i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(h.high64 >> (56 - 8 * i));
  return to_hex(bytes, sizeof bytes);
}

uint64_t vec_key(const std::string& chunk_id) {
  return XXH3_64bits(chunk_id.data(), chunk_id.size());
}

}  // namespace chimera
